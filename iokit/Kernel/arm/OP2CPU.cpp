/*
 * Copyright (c) 2026. Port for OnePlus 2 (Qualcomm Snapdragon 810 / MSM8994).
 */

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <arm/machine_routines.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "OP2CPU.h"
#include "OP2InterruptController.h"

/* Minimal mirrors of osfmk/arm/cpu_data_internal.h layouts (that header is
 * not exported to iokit). Layouts verified against the source. */
struct op2_cpu_data_entry {
	void *cpu_data_paddr;
	void *cpu_data_vaddr;
};
struct op2_reset_handler_data {
	vm_offset_t assist_reset_handler;
	vm_offset_t cpu_data_entries;
};

#define super IOCPU

OSDefineMetaClassAndStructors(OP2CPU, IOCPU);

#define OP2_MAX_CPUS 8
static OP2CPU *op2_cpus[OP2_MAX_CPUS];
static uint32_t op2_cpus_registered = 0;
/* Per-core bring-up verdict, 1 = reached initCPU. Read by the probe
 * panic and the deadline dump (early-boot IOLogs scroll out of ramoops). */
static uint32_t op2_ap_online[OP2_MAX_CPUS];
/* AP mailbox: written by the MMU-off scratch entry (start.s) via a
 * PC-relative store. Non-static: referenced from assembly. 64B aligned
 * so the AP's line never shares with anything else. */
volatile uint64_t op2_ap_mailbox[8] __attribute__((aligned(64)));
/* B11 XNU-path crumbs: [0]=x20 bootargs PA, [1]=x21 cpu_data PA,
 * [2]=MPIDR. Written by start_cpu's first instruction (start.s). */
volatile uint64_t op2_ap_crumbs[8] __attribute__((aligned(64)));
/* Park-site selector (boot-arg op2_appark=N, 0 = full XNU path).
 * Non-static: read by the gated parks in start.s. */
volatile uint32_t op2_ap_parksel __attribute__((aligned(64)));
/* Ramoops AP scratch (B17): survives resets; readable from recovery. */
extern "C" volatile uint64_t *op2_ramoops_scratch(void);


extern "C" {
extern int master_cpu;
extern uintptr_t op2_secondary_entry;
extern uintptr_t op2_ap_probe;
extern vm_address_t start_cpu_paddr;
extern uint64_t cpu_ttep;
extern void start_cpu(void);
extern void LowResetVectorBase(void);
extern struct op2_cpu_data_entry CpuDataEntries[];
extern struct op2_reset_handler_data ResetHandlerData;
kern_return_t processor_start(processor_t processor);
vm_offset_t ml_static_vtop(vm_offset_t va);
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
void flush_dcache(vm_offset_t addr, unsigned size, boolean_t phys_flag);
void CleanPoC_DcacheRegion_Force(vm_offset_t va, unsigned length);
void FlushPoC_DcacheRegion(vm_offset_t va, unsigned length);
void op2_cpu_dump_status(void);
}

struct scm_desc_32 {
	uint32_t arginfo;
	uint32_t args[10];
	uint32_t ret[3];
};

static uint32_t scm_extra_arg_buf[8] __attribute__((aligned(64)));

