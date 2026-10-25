/*
 * Minimal device-tree platform expert for OnePlus 2 (MSM8994).
 * Uses XNU's normal platform, power-domain and nub initialization.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
extern "C" {
#include <prng/random.h>
void ml_init_max_cpus(unsigned int max_cpus);
}

class OP2PlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(OP2PlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	bool getModelName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
	bool getMachineName(char *name, int maxLength) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(OP2PlatformExpert, IODTPlatformExpert);

bool
OP2PlatformExpert::start(IOService *provider)
{
	IOLog("OP2: starting device-tree platform expert\n");
	op2_prng_bootstrap();
	ml_init_max_cpus(1);
	if (!IODTPlatformExpert::start(provider)) {
		IOLog("OP2: IODTPlatformExpert::start failed\n");
		return false;
	}
	publishResource("IORTC", kOSBooleanTrue);
	registerService();
	IOLog("OP2: platform expert registered successfully\n");
	return true;
}

const char *
OP2PlatformExpert::deleteList(void)
{
	return "()";
}

const char *
OP2PlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
OP2PlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * Stock IODTPlatformExpert requires an Apple NVRAM /options node.
	 * OP2 has no Apple NVRAM controller; publish real CPU and device nodes
	 * directly without failing. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));
}

bool
OP2PlatformExpert::getModelName(char *name, int maxLength)
{
	if (name == NULL || maxLength <= 0) {
		return false;
	}
	strlcpy(name, "iPhone7,1", (size_t)maxLength);
	return true;
}

bool
OP2PlatformExpert::getMachineName(char *name, int maxLength)
{
	if (name == NULL || maxLength <= 0) {
		return false;
	}
	strlcpy(name, "iPhone 6 Plus", (size_t)maxLength);
	return true;
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
