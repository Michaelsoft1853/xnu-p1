/*
 * Kernel Adreno 430 GPU Driver for OnePlus 2 (MSM8994).
 *
 * Implements hardware bring-up for Qualcomm Adreno 430:
 * 1. MMSSCC clock branches & GDSC power domain uncollapse
 * 2. Adreno 430 MMIO mapping (0xfdb00000, 256 KB)
 * 3. Ringbuffer allocation (32 KB contiguous wired physical memory)
 * 4. Microcode firmware bootstrapping (a420_pm4.fw, a420_pfp.fw)
 * 5. Hardware command processing & PM4 Type 3 packet execution
 */

#ifndef _OP2_GPU_H
#define _OP2_GPU_H

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

/* Physical Base Addresses */
#define OP2_MMSSCC_PHYS_BASE            0xfd8c0000ULL
#define OP2_MMSSCC_PHYS_SIZE            0x6000ULL

#define OP2_ADRENO_PHYS_BASE            0xfdb00000ULL
#define OP2_ADRENO_PHYS_SIZE            0x40000ULL      /* 256 KB */

/* MMSSCC Register Offsets (Byte offsets from 0xfd8c0000) */
#define MMSS_OXILI_GX_GDSC              0x4024
#define MMSS_OXILI_CX_GDSC              0x4034
#define MMSS_OXILI_GFX3D_CBCR           0x4028
#define MMSS_OXILICX_AHB_CBCR           0x403c
#define MMSS_OXILI_RBBMTIMER_CBCR       0x40b0

/* GDSC Bits */
#define GDSC_SW_COLLAPSE_BIT            (1U << 0)
#define GDSC_PWR_ON_BIT                 (1U << 31)

/* CBCR Bits */
#define CBCR_CLK_ENABLE_BIT             (1U << 0)
#define CBCR_CLK_OFF_BIT                (1U << 31)

/* Adreno 4xx Word Offsets (Byte Offset = Word * 4) */
#define A4XX_RBBM_SW_RESET_CMD          0x022
#define A4XX_RBBM_AHB_CTL0              0x023
#define A4XX_RBBM_AHB_CTL1              0x024
#define A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL  0x02b
#define A4XX_RBBM_RBBM_CTL              0x03e
#define A4XX_RBBM_PERFCTR_CTL           0x170
#define A4XX_RBBM_GPU_BUSY_MASKED       0x17a
#define A4XX_RBBM_STATUS                0x191
#define A4XX_RBBM_POWER_STATUS          0x1b0
#define A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL2 0x1b8

#define A4XX_UCHE_CACHE_WAYS_VFD        0x384
#define A4XX_UCHE_TRAP_BASE_LO          0x386
#define A4XX_UCHE_TRAP_BASE_HI          0x387

#define A4XX_RB_GMEM_BASE_ADDR          0xcc0

#define A4XX_CP_RB_BASE                 0x200
#define A4XX_CP_RB_CNTL                 0x201
#define A4XX_CP_RB_RPTR_ADDR            0x203
#define A4XX_CP_RB_RPTR                 0x204
#define A4XX_CP_RB_WPTR                 0x205
#define A4XX_CP_PFP_UCODE_ADDR          0x223
#define A4XX_CP_PFP_UCODE_DATA          0x224
#define A4XX_CP_ME_RAM_WADDR            0x225
#define A4XX_CP_ME_RAM_DATA             0x227
#define A4XX_CP_DEBUG                   0x22c
#define A4XX_CP_ME_CNTL                 0x22d

/* PM4 Packet Macros */
#define CP_TYPE3_PKT                    (3U << 30)
#define cp_type3_packet(op, cnt)        (CP_TYPE3_PKT | (((cnt) - 1) << 16) | (((op) & 0xff) << 8))

#define CP_ME_INIT                      0x48
#define CP_NOP                          0x10
#define CP_REG_RMW                      0x21
#define CP_INTERRUPT                    0x40
#define CP_BOOTSTRAP_UCODE              0x6f

/* Ringbuffer Configuration */
#define OP2_GPU_RB_SIZE                 (32 * 1024)     /* 32 KB */
#define OP2_GPU_RB_DWORDS               (OP2_GPU_RB_SIZE / 4)

class OP2GPU : public IOService {
	OSDeclareDefaultStructors(OP2GPU);

private:
	vm_offset_t fMMSSBase;
	vm_offset_t fGPUBase;

	uint32_t   *fRingBufferVirt;
	uint64_t    fRingBufferPhys;
	uint32_t    fWPTR;

	bool        fEnabled;
	bool        fHardwareInitialized;

	void        writeMMSS(uint32_t byte_offset, uint32_t val);
	uint32_t    readMMSS(uint32_t byte_offset);

	void        writeGPU(uint32_t word_offset, uint32_t val);
	uint32_t    readGPU(uint32_t word_offset);

	bool        uncollapseGDSC(uint32_t byte_offset, const char *name);
	bool        enableBranchClock(uint32_t byte_offset, const char *name);
	bool        powerOnClocks(void);

	bool        initAdrenoRegisters(void);
	bool        bootstrapMicrocode(void);
	bool        initRingBuffer(void);
	bool        submitPacket(const uint32_t *words, uint32_t count);
	bool        waitRPTR(uint32_t target_rptr, uint32_t timeout_us);

public:
	virtual bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
#endif /* _OP2_GPU_H */