static int32_t op2_scm_call2_armv8_32(uint32_t fn_id, struct scm_desc_32 *desc)
{
	uint32_t arglen = desc->arginfo & 0xf;
	uint32_t x5 = 0;

	if (arglen > 4) {
		for (uint32_t i = 0; i < arglen - 3; i++) {
			scm_extra_arg_buf[i] = desc->args[3 + i];
		}
		vm_offset_t p = ml_static_vtop((vm_offset_t)scm_extra_arg_buf);
		flush_dcache((vm_offset_t)scm_extra_arg_buf, sizeof(scm_extra_arg_buf), FALSE);
		x5 = (uint32_t)p;
	} else if (arglen == 4) {
		x5 = desc->args[3];
	}

	register uint32_t r0 __asm__("w0") = fn_id;
	register uint32_t r1 __asm__("w1") = desc->arginfo;
	register uint32_t r2 __asm__("w2") = desc->args[0];
	register uint32_t r3 __asm__("w3") = desc->args[1];
	register uint32_t r4 __asm__("w4") = desc->args[2];
	register uint32_t r5 __asm__("w5") = x5;
	register uint32_t r6 __asm__("w6") = 0;

	uint32_t intr_tries = 0;
	do {
		/* Reload every iteration: r0..r3 are read-write across the
		 * call, so after an INTERRUPTED return they hold TZ garbage
		 * (w0 = 1). Reissuing without reload invokes bogus fn 1. */
		r0 = fn_id;
		r1 = desc->arginfo;
		r2 = desc->args[0];
		r3 = desc->args[1];
		r4 = desc->args[2];
		r5 = x5;
		r6 = 0;
		__asm__ volatile(
			"smc #0\n"
			: "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
			: "r"(r4), "r"(r5), "r"(r6)
			: "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory"
		);
		/* SCM_INTERRUPTED: TZ asks us to reissue. Cap it: an
		 * endlessly-interrupted call used to hang startCPU (and the
		 * startAll probe with it); fail bounded instead. */
	} while ((int32_t)r0 == 1 && ++intr_tries < 1000); // SCM_INTERRUPTED
	if ((int32_t)r0 == 1) {
		IOLog("OP2CPU: SCM fn 0x%x interrupted %u times, giving up\n",
		    fn_id, intr_tries);
		r0 = (uint32_t)-2;
	}

	desc->ret[0] = r1;
	desc->ret[1] = r2;
	desc->ret[2] = r3;
	return (int32_t)r0;
}

/* Standard ARM PSCI calls (single SMC, no reissue loop -- PSCI has no
 * INTERRUPTED resume). PSCI_VERSION (0x84000000) is the safest probe:
 * no arguments, implemented by any PSCI firmware. If even it hangs,
 * Secure-World calls are broken from our context, not just CPU_ON. */
static int64_t
op2_psci_call(uint64_t func, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = func;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile(
		"smc #0\n"
		: "+r"(x0)
		: "r"(x1), "r"(x2), "r"(x3)
		: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
		  "x13", "x14", "x15", "x16", "x17", "cc", "memory"
	);
	return (int64_t)x0;
}

static void udelay_kern(uint32_t us)
{
	uint64_t start = 0;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(start));
	uint64_t ticks = (uint64_t)us * 20ULL;
	uint64_t now = 0;
	do {
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
	} while (now - start < ticks);
}

