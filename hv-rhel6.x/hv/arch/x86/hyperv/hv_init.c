/*
 * X86 specific Hyper-V initialization code.
 *
 * Copyright (C) 2016, Microsoft, Inc.
 *
 * Author : K. Y. Srinivasan <kys@microsoft.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 */

#include <linux/types.h>
#include <linux/module.h>
#include <lis/asm/hyperv.h>
#include <lis/asm/mshyperv.h>
#include <asm/hypervisor.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/io.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18)
#include <linux/clockchips.h>
#endif

#define HV_CLOCK_SHIFT  22

#ifdef CONFIG_X86_64

static struct ms_hyperv_tsc_page *tsc_pg;

static u64 read_hv_clock_tsc(struct clocksource *arg)
{
       u64 current_tick;

       if (tsc_pg->tsc_sequence != 0) {
               /*
                * Use the tsc page to compute the value.
                */

               while (1) {
                       u64 tmp;
                       u32 sequence = tsc_pg->tsc_sequence;
                       u64 cur_tsc;
                       u64 scale = tsc_pg->tsc_scale;
                       s64 offset = tsc_pg->tsc_offset;

                       rdtscll(cur_tsc);
                       /* current_tick = ((cur_tsc *scale) >> 64) + offset */
                       asm("mulq %3"
                               : "=d" (current_tick), "=a" (tmp)
                               : "a" (cur_tsc), "r" (scale));

                       current_tick += offset;
                       if (tsc_pg->tsc_sequence == sequence)
                               return current_tick;

                       if (tsc_pg->tsc_sequence != 0)
                               continue;
                       /*
                        * Fallback using MSR method.
                        */
                       break;
               }
       }
       rdmsrl(HV_X64_MSR_TIME_REF_COUNT, current_tick);
       return current_tick;
}

static struct clocksource hyperv_cs_tsc = {
	.name           = "hyperv_clocksource_tsc_page",
	.rating         = 425,
	.read           = read_hv_clock_tsc,
	.mask           = CLOCKSOURCE_MASK(64),
	.flags          = CLOCK_SOURCE_IS_CONTINUOUS,
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6,3))
	.mult           = (100 << HV_CLOCK_SHIFT),
	.shift          = HV_CLOCK_SHIFT,
#endif
};
#endif

static u64 read_hv_clock_msr(struct clocksource *arg)
{
       u64 current_tick;
       /*
        * Read the partition counter to get the current tick count. This count
        * is set to 0 when the partition is created and is incremented in
        * 100 nanosecond units.
        */
       rdmsrl(HV_X64_MSR_TIME_REF_COUNT, current_tick);
       return current_tick;
}

static struct clocksource hyperv_cs_msr = {
	.name		= "hyperv_clocksource_msr",
	.rating		= 425,
	.read		= read_hv_clock_msr,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6,3))
	.mult           = (100 << HV_CLOCK_SHIFT),
	.shift          = HV_CLOCK_SHIFT,
#endif
};

static void *hypercall_pg;
/*
 * This function is to be invoked early in the boot sequence after the
 * hypervisor has been detected.
 *
 * 1. Setup the hypercall page.
 * 2. Register Hyper-V specific clocksource.
 */
void hyperv_init(void)
{
	u64 guest_id;
	union hv_x64_msr_hypercall_contents hypercall_msr;

#if (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(6,4))
	if (x86_hyper != &x86_hyper_ms_hyperv)
		return;
#endif

	/*
	 * Setup the hypercall page and enable hypercalls.
	 * 1. Register the guest ID
	 * 2. Enable the hypercall and register the hypercall page
	 */
	guest_id = generate_guest_id(0, LINUX_VERSION_CODE, 0);
	wrmsrl(HV_X64_MSR_GUEST_OS_ID, guest_id);

#ifdef CONFIG_X86_64
	hypercall_pg  = __vmalloc(PAGE_SIZE, GFP_KERNEL, PAGE_KERNEL_RX);
#else
	hypercall_pg = __vmalloc(PAGE_SIZE, GFP_KERNEL,
                             __pgprot(__PAGE_KERNEL & (~_PAGE_NX)));
#endif
	if (hypercall_pg == NULL) {
		wrmsrl(HV_X64_MSR_GUEST_OS_ID, 0);
		return;
	}

	rdmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr.as_uint64);
	hypercall_msr.enable = 1;
	hypercall_msr.guest_physical_address = vmalloc_to_pfn(hypercall_pg);
	wrmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr.as_uint64);

	/*
	 * Register Hyper-V specific clocksource.
	 */
#ifdef CONFIG_X86_64
	if (ms_hyperv.features & HV_X64_MSR_REFERENCE_TSC_AVAILABLE) {
		union hv_x64_msr_hypercall_contents tsc_msr;

		tsc_pg = __vmalloc(PAGE_SIZE, GFP_KERNEL, PAGE_KERNEL);
		if (!tsc_pg) {
			clocksource_register_hz(&hyperv_cs_msr, NSEC_PER_SEC/100);
			return;
		}

		rdmsrl(HV_X64_MSR_REFERENCE_TSC, tsc_msr.as_uint64);

		tsc_msr.enable = 1;
		tsc_msr.guest_physical_address = vmalloc_to_pfn(tsc_pg);

		wrmsrl(HV_X64_MSR_REFERENCE_TSC, tsc_msr.as_uint64);
#if  (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6,3))
		clocksource_register(&hyperv_cs_tsc);
#else
		clocksource_register_hz(&hyperv_cs_tsc, NSEC_PER_SEC/100);
#endif
		return;
	}
