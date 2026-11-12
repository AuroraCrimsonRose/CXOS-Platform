# System monitor
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN 2026-11-11, nothing built beyond the console Process
> Manager** (roadmap 2.7c: `SYS_PROCESS_QUERY`, `GRANT_INSPECT`,
> `procman`). What the Process Manager's performance tab shows, where each
> value comes from on the rig, and the rules for reading hardware.

---

## 1. What it shows, and the source on the rig

| Area | Values | Source |
|---|---|---|
| CPU | usage total, per core and per module (the FX-8350's 8 cores in 4 modules); each core's current clock; L1, L2, L3 sizes; model and features | the kernel's per-CPU time; the P-state MSRs (or APERF/MPERF, which the rig has: `hardware/CX_HW_REPORT.md`); CPUID cache leaves |
| CPU temperature | AMD's reported value (`Tctl`) | the northbridge's PCI function 3. On family 15h it is a control value, not degrees: labelled so, as Linux's `k10temp` does |
| Fans, board temperatures, voltages | fan speeds, board and system temperatures, rails | the board's Super I/O monitoring chip, chip by chip; the MSI MS-7641's is identified on the rig first |
| Memory | total, in use, cached, kernel, shared memory objects; per module size, speed, timings, maker, slot | the PMM; SMBIOS type 17; each module's SPD over the SMBus |
| Board and firmware | maker, model, BIOS version and date | SMBIOS types 0, 1, 2 |
| Disks | read and write rates, busy time, queue depth; per disk model, size, health, temperature, power-on hours | the AHCI driver; ATA IDENTIFY and SMART |
| Network | send and receive per adapter, link speed, MAC, addresses; per-app traffic | the network service (`CX_NETWORK.md` §4 item 2) |
| GPU | VRAM size now; usage and temperature with a real driver | the BARs; later the W5000's own controller |
| Audio | underruns per stream | `CX_AUDIO.md` §2a item 1 |
| System | uptime; processes, threads, handles; interrupts and context switches per second, per CPU; page faults | the kernel's counters |

## 2. The performance tab (Aurora, 2026-11-11)

- **Every item can be shown or hidden** except overall CPU, memory, disk and
  network usage, which always show. The same per device: one adapter, one
  disk, one disk's temperature.
- The refresh interval and the length of each graph's history are settings.
- **Copy system info:** a text report for bug reports.
- **Alerts,** optional: a notification above a chosen temperature, or when a
  fan stops.
- Survey first: Haiku's ActivityMonitor, Serenity's SystemMonitor, Windows
  Task Manager's Performance tab.

## 2b. The processes and services tabs (Aurora, 2026-11-11)

"Needs to show running services and such too and how much resources every
program and service uses."

- **Processes:** every program, the system's own included, grouped under
  its app or service (a job, `process_stat.job`), with a total per group.
  Columns: CPU share, memory (in use and quota), disk and network rates,
  threads, handles, signer and trust, the pid that started it. GPU when
  there is a GPU to count. Sortable; a column can be hidden.
- **Services:** every service the supervisor knows from its `.xosv`, run
  or not: its name and description, state (running, stopped, failed,
  disabled), how it starts, its pid, how often it restarted and why it
  last ended, its grants, and the same resource columns as its process.
  Start, stop and restart are the supervisor's to grant, not
  `GRANT_INSPECT`'s (which only reads).
- Where each number comes from: CPU, memory, threads and handles are in
  `process_stat` today (`SYS_PROCESS_QUERY`, 2.7c). Per-process disk and
  network bytes are not counted anywhere yet: the filesystem and network
  services count them per client, by the badge the client called with. A
  service's state and restart count are the supervisor's, through a query
  on its endpoint (not built).
- Survey first: Serenity's SystemMonitor, Haiku's ProcessController and
  ActivityMonitor, Windows Task Manager's Processes, Details and Services
  tabs.

## 3. Rules

1. **Read-only first.** A hardware-monitor service in user space reads the
   sensors with narrow grants: the Super I/O's ports, the SMBus, read-only
   MSRs. SPD reads go only to the modules' addresses (`0x50`-`0x57`): a write
   at the wrong SMBus address can ruin a memory module.
2. **Fan control is later, and its own grant,** with the consent screen and
   a floor it cannot go below; if the service dies, the fans return to the
   board's automatic control.
3. **Hardware detail is a privilege.** Exact models fingerprint a machine.
   The Process Manager (platform-signed) sees all of it; other apps get
   coarse values unless granted.
4. **Tested in the boots, every reading sabotaged.** QEMU's `q35` gives
   SMBIOS (`-smbios` sets the tables) and, I believe, SPD EEPROMs on the
   ICH9 SMBus (to check); a virtual sensor device stands in for fans and
   temperatures. A malformed table or a bad SPD checksum must be refused.

QEMU's Intel parts beside `q35` that help here and elsewhere: `intel-hda`
(the audio plan's first driver), `intel-iommu` (`kernel/CX_DRIVERS.md` §4
item 5), `e1000e` and `igb` if more network drivers are wanted, and Intel
CPU models (`-cpu Skylake-Client` and the like) under TCG only, since the
development PC is AMD.

## 4. Order

1. **SMBIOS from the BIOS loader.** Today only the UEFI stub looks for it
   (`hardware/CX_HW_REPORT.md`: "SMBIOS 0"); the loader scans for it as it
   scans for the RSDP and fills CXBI's field, and the kernel checks it.
2. CPU and memory with the Process Manager's window (2.8a's second
   program): per-core usage, clocks, caches, SMBIOS memory; the processes
   tab, grouped by job; the services tab, once the supervisor answers a
   query (§2b).
3. Disks (SMART) and network with their services.
4. SPD, the Super I/O chip and `Tctl` on the rig; alerts.
5. Fan control, then the GPU.

The rig has 4 GB (it had 8 earlier), and its E820 map agrees: 3.98 GiB
usable (`hardware/CX_HW_REPORT.md`). SMBIOS's slot list will show which
slots are empty.