static int unclamp_secondary_cpu(uint32_t cpu)
{
	if (cpu == 0 || cpu > 3) return -1;

	vm_offset_t ldo_pa = 0xf9070000UL + cpu * 0x1000UL;
	vm_offset_t acc_pa = 0xf908b000UL + cpu * 0x10000UL;

	volatile uint32_t *ldo = (volatile uint32_t *)ml_io_map(ldo_pa, 0x1000);
	volatile uint32_t *acc = (volatile uint32_t *)ml_io_map(acc_pa, 0x1000);

	if (!ldo || !acc) {
		IOLog("OP2CPU: failed to map LDO/ACC for CPU %u\n", cpu);
		return -1;
	}

	/* Vendor order (cpu_ops.c): ACC unclamp FIRST, then LDO config.
	 * The LDO block ends with the retention latch pulse; latching
	 * before the ACC switches load leaves the rail to collapse under
	 * the AP (B6: AP ran wild, progressive corruption, silent reset). */
	uint32_t v;

	/* 1. ACC Unclamp Sequence */
	acc[0x14 / 4] = 0x00000001; // CPU_PWR_GATE_CTL: head switch enable few
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);

	acc[0x14 / 4] = 0x00000003; // head switch enable rest
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);

	acc[0x04 / 4] = 0x00000079; // CPU_PWR_CTL: de-assert coremem clamp
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);

	acc[0x04 / 4] = 0x0000007D; // close coremem array gdhs
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);

	acc[0x04 / 4] = 0x0000003D; // de-assert clamp
	__asm__ volatile("dmb sy" ::: "memory");

	acc[0x04 / 4] = 0x0000003C; // de-assert clamp
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);

	acc[0x04 / 4] = 0x0000000C; // de-assert core reset
	__asm__ volatile("dmb sy" ::: "memory");

	acc[0x04 / 4] = 0x0000008C; // assert PWRDUP
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);

	/* 2. Configure LDO + retention latch. VREF 0x2a is this LDO's own
	 * qcom,ldo-vref-ret (live.dts); 0x3e belongs to the A57 LDOs. */
	v = ldo[0x28 / 4];
	ldo[0x28 / 4] = (v & 0xffffff00) | 0x12; // LDO_BHS_PWR_CTL
	v = ldo[0x18 / 4];
	ldo[0x18 / 4] = (v & 0xffffff00) | 0xc2; // LDO_CFG1
	v = ldo[0x18 / 4];
	ldo[0x18 / 4] = (v & 0xffff00ff) | (0xf8 << 8);
	v = ldo[0x1c / 4];
	ldo[0x1c / 4] = (v & 0xffffff00) | 0x60; // LDO_CFG2
	v = ldo[0x1c / 4];
	ldo[0x1c / 4] = (v & 0xff00ffff) | (0x4a << 16);

	v = ldo[0x20 / 4];
	v &= ~(1 << 16);
	ldo[0x20 / 4] = v; // LDO_VREF_CFG
	v = (v & 0xffff80ff) | (0x2a << 8);
	ldo[0x20 / 4] = v;

	ldo[0] = 0;
	ldo[0] = 1;
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);
	ldo[0] = 0;
	__asm__ volatile("dmb sy" ::: "memory");

	IOLog("OP2CPU(%u): unclamp done - PWR_CTL: 0x%08x, GATE_CTL: 0x%08x\n",
	    cpu, acc[0x04 / 4], acc[0x14 / 4]);
	return 0;
}

bool
OP2CPU::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}

	cpu_num = 0;
	phys_id = 0;
	cpu_initialized = 0;

	OSData *cpuIDData = OSDynamicCast(OSData, provider->getProperty("cpu-id"));
	if (cpuIDData) {
		cpu_num = *(const uint32_t *)cpuIDData->getBytesNoCopy();
	}
	OSData *regData = OSDynamicCast(OSData, provider->getProperty("reg"));
	if (regData) {
		phys_id = *(const uint32_t *)regData->getBytesNoCopy();
	}

	setCPUNumber(cpu_num);
	IOLog("OP2CPU: registering CPU %u (phys_id=%u)\n", cpu_num, phys_id);

	ml_processor_info_t proc_info;
	bzero(&proc_info, sizeof(proc_info));
	proc_info.cpu_id = (cpu_id_t)this;
	proc_info.phys_id = phys_id;
	proc_info.log_id = cpu_num;
	proc_info.cluster_id = 0;
	proc_info.cluster_type = CLUSTER_TYPE_SMP;

	perfmon_interrupt_handler_func pmi_handler = NULL;
	kern_return_t kr = ml_processor_register(&proc_info, &machProcessor, &ipi_handler, &pmi_handler);
	if (kr != KERN_SUCCESS) {
		IOLog("OP2CPU(%u): ml_processor_register failed: %d\n", cpu_num, kr);
		return false;
	}

	registerService();

	if (cpu_num < OP2_MAX_CPUS) {
		__atomic_store_n(&op2_cpus[cpu_num], this, __ATOMIC_RELEASE);
		__atomic_add_fetch(&op2_cpus_registered, 1, __ATOMIC_ACQ_REL);
	}

	if (cpu_num == (UInt32)master_cpu) {
		thread_t thread;
		kr = kernel_thread_start((thread_continue_t)&OP2CPU::startAll, NULL, &thread);
		if (kr != KERN_SUCCESS) {
			panic("OP2CPU: failed to start startup thread: %d", kr);
		}
		thread_deallocate(thread);
	}

	return true;
}

