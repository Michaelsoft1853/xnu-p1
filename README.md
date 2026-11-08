# XNU Kernel for OnePlus 2 (Qualcomm Snapdragon 810 / MSM8994)

This repository contains a specialized fork of Apple's open-source **Darwin/XNU kernel** (`xnu-4903.270.47`, macOS 10.14.6 / iOS 12.5.7 era), re-engineered to boot on the **OnePlus 2** (`oneplus2` / `ONE A2003`, Qualcomm Snapdragon 810 / MSM8994) and drive real Apple iOS 12.5.7 userland (`16H81`, `n56ap` iPhone 6 Plus).

---

## Hardware Architecture & Platform Adaptations

| Subsystem | Apple Stock Target (A8 / T7000) | OnePlus 2 Adaptation (`ARM64_BOARD_CONFIG_MSM8994`) |
| :--- | :--- | :--- |
| **Translation Granule** | 16 KB pages (`ARM_PGSHIFT=14`) on >1GB DRAM | **Native 4 KB pages (`ARM_PGSHIFT=12`)** across 3-level translation tables |
| **Physical Memory** | Fixed Apple carveouts (`0x800000000`+) | Mapped to Qualcomm DRAM bank `0x10200000`–`0xAC000000` (~2,403 MiB managed) |
| **Interrupt Controller** | Apple AIC / proprietary FIQ | **ARM GICv2** MMIO distributor (`0xf9000000`) and CPU interface (`0xf9002000`) |
| **Hardware Timer** | Apple decremeter | **ARM Generic Virtual Timer** (`CNTV_CTL_EL0`, PPI 20, 10 ms periodic ticks) |
| **Multiprocessing (SMP)**| Apple-proprietary CPU power sequencer | **Dual-core SMP (`cpus=2`)** on Cortex-A53 Little cluster via mailbox and PSCI `CPU_ON` |
| **Storage Controller** | Apple NVMe / NAND controller | **Qualcomm SDCC 5.0 / SDHCI eMMC 5.0** (`bsd/dev/op2_emmc.c`) with conditional R/W |
| **Filesystem** | APFS / HFS+ kext | **Built-in Apple HFS+ driver (`bsd/op2_hfs/`)** with macOS 10.14 compatibility KPIs |
| **Display Pipeline** | AppleCLCD / IOMobileFramebuffer | **MDSS MDP5 command-mode panel** (1080×1920) with dynamic surface routing & zero-fill freeze |
| **Touch & Navigation** | Apple Multitouch SPI | **Synaptics S3320 I2C** + dedicated **`OP2Buttons` Consumer HID** (0x0C / 0x40) |
| **Memory Governance** | Restrictive 36MB legacy daemon profiles | **256MB task footprint floor** (`osfmk/kern/task.c`) protecting daemons on 4GB RAM |

---

## Key Driver Subsystems & Modifications

### 1. Dual-Core Multiprocessing (`osfmk/arm64/start.s`, `osfmk/arm/arm_init.c`, `iokit/Kernel/arm/OP2CPU.cpp`)
- Secondary Cortex-A53 core (CPU 1) spun up via mailbox spin-table synchronization (`0x00080000` / `0x00080008`) and ARM PSCI `CPU_ON` SMC call (`0xC4000003`).
- Secondary entry point `arm_init_secondary` in `start.s` initializes CPU 1 architectural registers, MMU, and local GICv2 interface.
- Preemptive Mach scheduler running multi-core threads; GICv2 SGI 0 used for cross-core TLB invalidations and AST scheduling.
- Reduces boot time to SpringBoard from 30–80 minutes down to ~5–6 minutes.

### 2. SDHCI eMMC 5.0 Persistent Storage (`bsd/dev/op2_emmc.c`)
- Custom Darwin block and character driver (`/dev/emmcroot0`, `/dev/remmcroot0`).
- **Conditional Read/Write:** Defaults to read-only safety mode; write support is enabled conditionally via boot arguments (`emmcrw=1` or `rootrw=1`).
- Hardware `CMD12` (STOP_TRANSMISSION) abort handling on timeouts, explicit DAT0 line polling, and automatic 3x retries on CRC or timeout faults.
- 0 failures across 1.36M+ operations in prolonged soak runs.

