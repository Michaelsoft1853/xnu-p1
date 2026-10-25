/*
 * Copyright (c) 2026. Port for OnePlus 2 (Qualcomm Snapdragon 810 / MSM8994).
 */

#ifndef _IOKIT_OP2CPU_H
#define _IOKIT_OP2CPU_H

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOCPU.h>

class OP2CPU : public IOCPU {
	OSDeclareDefaultStructors(OP2CPU);

protected:
	UInt32 cpu_num;
	UInt32 phys_id;
	uint32_t cpu_initialized;

public:
	static void startAll(void *arg, wait_result_t result);
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
	virtual void signalCPU(IOCPU *target) APPLE_KEXT_OVERRIDE;
	virtual const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
};

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
#endif /* ! _IOKIT_OP2CPU_H */
