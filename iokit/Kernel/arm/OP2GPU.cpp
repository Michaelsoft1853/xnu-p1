/*
 * Kernel Adreno 430 GPU Driver for OnePlus 2 (MSM8994).
 *
 * Implements Phase 2 of GPU bring-up:
 * 1. MMSSCC clock branches & GDSC power domain uncollapse
 * 2. Adreno 430 MMIO mapping (0xfdb00000, 256 KB)
 * 3. 32 KB wired command ring buffer allocation
 * 4. Microcode firmware bootstrapping (a420_pm4.fw, a420_pfp.fw)
 * 5. Hardware command processing & PM4 Type 3 packet execution
 */

#include "OP2GPU.h"
#include "op2_a430_fw.h"
#include <pexpert/pexpert.h>
#include <libkern/OSByteOrder.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)

OSDefineMetaClassAndStructors(OP2GPU, IOService);

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
}

void
OP2GPU::writeMMSS(uint32_t byte_offset, uint32_t val)
{
	if (!fMMSSBase) return;
	volatile uint32_t *reg = (volatile uint32_t *)(fMMSSBase + byte_offset);
	*reg = val;
	OSMemoryBarrier();
}

uint32_t
OP2GPU::readMMSS(uint32_t byte_offset)
{
	if (!fMMSSBase) return 0;
	OSMemoryBarrier();
	volatile uint32_t *reg = (volatile uint32_t *)(fMMSSBase + byte_offset);
	return *reg;
}

void
OP2GPU::writeGPU(uint32_t word_offset, uint32_t val)
{
	if (!fGPUBase) return;
	volatile uint32_t *reg = (volatile uint32_t *)(fGPUBase + (word_offset << 2));
	*reg = val;
	OSMemoryBarrier();
}

uint32_t
OP2GPU::readGPU(uint32_t word_offset)
{
	if (!fGPUBase) return 0;
	OSMemoryBarrier();
	volatile uint32_t *reg = (volatile uint32_t *)(fGPUBase + (word_offset << 2));
	return *reg;
}

bool
OP2GPU::uncollapseGDSC(uint32_t byte_offset, const char *name)
{
	IOLog("OP2 GPU: reading GDSC %s (offset 0x%04x)...\n", name, byte_offset);
	uint32_t val = readMMSS(byte_offset);
	IOLog("OP2 GPU: GDSC %s initial value = 0x%08x\n", name, val);

	if ((val & GDSC_PWR_ON_BIT) && !(val & GDSC_SW_COLLAPSE_BIT)) {
		IOLog("OP2 GPU: GDSC %s already uncollapsed (0x%08x)\n", name, val);
		return true;
	}

	/* Clear SW_COLLAPSE bit to power on domain */
	val &= ~GDSC_SW_COLLAPSE_BIT;
	writeMMSS(byte_offset, val);

	/* Poll for PWR_ON bit to be set (up to 100 tries of 10us = 1ms) */
	for (int i = 0; i < 100; i++) {
		val = readMMSS(byte_offset);
		if (val & GDSC_PWR_ON_BIT) {
			IOLog("OP2 GPU: GDSC %s successfully uncollapsed (0x%08x, took %d us)\n",
			      name, val, i * 10);
			return true;
		}
		IODelay(10);
	}

	IOLog("OP2 GPU: WARNING: GDSC %s timeout waiting for PWR_ON (0x%08x)\n", name, val);
	return false;
}

bool
OP2GPU::enableBranchClock(uint32_t byte_offset, const char *name)
{
	IOLog("OP2 GPU: reading clock %s (offset 0x%04x)...\n", name, byte_offset);
	uint32_t val = readMMSS(byte_offset);
	IOLog("OP2 GPU: clock %s initial value = 0x%08x\n", name, val);

	if ((val & CBCR_CLK_ENABLE_BIT) && !(val & CBCR_CLK_OFF_BIT)) {
		IOLog("OP2 GPU: Clock %s already active (0x%08x)\n", name, val);
		return true;
	}

	/* Write CLK_ENABLE bit */
	writeMMSS(byte_offset, val | CBCR_CLK_ENABLE_BIT);

	/* Poll for CLK_OFF bit to become 0 (up to 100 tries of 10us = 1ms) */
	for (int i = 0; i < 100; i++) {
		val = readMMSS(byte_offset);
		if (!(val & CBCR_CLK_OFF_BIT)) {
			IOLog("OP2 GPU: Clock %s enabled (0x%08x, took %d us)\n",
			      name, val, i * 10);
			return true;
		}
		IODelay(10);
	}

	IOLog("OP2 GPU: WARNING: Clock %s timeout waiting for CLK_OFF==0 (0x%08x)\n", name, val);
	return false;
}