### 3. Display Engine & Zero-Fill Preservation (`iokit/Kernel/arm/OP2Framebuffer.cpp`, `OP2Surface.cpp`)
- Interfaces with Qualcomm continuous splash buffer at `0x03401000` (1080×1920, 32-bit ARGB/BGRA, 4352-byte padded stride).
- Scans userland surfaces up to ID 63, dynamically tracing SpringBoard's double-buffered Retina frames (1242×2208) and bilinearly downsampling to native 1080×1920.
- **RGB Alpha Mask Fix:** Evaluates `(row[x] & 0x00FFFFFF) != 0` to differentiate opaque black (`0xFF000000`) from uninitialized buffers.
- **Zero-Fill Guard:** Freezes existing panel memory (`fPixels`) during blank composite swaps, ensuring 100% of sampled panel pixels remain non-zero (`panel_nz = 2000/2000`).

### 4. Dedicated `OP2Buttons` Consumer HID Service (`iokit/Kernel/arm/OP2Digitizer.cpp`)
- Declares a standalone `IOHIDEventService` publishing Consumer/ConsumerControl (Page `0x0C`, Usage `0x40`).
- Binds directly to SpringBoard's Mach runloop notification port, allowing hardware capacitive button events and simulated AUTOHOME pulses to wake the lockscreen and navigate to the application icon grid.

### 5. Memory Management & Jetsam Floor (`osfmk/kern/task.c`)
- Enforces a minimum 256MB physical footprint floor for tasks under `ARM64_BOARD_CONFIG_MSM8994`.
- Prevents `memorystatus` jetsam from terminating `assistivetouchd` under legacy 36MB iPhone 6 Plus memory limits.

---

## Building the Kernel

### Prerequisites
* macOS host running macOS 10.14 through 14.x.
* Xcode 10.3 clang toolchain (or compatible Apple clang targeting `aarch64-apple-darwin`).
* Homebrew LLVM (`clang`, `ld.lld`, `llvm-objcopy`).

### Compilation

From the root project directory:
```bash
./tools/build-xnu.sh
```

Or invoke `make` directly within the XNU directory:
```bash
make SDKROOT=macosx.internal \
     ARCH_CONFIGS=ARM64 \
     KERNEL_CONFIGS=DEVELOPMENT \
     MACHINE_CONFIGS=MSM8994
```

The compiled kernel binary is produced at:
```text
BUILD/obj/DEVELOPMENT_ARM64_MSM8994/kernel.development.msm8994
```

---

## Directory Organization

* `bsd/dev/op2_emmc.c` — Qualcomm SDHCI eMMC 5.0 block device driver.
* `bsd/op2_hfs/` — Apple HFS+ filesystem driver and macOS 10.14 shims.
* `iokit/Kernel/arm/OP2CPU.cpp` — SMP CPU nub and secondary core lifecycle management.
* `iokit/Kernel/arm/OP2Framebuffer.cpp` — Qualcomm MDSS MDP5 framebuffer and `AppleCLCD` pipeline.
* `iokit/Kernel/arm/OP2Surface.cpp` — Dynamic surface tracking and 1242×2208 downsampling engine.
* `iokit/Kernel/arm/OP2Digitizer.cpp` — Synaptics touch and `OP2Buttons` Consumer HID service.
* `iokit/Kernel/arm/OP2I2C.cpp` — Qualcomm BLSP I2C bus driver.
* `iokit/Kernel/arm/OP2InterruptController.cpp` — ARM GICv2 interrupt controller driver.
* `iokit/Kernel/arm/OP2PlatformExpert.cpp` — `N56AP` device tree matching and early bootstrap.
* `osfmk/arm64/start.s` — Early ARM64 entry vector, MMU bootstrap, and CPU 1 secondary entry.
* `osfmk/arm/arm_init.c` — Architecture initialization and 4KB translation table setup.
* `osfmk/arm64/op2_snapshot.c` — Pre-reset daemon thread state and dyld snapshot capture.
* `osfmk/arm64/sleh.c` — ARM64 exception vectors, GICv2 timer dispatch, and user state capture.
* `pexpert/pexpert/arm64/MSM8994.h` — Register maps for MSM8994 GIC, timer, SDCC, and TLMM.

---

## License

This project is licensed under the terms of the **Apple Public Source License (APSL 2.0)**. See the `LICENSE` file for full terms and conditions.
