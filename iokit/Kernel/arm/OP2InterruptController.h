/*
 * Copyright (c) 2026. Port for OnePlus 2 (Qualcomm Snapdragon 810 / MSM8994).
 */

#ifndef _IOKIT_OP2_INTERRUPT_CONTROLLER_H
#define _IOKIT_OP2_INTERRUPT_CONTROLLER_H

#include <IOKit/IOInterruptController.h>

class OP2InterruptController : public IOInterruptController {
	OSDeclareDefaultStructors(OP2InterruptController);

protected:
	int numVectors;

public:
	virtual bool init(void) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	virtual IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;

	virtual void initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;
	virtual void disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector) APPLE_KEXT_OVERRIDE;

	virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;
};

#endif /* ! _IOKIT_OP2_INTERRUPT_CONTROLLER_H */
