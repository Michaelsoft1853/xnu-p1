/*
 * OP2Watchdog.cpp - Built-in IOWatchdog for OnePlus 2
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>

class IOWatchdog : public IOService {
	OSDeclareDefaultStructors(IOWatchdog);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdog, IOService);

bool
IOWatchdog::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOWatchdogUserClient");
	registerService();
	IOLog("OP2 WDT: IOWatchdog registered (accepts check-ins, never fires)\n");
	return true;
}

class IOWatchdogUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOWatchdogUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdogUserClient, IOUserClient);

IOReturn
IOWatchdogUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

static uint64_t op2_wdt_logged;

IOReturn
IOWatchdogUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		IOByteCount len = args->structureOutputDescriptor->getLength();
		for (IOByteCount off = 0; off < len; off += 8) {
			static const uint64_t zero = 0;
			args->structureOutputDescriptor->writeBytes(off, &zero, len - off < 8 ? len - off : 8);
		}
	} else if (args->structureOutput != NULL && args->structureOutputSize > 0) {
		memset(args->structureOutput, 0, args->structureOutputSize);
	}

	if (selector < 64 && !(op2_wdt_logged & (1ULL << selector))) {
		op2_wdt_logged |= (1ULL << selector);
		char name[32] = {0};
		proc_selfname(name, sizeof(name));
		IOLog("OP2 WDT: %s check-in sel %u\n", name, selector);
	}
	return kIOReturnSuccess;
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