void
OP2CPU::startAll(void *arg __unused, wait_result_t result __unused)
{
	uint32_t target_cpus = ml_get_cpu_count();
	IOLog("OP2CPU: startAll thread started, waiting for %u CPUs to register...\n", target_cpus);

	uint64_t deadline;
	clock_interval_to_deadline(5, kSecondScale, &deadline);
	while (__atomic_load_n(&op2_cpus_registered, __ATOMIC_ACQUIRE) < target_cpus) {
		if (mach_absolute_time() > deadline) {
			IOLog("OP2CPU: warning: registration timeout (%u/%u registered)\n",
			    op2_cpus_registered, target_cpus);
			break;
		}
		IOSleep(2);
	}

	uint32_t count = __atomic_load_n(&op2_cpus_registered, __ATOMIC_ACQUIRE);
	if (count > target_cpus) count = target_cpus;

	for (uint32_t i = 0; i < count; i++) {
		OP2CPU *cpu = __atomic_load_n(&op2_cpus[i], __ATOMIC_ACQUIRE);
		if (!cpu) continue;

		IOLog("OP2CPU(%u): invoking processor_start\n", i);
		kern_return_t kr = processor_start(cpu->machProcessor);
		if (kr != KERN_SUCCESS) {
			panic("OP2CPU(%u): processor_start failed: %d", i, kr);
		}

		clock_interval_to_deadline(500, kMillisecondScale, &deadline);
		while (__atomic_load_n(&cpu->cpu_initialized, __ATOMIC_ACQUIRE) == 0) {
			if (mach_absolute_time() > deadline) {
				IOLog("OP2CPU(%u): secondary core bring-up timed out after 500ms; skipping core and continuing\n", i);
				break;
			}
			IOSleep(2);
		}
		if (__atomic_load_n(&cpu->cpu_initialized, __ATOMIC_ACQUIRE) != 0) {
			IOLog("OP2CPU(%u): came online successfully!\n", i);
			if (i < OP2_MAX_CPUS) {
				__atomic_store_n(&op2_ap_online[i], 1, __ATOMIC_RELEASE);
			}
		}
		/* Mailbox + ACC report (mailbox mode): invalidate our
		 * stale-zero mailbox copy first (the AP wrote straight to
		 * RAM with no caches), then read it. */
		{
			uint32_t mbox_mode = 0;
			PE_parse_boot_argn("op2_apmailbox", &mbox_mode,
			    sizeof(mbox_mode));
			if (mbox_mode != 0 && i != (uint32_t)master_cpu) {
				FlushPoC_DcacheRegion(
				    (vm_offset_t)op2_ap_mailbox,
				    sizeof(op2_ap_mailbox));
				__asm__ volatile("dsb sy" ::: "memory");
				IOLog("OP2CPU(%u): AP mailbox: 0x%llx 0x%llx (want low32 0xe001a9b1)\n",
				    i, op2_ap_mailbox[0], op2_ap_mailbox[1]);
			} else if (i != (uint32_t)master_cpu) {
				FlushPoC_DcacheRegion(
				    (vm_offset_t)op2_ap_crumbs,
				    sizeof(op2_ap_crumbs));
				FlushPoC_DcacheRegion(
				    (vm_offset_t)op2_ap_mailbox,
				    sizeof(op2_ap_mailbox));
				__asm__ volatile("dsb sy" ::: "memory");
				IOLog("OP2CPU(%u): AP crumbs: x20=0x%llx x21=0x%llx mpidr=0x%llx lr=0x%llx park=0x%llx c=0x%llx (want mpidr 0x80000001)\n",
				    i, op2_ap_crumbs[0], op2_ap_crumbs[1],
				    op2_ap_crumbs[2], op2_ap_crumbs[3],
				    op2_ap_mailbox[0], op2_ap_mailbox[1]);
			}
		}
	}

	/* SMP probe mode: panic now with the verdict while the full early
	 * log still fits in ramoops (~1-min iteration instead of 2h). */
	{
		uint32_t smptest = 0;
		PE_parse_boot_argn("op2_smptest", &smptest, sizeof(smptest));
		if (smptest != 0) {
			op2_cpu_dump_status();
			panic("OP2 SMP probe: verdict above; resetting\n");
		}
	}

	if (op2_cpus[0]) {
		op2_cpus[0]->cpusRunning();
	}
	IOLog("OP2CPU: all %u CPUs online and running!\n", count);
	thread_terminate(current_thread());
}

