# Security
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN, for x86-64.** Who CXOS defends against, what it protects,
> and the layers that do it, each pointing at the design document that builds
> it. The x86-32 review findings and their fixes are in
> `planning/HARDENING_PLAN.md` and `reviews/`, which stay as written; their
> open items (key rotation, X's safety contract, concurrent IPC tests) are
> carried here.

---

## 1. What is protected

1. **The user's data:** documents, settings, secrets.
2. **The user's privacy:** keystrokes, screen, microphone, camera, location,
   network activity.
3. **The system's integrity:** the kernel, `/System`, the boot chain, the
   signing keys.
4. **The user's control:** what is running, what each app may do, and the
   ability to stop it.

## 2. Who CXOS defends against

| Attacker | Example | Main defences |
|---|---|---|
| **A malicious or compromised app** | An app that tries to read another's files or keys | Handles only (§3.1), per-app folders, the picker, input to the focused window only, permissions (§3.6) |
| **The network** | Crafted packets, a hostile server | Network stack out of the kernel, per-app network permission, system TLS (`system/CX_NETWORK.md`) |
| **Hostile media and files** | A crafted USB stick, disk image, font or document | Foreign filesystems and parsers out of the kernel; the origin marker on downloads; fuzzing (§3.10) |
| **A buggy or malicious driver** | A driver writing outside its device | User-space drivers holding only their device; signed drivers limited to declared devices (`kernel/CX_DRIVERS.md`) |
| **The user being tricked** | A fake password dialog, an app pretending to be the system | Trusted UI (§3.7), signer shown in title bars, consent screens no app can draw |
| **The supply chain** | A tampered update, a stolen signing key | Signed everything, rollback protection, key separation and revocation (§3.9) |
| **Physical access** | A stolen disk; a tampered loader | Encryption (§3.8); verified `/System`. **Gaps on the rig, stated below.** |

**Out of scope for now:** hardware side channels beyond the standard
mitigations, and attackers who can reflash firmware.

### Gaps on the rig, stated plainly

The rig (`hardware/CX_TARGET_HARDWARE.md`) lacks most hardware security
features, so on it:

- **No verified boot.** BIOS cannot check the loader, so someone with physical
  access can replace it. The loader still checks the kernel, and the kernel
  every program, but the first link is unprotected. UEFI machines with Secure
  Boot close this (CXOS's own Secure Boot keys already work).
- **No IOMMU.** A device can write anywhere in memory by DMA, so drivers on
  the rig must be trusted, not merely isolated.
- **No TPM.** Disk keys come from the user's password only; nothing can
  detect a tampered boot before the password is typed.
- **No SMEP, SMAP or CET.** The kernel uses them where they exist.

CXOS shows this to the user (§3.11) rather than implying protection it does
not have.

## 3. The layers

### 3.1 Authority: handles only

The roadmap's first principle. Nothing is done to an object except through a
handle with the right; rights only shrink when passed; nothing is inherited
unless named; each handle records the grant it came from, so authority can be
traced and revoked (`kernel/CX_KERNEL_PLAN.md` §2.1, `kernel/CX_SYSCALLS_IPC.md`).
The `GRANT_*` bits remain as a coarse outer limit.
`GRANT_DISK` and `GRANT_IOPORT` become handles to one device or one port
range.

### 3.2 The kernel

- **Less of it.** Network stack, foreign filesystems, drivers, drawing and
  input routing all move to user space. What stays is what must.
- **Exploit mitigations,** each turned on where the CPU allows (`kernel/
  CX_KERNEL_PLAN.md` §2.3, §8): NX everywhere, W^X with no exceptions, KASLR,
  the stack protector, guard pages on every stack, SMEP and SMAP, kernel
  pointers never shown to programs, page-table isolation only on CPUs that
  need it, control-flow integrity later (CET where present).
- **Type-segregated heap** (`kernel/CX_KERNEL_PLAN.md` §9.2), so a
  use-after-free cannot be reused as a different type.
- **A random number generator.** The kernel has no entropy source today, yet
  KASLR, the stack protector, TCP sequence numbers and keys all need one.
  Seeded from the loader (`cxk_boot_info.rng_seed`), RDRAND/RDSEED where
  present, and interrupt timing; a ChaCha20-based generator; `random_read`
  for programs.
- **A syscall fuzzer generated from `.xfil`.** The interface definition knows
  every call's arguments, types and handle rights, so the fuzzer is generated,
  not written (as syzkaller is driven by syscall descriptions). Run on every
  ABI change.

### 3.3 Code signing and loading

Kept from x86-32: every executable is signed; the loader verifies the
signature before anything else; a valid signature from an untrusted key runs
nothing (`x86_32/formats/CX_KEY_FORMAT.md`). Added:

- **The loader verifies the kernel** (`boot/CX_BOOT.md` §3.4).
- **What a signer may ask for is bounded by the signer.** The platform key
  may grant anything; a publisher key only what its certificate allows.
- **Revocation and rotation** (the open x86-32 item): a signed revocation list
  shipped as part of `/System`, and a generation counter in each key, so a
  stolen key can be refused without rebuilding the kernel.
- **Ed25519 instead of RSA-2048.** Smaller keys and signatures, faster
  verification, and simpler constant-time code. A post-quantum signature
  (ML-DSA) alongside it is for the ecosystem phase.

### 3.4 Isolation of apps

- Each app sees only its own folder; other files come through the picker
  (`system/CX_FILESYSTEM.md` §5).
- Input reaches only the focused window; screen capture and overlays need a
  capability (`system/CX_DISPLAY.md`).
- Jobs bound memory, threads and disk (`kernel/CX_KERNEL_PLAN.md` §4).
- The **input hole** (`SYS_INPUT_READ`, `SYS_INPUT_POLL`, `SYS_MOUSE_READ` need no capability)
  is closed early, independently of everything else.

### 3.5 Memory safety

- **New services are written in X** once it has bounds-checked slices,
  overflow traps and non-null pointers (`language/CX_X_PLAN.md` §3). Parsers
  first: network, fonts, images, foreign filesystems.
- **X's safety contract** is written down (the open x86-32 item): what X
  guarantees and what remains the programmer's responsibility, especially
  for `*phys` and `*dma` pointers.
- **The C kernel shrinks** as services move out, and is fuzzed (§3.2, §3.10).

### 3.6 Permissions and privacy

- **An app's package declares what it needs:** network (none, local,
  internet, named hosts), camera, microphone, screen capture, location,
  background running, and the folders it wants beyond its own.
- **The user grants each at install or on first use,** through the trusted
  UI, and can revoke any at any time.
- **Indicators** while the microphone, camera or screen capture is in use,
  drawn by the display server where no app can cover them.
- **Attribution:** every permission use is logged with the app that made it,
  and the Process Manager shows it.
- **No keystroke monitoring permission exists.** Accessibility tools get a
  separate, prominently shown capability.

### 3.7 Trusted UI

From `system/CX_DISPLAY.md`: a key combination no app can intercept that
always reaches the system; consent screens and password prompts drawn only by
the system; the signer in every title bar. A password is never typed into an
app's window to raise privilege.

### 3.8 Accounts, secrets and data at rest

- **Accounts and login** (none exist today): passwords hashed with Argon2id;
  rate limiting.
- **Raising privilege** is a consent screen in the trusted UI, never a
  password typed into a terminal.
- **A secrets service** (a keychain): each app stores secrets only it can
  read, encrypted with the user's key.
- **Encryption at rest:** per-user keys from the password, a key per file,
  metadata authenticated so offline tampering is caught before it is parsed
  (`system/CX_FILESYSTEM.md` §9).

### 3.9 Updates, keys and the build

- Updates are signed system layers; rollback is automatic on a failed boot
  (`boot/CX_BOOT.md`), and **downgrade protection** refuses an older signed
  layer than the newest one installed, so an attacker cannot reinstall a
  version with a known flaw.
- **Key separation:** an offline root key that signs only other keys;
  separate keys for the kernel, the OS, and each publisher. Today the platform
  key signs the kernel and every OS component.
- **Release signing** (open from x86-32): signed locally, never by a CI
  secret.
- **Deterministic builds** (already true) let anyone rebuild a release and
  compare it. A public signing log is for the ecosystem phase.

### 3.10 Hostile input

- Every parser of outside data (packets, filesystems, fonts, images, CXEX
  headers, X Data, archives) builds for the host and is **fuzzed** in the
  DevKit, with its corpus kept in the repository.
- **Downloads carry an origin marker;** opening one for the first time shows
  where it came from and who signed it (roadmap §5).
- Files are identified by their header, never by their extension
  (`formats/CX_FILE_TYPES.md`).

### 3.11 Visible security state

A page in Settings lists every protection and whether it is active on this
machine: verified boot, IOMMU, SMEP/SMAP, encryption, CET, and so on, with
the reason when one is not. On the rig most hardware lines read "not
available on this hardware".

### 3.12 Audit

A security log of grants, denials, permission uses, signature failures and
privilege changes. Each entry includes the hash of the previous one, so a
deleted or altered entry shows. It survives reboots with the crash log.

## 4. How it is verified

Carried from x86-32 and unchanged: **every check is proven by breaking it**
(a sabotage that must turn exactly the covering tests red); a test on a
policy is not a test that its caller applies it; independent security
reviews go in `reviews/` and are tracked here. Added: the generated syscall
fuzzer, parser fuzzing, the crash-consistency harness, and a self-test for
each mitigation that confirms it is actually on (a write to a read-only
kernel page must fault, and so on).

## 5. Order

| When | Work |
|---|---|
| Now, independent | Close the input hole; the stack protector; the kernel random number generator |
| Phase 0 | Handles with rights and the grant chain; type-segregated heap; the loader verifies the kernel; NX, W^X, KASLR, SMEP/SMAP where present; mitigation self-tests |
| Phase 0.6 | The system page (syscalls only from it); the ABI hash; the generated syscall fuzzer |
| Phase 1 | Jobs and budgets; `GRANT_DISK` and `GRANT_IOPORT` become handles; Ed25519, revocation and key generations |
| Phase 2 | User-space drivers; trusted UI; input to the focused window; capture capabilities and indicators |
| Phase 3 | Per-app folders and the picker; the `/System` hash tree; downgrade protection; key separation |
| Phase B | Accounts and login; the secrets service; package permissions; network permission; the security log; the Settings page |
| Later | Encryption at rest; CET and CFI; IOMMU isolation; ML-DSA; a public signing log |