#endif
	/*
	 * For 32 bit guests just use the MSR based mechanism for reading
	 * the partition counter.
	 */

	if (ms_hyperv.features & HV_X64_MSR_TIME_REF_COUNT_AVAILABLE)
#if  (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6,3))
		clocksource_register(&hyperv_cs_msr);
#else
		clocksource_register_hz(&hyperv_cs_msr, NSEC_PER_SEC/100);
#endif
}

/*
 * This routine is called before kexec/kdump, it does the required cleanup.
 */
void hyperv_cleanup(void)
{
	union hv_x64_msr_hypercall_contents hypercall_msr;

	/* Reset our OS id */
	wrmsrl(HV_X64_MSR_GUEST_OS_ID, 0);

	/* Reset the hypercall page */
	hypercall_msr.as_uint64 = 0;
	wrmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr.as_uint64);

	/* Reset the TSC page */
	hypercall_msr.as_uint64 = 0;
	wrmsrl(HV_X64_MSR_REFERENCE_TSC, hypercall_msr.as_uint64);
}
EXPORT_SYMBOL_GPL(hyperv_cleanup);

/*
 * hv_do_hypercall- Invoke the specified hypercall
 */
u64 hv_do_hypercall(u64 control, void *input, void *output)
{
	u64 input_address = (input) ? virt_to_phys(input) : 0;
	u64 output_address = (output) ? virt_to_phys(output) : 0;
#ifdef CONFIG_X86_64
	u64 hv_status = 0;

	if (!hypercall_pg)
		return (u64)ULLONG_MAX;

	__asm__ __volatile__("mov %0, %%r8" : : "r" (output_address) : "r8");
	__asm__ __volatile__("call *%3" : "=a" (hv_status) :
			     "c" (control), "d" (input_address),
			     "m" (hypercall_pg));

	return hv_status;

#else

	u32 control_hi = control >> 32;
	u32 control_lo = control & 0xFFFFFFFF;
	u32 hv_status_hi = 1;
	u32 hv_status_lo = 1;
	u32 input_address_hi = input_address >> 32;
	u32 input_address_lo = input_address & 0xFFFFFFFF;
	u32 output_address_hi = output_address >> 32;
	u32 output_address_lo = output_address & 0xFFFFFFFF;

	if (!hypercall_pg)
		return (u64)ULLONG_MAX;

	__asm__ __volatile__ ("call *%8" : "=d"(hv_status_hi),
			      "=a"(hv_status_lo) : "d" (control_hi),
			      "a" (control_lo), "b" (input_address_hi),
			      "c" (input_address_lo), "D"(output_address_hi),
			      "S"(output_address_lo), "m" (hypercall_pg));

	return hv_status_lo | ((u64)hv_status_hi << 32);
#endif /* !x86_64 */
}
EXPORT_SYMBOL_GPL(hv_do_hypercall);

void hyperv_report_panic(struct pt_regs *regs)
{
	wrmsrl(HV_X64_MSR_CRASH_P0, regs->ip);
	wrmsrl(HV_X64_MSR_CRASH_P1, regs->ax);
	wrmsrl(HV_X64_MSR_CRASH_P2, regs->bx);
	wrmsrl(HV_X64_MSR_CRASH_P3, regs->cx);
	wrmsrl(HV_X64_MSR_CRASH_P4, regs->dx);

	/*
	 * Let Hyper-V know there is crash data available
	 */
	wrmsrl(HV_X64_MSR_CRASH_CTL, HV_CRASH_CTL_CRASH_NOTIFY);
}
EXPORT_SYMBOL_GPL(hyperv_report_panic);

bool hv_is_hypercall_page_setup(void)
{
	union hv_x64_msr_hypercall_contents hypercall_msr;

	/* Check if the hypercall page is setup */
	hypercall_msr.as_uint64 = 0;
	rdmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr.as_uint64);

	if (!hypercall_msr.enable)
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(hv_is_hypercall_page_setup);

void hv_print_host_info(void) {
	int hv_host_info_eax;
	int hv_host_info_ebx;
	int hv_host_info_ecx;
	int hv_host_info_edx;

	/*
	 * Extract host information.
	 */
	if (cpuid_eax(HVCPUID_VENDOR_MAXFUNCTION) >= HVCPUID_VERSION) {
		hv_host_info_eax = cpuid_eax(HVCPUID_VERSION);
		hv_host_info_ebx = cpuid_ebx(HVCPUID_VERSION);
		hv_host_info_ecx = cpuid_ecx(HVCPUID_VERSION);
		hv_host_info_edx = cpuid_edx(HVCPUID_VERSION);

		pr_info("Hyper-V Host Build:%d-%d.%d-%d-%d.%d\n",
			hv_host_info_eax, hv_host_info_ebx >> 16,
			hv_host_info_ebx & 0xFFFF, hv_host_info_ecx,
			hv_host_info_edx >> 24, hv_host_info_edx & 0xFFFFFF);
	}
}
EXPORT_SYMBOL_GPL(hv_print_host_info);
