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
#include "OP2CPU.h"
#include "OP2InterruptController.h"

#define super IOCPU

OSDefineMetaClassAndStructors(OP2CPU, IOCPU);

#define OP2_MAX_CPUS 8
static OP2CPU *op2_cpus[OP2_MAX_CPUS];
static uint32_t op2_cpus_registered = 0;

extern "C" {
extern int master_cpu;
extern uintptr_t op2_secondary_entry;
kern_return_t processor_start(processor_t processor);
vm_offset_t ml_static_vtop(vm_offset_t va);
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
void flush_dcache(vm_offset_t addr, unsigned size, boolean_t phys_flag);
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

	do {
		__asm__ volatile(
			"smc #0\n"
			: "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
			: "r"(r4), "r"(r5), "r"(r6)
			: "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory"
		);
	} while ((int32_t)r0 == 1); // SCM_INTERRUPTED

	desc->ret[0] = r1;
	desc->ret[1] = r2;
	desc->ret[2] = r3;
	return (int32_t)r0;
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

	/* 1. Configure LDO */
	uint32_t v = ldo[0x28 / 4];
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
	v = (v & 0xffff80ff) | (0x3e << 8);
	ldo[0x20 / 4] = v;

	ldo[0] = 0;
	ldo[0] = 1;
	__asm__ volatile("dmb sy" ::: "memory");
	udelay_kern(2);
	ldo[0] = 0;
	__asm__ volatile("dmb sy" ::: "memory");

	/* 2. ACC Unclamp Sequence */
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

		clock_interval_to_deadline(100, kMillisecondScale, &deadline);
		while (__atomic_load_n(&cpu->cpu_initialized, __ATOMIC_ACQUIRE) == 0) {
			if (mach_absolute_time() > deadline) {
				IOLog("OP2CPU(%u): secondary core bring-up timed out after 100ms; skipping core and continuing\n", i);
				break;
			}
			IOSleep(2);
		}
		if (__atomic_load_n(&cpu->cpu_initialized, __ATOMIC_ACQUIRE) != 0) {
			IOLog("OP2CPU(%u): came online successfully!\n", i);
		}
	}

	if (op2_cpus[0]) {
		op2_cpus[0]->cpusRunning();
	}
	IOLog("OP2CPU: all %u CPUs online and running!\n", count);
	thread_terminate(current_thread());
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

	// 1. Set SCM TrustZone boot address to op2_secondary_entry
	vm_offset_t boot_addr = ml_static_vtop((vm_offset_t)&op2_secondary_entry);
	IOLog("OP2CPU(%u): setting TrustZone boot address to PA 0x%lx\n", cpu_num, (unsigned long)boot_addr);

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
		rc = op2_scm_call2_armv8_32(0x82000111, &desc);
	}
	if (rc != 0) {
		IOLog("OP2CPU(%u): ERROR scm_set_boot_addr_mc failed (rc=%d)!\n", cpu_num, rc);
		return KERN_FAILURE;
	}

	// 2. Unclamp secondary CPU via ACC & LDO
	if (unclamp_secondary_cpu(cpu_num) != 0) {
		IOLog("OP2CPU(%u): ERROR unclamp failed!\n", cpu_num);
		return KERN_FAILURE;
	}

	// 3. Broadcast SEV event
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
