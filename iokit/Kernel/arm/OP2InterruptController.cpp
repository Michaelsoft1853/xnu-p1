/*
 * Copyright (c) 2026. Port for OnePlus 2 (Qualcomm Snapdragon 810 / MSM8994).
 */

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include "OP2InterruptController.h"

#define super IOInterruptController

OSDefineMetaClassAndStructors(OP2InterruptController, IOInterruptController);

bool
OP2InterruptController::init(void)
{
	if (!super::init()) {
		return false;
	}

	numVectors = 1024;
	vm_size_t sz = numVectors * sizeof(IOInterruptVector);
	vectors = (IOInterruptVector *)IOMalloc(sz);
	if (!vectors) {
		return false;
	}
	bzero(vectors, sz);

	for (int i = 0; i < numVectors; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		if (!vectors[i].interruptLock) {
			return false;
		}
	}

	return true;
}

bool
OP2InterruptController::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}

	IOLog("OP2InterruptController: registering interrupt controller services\n");

	IOPlatformExpert *plat = getPlatform();
	if (plat) {
		if (gPlatformInterruptControllerName) {
			plat->registerInterruptController(gPlatformInterruptControllerName, this);
		}
		if (gIODTDefaultInterruptController) {
			plat->registerInterruptController(const_cast<OSSymbol *>(gIODTDefaultInterruptController), this);
		}
		const OSSymbol *gicSym = OSSymbol::withCString("gic");
		if (gicSym) {
			plat->registerInterruptController(const_cast<OSSymbol *>(gicSym), this);
			gicSym->release();
		}
		const OSSymbol *op2Sym = OSSymbol::withCString("OP2InterruptController");
		if (op2Sym) {
			plat->registerInterruptController(const_cast<OSSymbol *>(op2Sym), this);
			op2Sym->release();
		}
	} else {
		IOLog("OP2InterruptController: ERROR getPlatform() is NULL!\n");
		return false;
	}

	registerService();
	return true;
}

IOReturn
OP2InterruptController::getInterruptType(IOService *nub, int source, int *interruptType)
{
	if (!interruptType) {
		return kIOReturnBadArgument;
	}
	*interruptType = kIOInterruptTypeLevel;
	return kIOReturnSuccess;
}

void
OP2InterruptController::initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	disableVectorHard(vectorNumber, vector);
}

void
OP2InterruptController::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	if (op2_gic_dist_base != 0 && vectorNumber < 1020) {
		volatile uint32_t *isenabler = (volatile uint32_t *)(op2_gic_dist_base + 0x0100);
		isenabler[vectorNumber / 32] = (1U << (vectorNumber % 32));
	}
}

void
OP2InterruptController::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *vector)
{
	if (op2_gic_dist_base != 0 && vectorNumber < 1020) {
		volatile uint32_t *icenabler = (volatile uint32_t *)(op2_gic_dist_base + 0x0180);
		icenabler[vectorNumber / 32] = (1U << (vectorNumber % 32));
	}
}

IOReturn
OP2InterruptController::handleInterrupt(void *refCon, IOService *nub, int source)
{
	if (source < 0 || source >= numVectors) {
		return kIOReturnBadArgument;
	}

	IOInterruptVector *vector = &vectors[source];
	if (!vector->interruptRegistered) {
		return kIOReturnInvalid;
	}

	vector->interruptActive = 1;
	if (!vector->interruptDisabledSoft) {
		vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
	} else {
		vector->interruptDisabledHard = 1;
		disableVectorHard(source, vector);
	}
	vector->interruptActive = 0;

	return kIOReturnSuccess;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
