/*
 * Copyright (c) 2026. Port for OnePlus 2 (Qualcomm Snapdragon 810 / MSM8994).
 */

#ifndef _PEXPERT_ARM_MSM8994_H
#define _PEXPERT_ARM_MSM8994_H

#ifdef MSM8994
#include "arm64_common.h"
#endif

/*
 * Pure ARMv8.0-A boot at EL1: no TrustZone monitor calls, single-cluster bringup.
 * Bypasses all Apple-proprietary SPR traps (HID*, CYC_CFG).
 */
#define NO_MONITOR 1
#define NO_ECORE 1

#ifndef ASSEMBLER

#define MSM8994_VTIMER_INTID		20
#define MSM8994_TIMEBASE_FREQ		19200000

/* Qualcomm MSM8994 GICv2 Memory Mapped Registers */
#define MSM8994_GICD_BASE		0xf9000000
#define MSM8994_GICC_BASE		0xf9002000
#define MSM8994_GICC_IAR_OFFSET		0x000c
#define MSM8994_GICC_EOIR_OFFSET	0x0010

/* Qualcomm eMMC Host Controller (SDHCI) */
#define MSM8994_SDHCI_BASE		0xf9824900
#define MSM8994_SDHCI_CORE_BASE		0xf9824000

/* Panic buffer carved from top of DRAM */
#define PLATFORM_PANIC_LOG_PADDR	0xabf00000
#define PLATFORM_PANIC_LOG_SIZE		0x100000	/* 1 MiB */

/* Ramoops persistent console buffer */
#define RAMOOPS_CONSOLE_PADDR		0xac0be000
#define RAMOOPS_CONSOLE_SIZE		0x40000		/* 256 KiB */

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM_MSM8994_H */
