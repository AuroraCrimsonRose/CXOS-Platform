# Drivers
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN.** How CXOS finds devices, binds drivers, and runs them.
> Compared against NT (WRK), XNU (IOKit, DriverKit), Haiku, Serenity and Redox.

---

## 1. Inherited

- `kmain.c` starts every driver in a fixed order.
- Each driver searches the PCI bus itself (for example `ahci.c` calls
  `pci_find`).
- `netif.c` tries e1000 then rtl8169 and uses the first found, polled.
- No device tree, no unloading, no general hot-plug, no power states.
- Everything is C in the kernel; a driver bug crashes the machine.
- Worth keeping: an operations table per device class (`netif_ops`,
  `struct disk`), working MSI and MSI-X, the reserved `.xkdr` type, signing.

## 2. How the others do it

| | Where drivers run | Matching | Lesson |
|---|---|---|---|
| NT | Kernel | PnP manager loads and calls add-device (`IopLoadDriver`) | Driver stacks and IRPs (`IoAttachDeviceToDeviceStack`, `IofCallDriver`) allow filter drivers, at great complexity. A signature permits anything, so old signed drivers with holes are an attack route. PnP and power came late. |
| XNU | Kernel, moving to user space (DriverKit, `IOUserServer`) | Matching tables with probe scores (`IOCatalogue::findDrivers`) | One registry tree; a clear lifecycle (`registerService`, `terminate`); entitlements per driver |
| Haiku | Kernel add-ons | `supports_device()` returns a score | Device node tree with rescan and removal (`device_manager.h`) |
| Serenity | Kernel, built in | A `probe()` per PCI driver (`Kernel/Bus/PCI/Driver.h`) | Simple; no isolation or hot-plug |
| Redox | User space | A PCI daemon with config files | Crash isolation; but `physmap` (`src/scheme/memory.rs`) lets a driver map any memory, and DMA is unconfined |

Everyone is moving drivers out of the kernel. Redox shows the trap: a user-space
driver with unlimited hardware access is not much safer than a kernel one.

## 3. Design

1. **A device tree with scored matching.** One tree of buses, devices and
   functions with properties, shown in the Process Manager. Drivers declare
   what they support in a table in a section of the driver file; the system
   binds them. The most specific driver wins; if it fails, the next-best
   takes over (the plain framebuffer is the display fallback).
2. **Drivers in user space by default, holding only their own hardware.** A
   driver process receives: its device's register ranges as memory-object
   handles (never arbitrary physical memory); its interrupt as a handle in the
   wait queue, its thread at real-time priority; DMA buffers typed `*dma`. With
   an IOMMU the device can reach only those buffers. A crashed driver is
   restarted by the supervisor and rebound. In the kernel stay CPU setup, the
   APIC and timer, PCI configuration access, the IOMMU, the panic console and
   what boot needs.
3. **Device classes as versioned contracts in X.** Block, network, input,
   display, audio, GPU. Shared work (block caching and partitions, handing
   packets to the stack) happens once in the class. **Filters by
   interposition with permission:** a process may serve a class interface over
   another driver only with a grant from the supervisor.
4. **Signing limits binding.** The signed manifest lists the device IDs and
   classes a driver may bind. A revocation list ships alongside.
5. **The full lifecycle from day one:** probe, start, stop, remove, suspend,
   resume.
6. **Drivers in X:** address types, fixed register layouts, `@isr` and
   `@nosleep` checking.
7. **Testing without hardware:** drivers talk only through handles, so a test
   hands one a simulated device.
8. **Visible health:** device, interrupt rate, DMA use and restart count per
   driver in the Process Manager.

**Cost:** a user-space driver adds a context switch per interrupt. Fast storage
and GPUs use multiple queues, batching and polling when busy.

**On the rig:** no IOMMU (`hardware/CX_TARGET_HARDWARE.md`), so drivers there
must be signed and trusted; isolation is built and tested under QEMU.

## 4. Order

1. Device tree, registry and scored matching; convert e1000, rtl8169, AHCI,
   ATA, USB and HID with no change in behaviour.
2. Class contracts for block, network and input; network moves to interrupts.
3. Driver-host primitives (needs memory objects and the wait queue).
4. First user-space driver: Bochs display; then input; then the NIC.
5. IOMMU under QEMU (AMD-Vi).
6. Hot-plug and power.
7. New drivers in X.
