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
- **Timekeeping.** The TSC is expected to run at a constant rate on this
  family; confirm with CPUID's invariant-TSC bit before relying on it.

## 3. Devices, and the driver each needs

| Device | Expected | Driver |
|---|---|---|
| SATA | SB710 AHCI | exists (`ahci.c`) |
| USB | OHCI and EHCI; no USB 3 | exist |
| Network | Realtek 8111 family | exists (`rtl8169.c` handles the 8111) |
| Audio | HD Audio with a Realtek codec | new (`system/CX_AUDIO.md`) |
| Display | the W5000's video BIOS gives VBE | the existing framebuffer path works today |
| GPU acceleration | GCN 1.0 | much later; see below |

## 4. The GPU

The W5000 is GCN 1.0. AMD published fairly complete documentation for this
generation, which helps a native driver. On Linux, amdgpu's support for it is
marked experimental and the older `radeon` driver is the default; Mesa's RADV
is expected to support it for Vulkan. A native driver is still a very large
project, and comes after virtio-gpu and Venus (`system/CX_DISPLAY.md` §8).
Check the licence of every file before reading Linux GPU driver code.
