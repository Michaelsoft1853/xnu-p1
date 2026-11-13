/*
 * H815 platform facts for Qualcomm MSM8992 (Snapdragon 808).
 * Values are from the published H815 Linux device tree and must be checked
 * against the target phone's live FDT before a boot image uses them.
 */

#ifndef _PEXPERT_ARM_MSM8992_H
#define _PEXPERT_ARM_MSM8992_H

#ifdef MSM8992
#include "arm64_common.h"
#endif

#define MSM8992_VTIMER_INTID             20
#define MSM8992_TIMEBASE_FREQ            19200000
#define MSM8992_CPU_COUNT                6

/* H815 GICv2, as described by the published MSM8992/H815 device trees. */
#define MSM8992_GICD_BASE               0xf9000000
#define MSM8992_GICC_BASE               0xf9002000
#define MSM8992_GICC_IAR_OFFSET         0x000c
#define MSM8992_GICC_EOIR_OFFSET        0x0010

/* H815 SDHC1/eMMC host and core register blocks. */
#define MSM8992_SDHCI_BASE              0xf9824900
#define MSM8992_SDHCI_CORE_BASE         0xf9824000

#endif /* _PEXPERT_ARM_MSM8992_H */