extern "C" void
op2_cpu_dump_status(void)
{
	printf("OP2 CPU STATUS: registered=%u\n",
	    __atomic_load_n(&op2_cpus_registered, __ATOMIC_ACQUIRE));
	for (uint32_t i = 0; i < OP2_MAX_CPUS; i++) {
		OP2CPU *cpu = __atomic_load_n(&op2_cpus[i], __ATOMIC_ACQUIRE);
		if (cpu != NULL) {
			printf("OP2 CPU STATUS: cpu%u online=%u\n", i,
			    __atomic_load_n(&op2_ap_online[i], __ATOMIC_ACQUIRE));
		}
	}
}

void
OP2CPU::initCPU(bool boot)
{
	IOLog("OP2CPU(%u): initCPU (boot=%d)\n", cpu_num, (int)boot);
	setCPUState(kIOCPUStateRunning);
	__atomic_store_n(&cpu_initialized, 1, __ATOMIC_RELEASE);
}

void
OP2CPU::quiesceCPU(void)
{
	IOLog("OP2CPU(%u): quiesceCPU\n", cpu_num);
	setCPUState(kIOCPUStateStopped);
}

kern_return_t
OP2CPU::startCPU(vm_offset_t /*start_paddr*/, vm_offset_t /*arg_paddr*/)
{
	IOLog("OP2CPU(%u): startCPU called!\n", cpu_num);
	if (cpu_num == (UInt32)master_cpu) {
		return KERN_SUCCESS;
	}

	/* 0. The AP starts with MMU/caches off and reads from memory.
	 * cpu_start() already cleaned all the DATA (CpuDataEntries,
	 * cpu_data, ResetHandlerData, page tables) before calling us; the
	 * only gap is kernel TEXT on the AP's entry path. Clean that by
	 * VA range (counted loop, always terminates).
	 * NEVER call CleanPoC_Dcache()/FlushPoC_Dcache() etc here: their
	 * way/set loops exit on a 32-bit carry idiom (adds+b.cc) that
	 * never fires in 64-bit -- they spin forever (B2-B5 hang). */
	CleanPoC_DcacheRegion_Force((vm_offset_t)&op2_secondary_entry, 256);
	CleanPoC_DcacheRegion_Force((vm_offset_t)&LowResetVectorBase, 16384);
	__asm__ volatile("dsb sy; isb" ::: "memory");
	IOLog("OP2CPU(%u): PoC entry-text clean before kick done\n", cpu_num);

	/* AP mailbox mode (op2_apmailbox=1): kick at the scratch entry
	 * (one magic store, then sleep) instead of the XNU reset path.
	 * The scratch is contained (local VBAR trap), so cpu0 always
	 * survives to report the mailbox. */
	uint32_t mbox_mode = 0;
	PE_parse_boot_argn("op2_apmailbox", &mbox_mode, sizeof(mbox_mode));

	/* NOTE (B20 lesson): no CPUECTLR_EL1/CBAR_EL1 access here. NS-EL1
	 * reads trap to EL3 on this TrustZone; TZ owns coherency and sets
	 * SMPEN itself during SCM bringup. An earlier pre-kick probe froze
	 * cold boot with zero output. */

	vm_offset_t boot_addr;
	if (mbox_mode != 0) {
		for (unsigned i = 0; i < 8; i++) {
			op2_ap_mailbox[i] = 0;
		}
		CleanPoC_DcacheRegion_Force((vm_offset_t)&op2_ap_probe, 256);
		CleanPoC_DcacheRegion_Force((vm_offset_t)op2_ap_mailbox,
		    sizeof(op2_ap_mailbox));
		__asm__ volatile("dsb sy; isb" ::: "memory");
		boot_addr = ml_static_vtop((vm_offset_t)&op2_ap_probe);
		IOLog("OP2CPU(%u): mailbox mode: entry PA 0x%lx\n",
		    cpu_num, (unsigned long)boot_addr);
	} else {
		uint32_t parksel = 0;
		PE_parse_boot_argn("op2_appark", &parksel, sizeof(parksel));
		op2_ap_parksel = parksel;
		/* Clear the ramoops AP scratch (it survives resets; a stale
		 * marker from a previous boot would lie about this one). */
		{
			volatile uint64_t *rs = op2_ramoops_scratch();
			if (rs != NULL) {
				rs[0] = 0;
				rs[1] = 0;
				__asm__ volatile("dsb sy" ::: "memory");
			}
		}
		for (unsigned i = 0; i < 8; i++) {
			op2_ap_crumbs[i] = 0;
			op2_ap_mailbox[i] = 0;
		}
		CleanPoC_DcacheRegion_Force((vm_offset_t)op2_ap_crumbs,
		    sizeof(op2_ap_crumbs));
		CleanPoC_DcacheRegion_Force((vm_offset_t)op2_ap_mailbox,
		    sizeof(op2_ap_mailbox));
		CleanPoC_DcacheRegion_Force((vm_offset_t)&op2_ap_parksel,
		    sizeof(op2_ap_parksel));
		boot_addr = ml_static_vtop((vm_offset_t)&op2_secondary_entry);
		if (parksel != 0) {
			IOLog("OP2CPU(%u): park-site %u selected\n", cpu_num,
			    parksel);
		}
	}

	/* 1. PSCI_VERSION first: the safest possible Secure-World call.
	 * If it returns, SMC delivery works and later hangs are
	 * call-specific; if it hangs, all SMC from here is broken. */
	{
		IOLog("OP2CPU(%u): issuing PSCI_VERSION\n", cpu_num);
		int64_t ver = op2_psci_call(0x84000000ULL, 0, 0, 0);
		IOLog("OP2CPU(%u): PSCI_VERSION rc=0x%llx\n", cpu_num, ver);
	}

	/* 2. Try standard PSCI_CPU_ON. On success TrustZone owns the
	 * whole sequence (power, unclamp, reset release); skip the
	 * proprietary SCM+unclamp path entirely. rc 0 = on, -1 = not
	 * supported, -2 = invalid params, -3 = denied. */
	{
		IOLog("OP2CPU(%u): issuing PSCI_CPU_ON mpidr=0x%x entry=0x%lx\n",
		    cpu_num, phys_id, (unsigned long)boot_addr);
		int64_t psci_rc = op2_psci_call(0xC4000003ULL, phys_id,
		    (uint64_t)boot_addr, 0);
		IOLog("OP2CPU(%u): PSCI_CPU_ON rc=%lld\n", cpu_num, psci_rc);
		if (psci_rc == 0) {
			IOLog("OP2CPU(%u): PSCI accepted, AP starting\n", cpu_num);
			return KERN_SUCCESS;
		}
		IOLog("OP2CPU(%u): PSCI not usable (rc=%lld), SCM fallback\n",
		    cpu_num, psci_rc);
	}

	// 3. Set SCM TrustZone boot address to op2_secondary_entry
	IOLog("OP2CPU(%u): setting TrustZone boot address to PA 0x%lx\n", cpu_num, (unsigned long)boot_addr);
	{
		uint64_t cur_el = 0, mpidr = 0;
		__asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
		__asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
		IOLog("OP2CPU(%u): AP inputs: entry VA 0x%lx PA 0x%lx\n",
		    cpu_num, (unsigned long)&op2_secondary_entry, (unsigned long)boot_addr);
		IOLog("OP2CPU(%u): AP inputs: LowResetVectorBase VA 0x%lx, start_cpu VA 0x%lx paddr 0x%lx\n",
		    cpu_num, (unsigned long)&LowResetVectorBase,
		    (unsigned long)&start_cpu, (unsigned long)start_cpu_paddr);
		IOLog("OP2CPU(%u): AP inputs: cpu_data VA %p PA %p entries PA 0x%llx ttep 0x%llx (boot EL 0x%llx MPIDR 0x%llx)\n",
		    cpu_num, CpuDataEntries[cpu_num].cpu_data_vaddr,
		    CpuDataEntries[cpu_num].cpu_data_paddr,
		    (unsigned long long)ResetHandlerData.cpu_data_entries,
		    (unsigned long long)cpu_ttep,
		    (unsigned long long)cur_el, (unsigned long long)mpidr);
	}

	struct scm_desc_32 desc;
	bzero(&desc, sizeof(desc));
	desc.args[0] = (uint32_t)boot_addr;
	desc.args[1] = (1 << cpu_num); // aff0
	desc.args[2] = (1 << 0);       // aff1: Cluster 0
	desc.args[3] = (1 << 0);       // aff2: 1
	desc.args[4] = ~0U;            // reserved
	desc.args[5] = 0x02 | 0x01;    // SCM_FLAG_COLDBOOT_MC | SCM_FLAG_HLOS
	desc.arginfo = 6;

	int32_t rc = op2_scm_call2_armv8_32(0x02000111, &desc);
	if (rc != 0) {
		IOLog("OP2CPU(%u): SCM 32-bit set-boot-addr rc=%d, trying 64-bit\n",
		    cpu_num, rc);
		rc = op2_scm_call2_armv8_32(0x82000111, &desc);
	}
	if (rc != 0) {
		IOLog("OP2CPU(%u): ERROR scm_set_boot_addr_mc failed (rc=%d)!\n", cpu_num, rc);
		return KERN_FAILURE;
	}
	IOLog("OP2CPU(%u): SCM set-boot-addr ok\n", cpu_num);

	// 4. Unclamp secondary CPU via ACC & LDO
	if (unclamp_secondary_cpu(cpu_num) != 0) {
		IOLog("OP2CPU(%u): ERROR unclamp failed!\n", cpu_num);
		return KERN_FAILURE;
	}

	// 5. Broadcast SEV event
	__asm__ volatile("sev; dsb sy; isb" ::: "memory");
	IOLog("OP2CPU(%u): unclamp and SEV broadcast complete\n", cpu_num);

	return KERN_SUCCESS;
}

void
OP2CPU::haltCPU(void)
{
	IOLog("OP2CPU(%u): haltCPU\n", cpu_num);
	setCPUState(kIOCPUStateStopped);
}

void
OP2CPU::signalCPU(IOCPU *target)
{
	if (target && op2_gic_dist_base != 0) {
		uint32_t target_cpu = target->getCPUNumber();
		uint32_t sgir = ((1U << target_cpu) << 16) | 1U; // SGI 1
		*(volatile uint32_t *)(op2_gic_dist_base + 0x0F00) = sgir;
		__asm__ volatile("dmb sy" ::: "memory");
	}
}

const OSSymbol *
OP2CPU::getCPUName(void)
{
	return OSSymbol::withCStringNoCopy("arm,cortex-a53");
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