bool
OP2GPU::powerOnClocks(void)
{
	bool ok = true;
	IOLog("OP2 GPU: probing MMSS GDSC power domains...\n");
	if (!uncollapseGDSC(MMSS_OXILI_CX_GDSC, "oxili_cx")) {
		ok = false;
	}
	if (!uncollapseGDSC(MMSS_OXILI_GX_GDSC, "oxili_gx")) {
		ok = false;
	}

	IOLog("OP2 GPU: probing MMSS branch clocks...\n");
	if (!enableBranchClock(MMSS_OXILICX_AHB_CBCR, "oxilicx_ahb")) {
		ok = false;
	}
	if (!enableBranchClock(MMSS_OXILI_GFX3D_CBCR, "oxili_gfx3d")) {
		ok = false;
	}
	if (!enableBranchClock(MMSS_OXILI_RBBMTIMER_CBCR, "oxili_rbbmtimer")) {
		ok = false;
	}

	return ok;
}

bool
OP2GPU::initAdrenoRegisters(void)
{
	IOLog("OP2 GPU: resetting Adreno 430 core...\n");
	writeGPU(A4XX_RBBM_SW_RESET_CMD, 1);
	IODelay(100);
	writeGPU(A4XX_RBBM_SW_RESET_CMD, 0);
	IODelay(100);

	uint32_t status = readGPU(A4XX_RBBM_STATUS);
	uint32_t pwr = readGPU(A4XX_RBBM_POWER_STATUS);
	IOLog("OP2 GPU: Adreno core status=0x%08x power_status=0x%08x\n", status, pwr);

	/* Basic RBBM Configuration */
	writeGPU(A4XX_RBBM_GPU_BUSY_MASKED, 0xffffffff);
	writeGPU(A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL, 0x10);
	writeGPU(A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL2, 0x30);
	writeGPU(A4XX_RBBM_AHB_CTL0, 0x00000001);
	writeGPU(A4XX_RBBM_AHB_CTL1, 0xa6ffffff);
	writeGPU(A4XX_RBBM_RBBM_CTL, 0x00000030);
	writeGPU(A4XX_RBBM_PERFCTR_CTL, 0x01);

	/* GMEM/OCMEM address and cache configuration */
	writeGPU(A4XX_RB_GMEM_BASE_ADDR, 0);
	writeGPU(A4XX_UCHE_CACHE_WAYS_VFD, 0x07);
	writeGPU(A4XX_UCHE_TRAP_BASE_LO, 0xffff0000);
	writeGPU(A4XX_UCHE_TRAP_BASE_HI, 0xffff0000);

	/* Set bootstrap speedup chicken bit for A430 */
	writeGPU(A4XX_CP_DEBUG, (1U << 14));

	return true;
}

bool
OP2GPU::initRingBuffer(void)
{
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fRingBufferVirt = (uint32_t *)IOMallocContiguous(OP2_GPU_RB_SIZE, 4096, &phys);
#pragma clang diagnostic pop
	if (!fRingBufferVirt) {
		IOLog("OP2 GPU: ERROR: failed to allocate %u bytes contiguous ringbuffer\n",
		      OP2_GPU_RB_SIZE);
		return false;
	}

	fRingBufferPhys = (uint64_t)phys;
	fWPTR = 0;
	bzero(fRingBufferVirt, OP2_GPU_RB_SIZE);

	IOLog("OP2 GPU: Ringbuffer allocated: virt=%p phys=0x%llx size=%u bytes (%u dwords)\n",
	      fRingBufferVirt, fRingBufferPhys, OP2_GPU_RB_SIZE, OP2_GPU_RB_DWORDS);

	/*
	 * CP_RB_CNTL:
	 * Size in quadwords = 32KB / 8 = 4096 quadwords.
	 * ilog2(4096) = 12.
	 * Bit 27: disable shadow RPTR to use direct register reading.
	 */
	uint32_t rb_cntl = (12 & 0x3f) | (1U << 27);
	writeGPU(A4XX_CP_RB_CNTL, rb_cntl);
	writeGPU(A4XX_CP_RB_BASE, (uint32_t)fRingBufferPhys);
	writeGPU(A4XX_CP_RB_WPTR, 0);
	writeGPU(A4XX_CP_RB_RPTR, 0);

	return true;
}

bool
OP2GPU::waitRPTR(uint32_t target_rptr, uint32_t timeout_us)
{
	for (uint32_t i = 0; i < timeout_us / 10; i++) {
		uint32_t rptr = readGPU(A4XX_CP_RB_RPTR);
		if (rptr == target_rptr) {
			return true;
		}
		IODelay(10);
	}
	uint32_t final_rptr = readGPU(A4XX_CP_RB_RPTR);
	IOLog("OP2 GPU: waitRPTR timeout! expected=%u actual=%u\n", target_rptr, final_rptr);
	return false;
}

