# Target hardware
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: the machine CXOS will run on first, and what it decides.** Details
> marked *expected* come from the board's family, not from a datasheet, and
> are to be confirmed on the machine (`lspci`-style enumeration from CXK's own
> PCI scan is the check).

---

## 1. The rig

| Part | Model | Notes |
|---|---|---|
| CPU | AMD FX-8350 | Piledriver, family 15h, 8 cores, 2012. Long mode, NX, AVX, FMA, AES-NI. |
| Board | MSI MS-7641 v3.0 | MSI's 760GM series: AMD 760G with SB710 southbridge *(expected)* |
| RAM | 8 GB DDR3 | |
| GPU | AMD FirePro W5000 | GCN 1.0, "Southern Islands" (Pitcairn) |
| Firmware | BIOS | No UEFI |

## 2. What it decides

- **64-bit.** 8 GB and a 64-bit CPU: a 32-bit kernel would waste half the
  memory or need PAE. x86-64 is the primary target (roadmap §1).
- **BIOS boot is first-class.** The BIOS loader goes from real mode to long
  mode itself (`boot/CX_BOOT.md`).
- **SMP matters.** Eight cores are a reason to do SMP soon after 64-bit.
- **No IOMMU.** The 760G has no AMD-Vi, so on this machine a user-space
  driver's device can still write anywhere in memory through DMA. Drivers on
  the rig must be signed and trusted; DMA isolation is built and tested under
  QEMU's emulated AMD IOMMU (`kernel/CX_DRIVERS.md`).
- **CPU security features to probe, not assume.** No CET (needs Zen 3), no
  SMAP. SMEP is probably absent. The kernel reads CPUID and uses what exists.
- **A test machine, not the target.** Nothing is compiled for Piledriver;
  CXOS runs on any x86-64 CPU and detects features at boot
  (`kernel/CX_KERNEL_PLAN.md` §2.3).
- **Logs without serial.** A failed boot is read from the crash log, written
  at panic to a reserved area of the disk (not kept in RAM, which the BIOS may
  clear) and saved as a file on the next boot. Built for IDE and SATA disks
  (roadmap step 4b) and for the USB stick the rig boots from (4c), which is
  reset first if the panic caught it mid-transfer. Proven in QEMU on xHCI,
  EHCI and OHCI; not yet on the rig. The rig has no internal disk and an AMI
  BIOS from 2005: no UEFI. On any other machine, boot only a build with
  `CXK_AUTO_INSTALL=0` (the default): it writes to no disk but its own stick,
  which it knows by the id the loader read from it, even if the machine's own
  drive has CXOS on it too.
  The system log is saved to the stick as well, from the end of the boot on,
  and read off it on another machine with `cxk rig bootlog disk5`; this is how
  the rig was debugged (2026-10-24).
- **ACPI tables need an interpreter.** Interrupt routing (`_PRT`), sleep and
  reliable shutdown on this board are described in ACPI bytecode (AML).
  Booting does not need it (the legacy PIC works); SMP and proper device
  interrupts do (`kernel/CX_DRIVERS.md` §5).
- **Timekeeping.** The TSC is expected to run at a constant rate on this
  family; confirm with CPUID's invariant-TSC bit before relying on it.

## 3. Devices, and the driver each needs

| Device | Expected | Driver |
|---|---|---|
| SATA | SB710 AHCI | exists (`ahci.c`) |
| USB | OHCI and EHCI; no USB 3. On the rig: 2 EHCI, 5 OHCI companions | exist; the USB keyboard and mouse work on the rig (2026-10-24): low-speed devices go to the OHCI companion, and the ports are polled so a device that bounces back to EHCI during the companion's start-up, or is plugged in later, is found |
| Network | Realtek 8111 family | exists (`rtl8169.c` handles the 8111) |
| Audio | HD Audio with a Realtek codec | new (`system/CX_AUDIO.md`) |
| Display | the W5000's video BIOS gives VBE | the existing framebuffer path works today |
| GPU acceleration | GCN 1.0 | much later; see below |
| Wi-Fi and Bluetooth (an add-in card) | Cudy WE3000S: Intel AX210, Wi-Fi 6E on PCIe (`8086:2725`), Bluetooth as a USB device (`8087:0032`) through the card's cable to a USB 2.0 header | new; see below |

**The AX210 (Aurora, 2026-11-11).** Aurora has three Cudy WE3000S cards; one
is in the Ryzen 7 5800XT development PC, where Windows shows the Bluetooth
behind a USB 2.0 hub on the internal header (`05E3:0608`). Whether one goes
in the rig (a free PCIe x1 slot and a USB 2.0 header) is to confirm. The
chip has no Bluetooth path over PCIe: without the header cable Bluetooth is
gone and Wi-Fi still works.

- **Wi-Fi:** a user-space driver (`kernel/CX_DRIVERS.md`) written here.
  Intel publishes no programming manual; the references are OpenBSD's
  `iwx` (ISC), its port in Haiku
  (`src/add-ons/kernel/drivers/network/wlan/iaxwifi200/`) and Linux's
  `iwlwifi` (GPL-2.0 or BSD-3-Clause). The 802.11 management, our own
  WPA2/WPA3 supplicant (SAE included) and regulatory limits are the larger
  work (`system/CX_NETWORK.md`).
- **Bluetooth:** a USB device, so it sits on the existing USB stack (the
  rig's EHCI with OHCI companions is enough for it), then the HCI and the
  Bluetooth protocols. After Wi-Fi.
- **Open (Aurora to decide): the firmware.** Both halves need Intel's
  firmware, a redistributable binary that runs on the card, not the CPU.
  It would be vendored under `vendor/` as data with its licence read: the
  one closed piece in the image.

## 4. The GPU

The W5000 is GCN 1.0. AMD published fairly complete documentation for this
generation, which helps a native driver. On Linux, amdgpu's support for it is
marked experimental and the older `radeon` driver is the default; Mesa's RADV
is expected to support it for Vulkan. A native driver is still a very large
project, and comes after virtio-gpu and Venus (`system/CX_DISPLAY.md` §8).
Check the licence of every file before reading Linux GPU driver code.