bool
OP2GPU::submitPacket(const uint32_t *words, uint32_t count)
{
	if (!fRingBufferVirt) return false;
	if (fWPTR + count >= OP2_GPU_RB_DWORDS) {
		IOLog("OP2 GPU: ringbuffer wrap not implemented in early bringup\n");
		return false;
	}

	for (uint32_t i = 0; i < count; i++) {
		fRingBufferVirt[fWPTR + i] = words[i];
	}
	fWPTR += count;

	OSMemoryBarrier();
	writeGPU(A4XX_CP_RB_WPTR, fWPTR);
	return true;
}

bool
OP2GPU::bootstrapMicrocode(void)
{
	IOLog("OP2 GPU: bootstrapping Adreno 430 microcode (PFP=%u words, PM4=%u words)...\n",
	      OP2_A430_PFP_FW_WORDS, OP2_A430_PM4_FW_WORDS);

	/* 1. Overwrite jump table with special bootstrap opcode */
	writeGPU(A4XX_CP_PFP_UCODE_ADDR, 0x400);
	writeGPU(A4XX_CP_PFP_UCODE_DATA, 0x6f0009);

	/* 2. Clear ME_HALT to start micro-engine */
	writeGPU(A4XX_CP_ME_CNTL, 0);

	/* 3. Build bootstrap packet */
	uint32_t pfp_idx = 1;
	uint32_t pfp_size = OP2_A430_PFP_FW_WORDS - pfp_idx;
	uint32_t pfp_addr = 0;

	uint32_t pm4_idx = 1;
	uint32_t pm4_size = OP2_A430_PM4_FW_WORDS - pm4_idx;
	uint32_t pm4_addr = 0;

	uint32_t bootstrap_size = pm4_size + pfp_size + 5;

	uint32_t *cmds = &fRingBufferVirt[fWPTR];
	uint32_t *c = cmds;

	*c++ = cp_type3_packet(CP_BOOTSTRAP_UCODE, bootstrap_size - 1);
	*c++ = pfp_size;
	*c++ = pfp_addr;
	*c++ = pm4_size;
	*c++ = pm4_addr;

	for (uint32_t i = pm4_idx; i < OP2_A430_PM4_FW_WORDS; i++) {
		*c++ = op2_a430_pm4_fw[i];
	}
	for (uint32_t i = pfp_idx; i < OP2_A430_PFP_FW_WORDS; i++) {
		*c++ = op2_a430_pfp_fw[i];
	}

	*c++ = cp_type3_packet(CP_REG_RMW, 3);
	*c++ = 0x20000000 + A4XX_CP_RB_WPTR;
	*c++ = 0xffffffff;
	*c++ = 0x00000002;
	*c++ = cp_type3_packet(CP_INTERRUPT, 1);
	*c++ = 0;

	uint32_t emitted = (uint32_t)(c - cmds);
	IOLog("OP2 GPU: Emitted %u words into ringbuffer for bootstrap\n", emitted);

	fWPTR += emitted;
	OSMemoryBarrier();

	/* In A4xx, submit WPTR - 2 first, then advance */
	writeGPU(A4XX_CP_RB_WPTR, fWPTR - 2);

	/* Wait for CP to consume the bootstrap payload */
	if (!waitRPTR(fWPTR - 2, 200000)) {
		IOLog("OP2 GPU: ERROR: microcode bootstrap packet timeout!\n");
		return false;
	}

	writeGPU(A4XX_CP_RB_WPTR, fWPTR);
	waitRPTR(fWPTR, 50000);

	/* Clear chicken bit */
	writeGPU(A4XX_CP_DEBUG, 0);

	IOLog("OP2 GPU: Microcode bootstrap complete! RPTR=%u WPTR=%u\n",
	      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_RB_WPTR));
	return true;
}

bool
OP2GPU::init(OSDictionary *dictionary)
{
	if (!IOService::init(dictionary))
		return false;

	fMMSSBase = 0;
	fGPUBase = 0;
	fRingBufferVirt = NULL;
	fRingBufferPhys = 0;
	fWPTR = 0;
	fEnabled = false;
	fHardwareInitialized = false;

	return true;
}

bool
OP2GPU::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;

	uint32_t gpu_arg = 0;
	PE_parse_boot_argn("gpu", &gpu_arg, sizeof(gpu_arg));
	if (gpu_arg == 0) {
		IOLog("OP2 GPU: driver disabled (pass gpu=1 in boot-args to enable)\n");
		registerService();
		return true;
	}

	uint32_t gpu_hw = 0;
	PE_parse_boot_argn("gpu_hw", &gpu_hw, sizeof(gpu_hw));

	IOLog("OP2 GPU: Initializing Adreno 430 GPU driver (gpu=%u, gpu_hw=%u)...\n", gpu_arg, gpu_hw);

	/* Set device properties in IORegistry */
	setProperty("DeviceName", "Qualcomm Adreno 430");
	setProperty("ChipID", 0x04030002, 32);
	setProperty("MetalPluginName", "OP2Metal");
	setProperty("MetalPluginClassName", "OP2MetalDevice");
	setProperty("IOGLBundleName", "OP2Metal");
	setProperty("IOGLPluginName", "OP2Metal");
	setProperty("RingBufferSize", OP2_GPU_RB_SIZE, 32);

	if (gpu_hw == 0) {
		IOLog("OP2 GPU: Running in safe software-shim mode (gpu_hw=0); direct silicon MMIO bypassed to prevent AXI bus stall\n");
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		IOLog("OP2 GPU: driver published in IORegistry successfully!\n");
		return true;
	}

	/* Direct Hardware Bring-Up (only if gpu_hw=1 is explicitly passed) */
	IOLog("OP2 GPU: Attempting physical silicon bring-up (gpu_hw=1)...\n");

	/* 1. Map MMSS CC and GPU MMIO */
	fMMSSBase = ml_io_map(OP2_MMSSCC_PHYS_BASE, OP2_MMSSCC_PHYS_SIZE);
	if (!fMMSSBase) {
		IOLog("OP2 GPU: ERROR: failed to map MMSS CC at 0x%llx\n", OP2_MMSSCC_PHYS_BASE);
		registerService();
		return true;
	}
	fGPUBase = ml_io_map(OP2_ADRENO_PHYS_BASE, OP2_ADRENO_PHYS_SIZE);
	if (!fGPUBase) {
		IOLog("OP2 GPU: ERROR: failed to map Adreno MMIO at 0x%llx\n", OP2_ADRENO_PHYS_BASE);
		registerService();
		return true;
	}
	IOLog("OP2 GPU: MMIO mapped: MMSSCC=%p Adreno=%p\n", (void *)fMMSSBase, (void *)fGPUBase);

	/* 2. Power on GDSC and branch clocks */
	if (!powerOnClocks()) {
		IOLog("OP2 GPU: powerOnClocks failed (rails/clocks not active); skipping Adreno MMIO to prevent AXI bus hang\n");
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		IOLog("OP2 GPU: driver registered in safe fallback mode\n");
		return true;
	}

	/* 3. Allocate ringbuffer */
	if (!initRingBuffer()) {
		registerService();
		return true;
	}

	/* 4. Configure Adreno core */
	initAdrenoRegisters();

	/* 5. Bootstrap microcode */
	if (!bootstrapMicrocode()) {
		IOLog("OP2 GPU: Microcode bootstrap failed; aborting pipeline start\n");
		registerService();
		return true;
	}

	/* 6. Issue ME Initialization packet */
	uint32_t me_init_cmds[] = {
		cp_type3_packet(CP_ME_INIT, 17),
		0x000003f7,
		0x00000000,
		0x00000000,
		0x00000000,
		0x00000080,
		0x00000100,
		0x00000180,
		0x00006600,
		0x00000150,
		0x0000014e,
		0x00000154,
		0x00000001,
		0x00000000,
		0x00000000,
		0x20000000,
		0x00000000,
		0x00000000
	};
	submitPacket(me_init_cmds, sizeof(me_init_cmds) / sizeof(uint32_t));
	if (waitRPTR(fWPTR, 50000)) {
		IOLog("OP2 GPU: CP_ME_INIT accepted! RPTR=%u WPTR=%u\n",
		      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_RB_WPTR));
	} else {
		IOLog("OP2 GPU: WARNING: CP_ME_INIT wait timed out\n");
	}

	/* 7. Issue first CP_NOP packet to test command execution pipeline */
	uint32_t nop_cmds[] = {
		cp_type3_packet(CP_NOP, 4),
		0xdeada430,
		0x12345678,
		0x87654321,
		0xcafe0001
	};
	submitPacket(nop_cmds, sizeof(nop_cmds) / sizeof(uint32_t));
	if (waitRPTR(fWPTR, 50000)) {
		IOLog("OP2 GPU: SUCCESS: Hardware CP_NOP packet executed! RPTR=%u WPTR=%u\n",
		      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_RB_WPTR));
		fHardwareInitialized = true;
	} else {
		IOLog("OP2 GPU: WARNING: Hardware CP_NOP packet wait timed out\n");
	}

	setProperty("HardwareAccelerated", fHardwareInitialized ? kOSBooleanTrue : kOSBooleanFalse);
	registerService();
	IOLog("OP2 GPU: driver published in IORegistry successfully!\n");
	return true;
}

void
OP2GPU::stop(IOService *provider)
{
	if (fRingBufferVirt) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		IOFreeContiguous(fRingBufferVirt, OP2_GPU_RB_SIZE);
#pragma clang diagnostic pop
		fRingBufferVirt = NULL;
	}
	IOService::stop(provider);
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
