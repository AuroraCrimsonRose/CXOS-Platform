# Safety: how CXOS knows it is correct
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN.** How correctness is established, from tests up to
> mathematical proof, and which properties get which. Security
> (`security/CX_SECURITY.md`) is about attackers; this document is about the
> code being right whether or not anyone attacks it. Most security failures
> start as a safety failure, so the two share many tools.

---

## 1. Where it stands

| Tool | Today |
|---|---|
| Boot self-tests | 47, run on every boot of a test build (`kernel/ktest*.c`) |
| Host tests | 164 xUnit tests in `devkit/CXEX.Tests`: unit, adversarial (hostile CXEX, ELF and GPT inputs), differential |
| Differential testing | The C# compiler is the oracle for `os/xc`; the self-hosting fixed point is checked |
| Sabotage | Every check must be seen to fail; done by hand |
| Determinism | Builds are byte-for-byte reproducible |
| Kernel C on the host | Since 2026-10-16: `kernel/test/host`, under AddressSanitizer and UBSan, run by `dotnet test` (§3.7). Covers the bitmap and SHA-256 so far |
| Fuzzing, model checking, proofs | None |

The discipline is unusually good. The tools stop at example-based tests: each
test checks the cases someone thought of.

## 2. The ladder

Each rung covers what the one below cannot. Higher rungs cost more, so each
property gets the lowest rung that settles it (§5).

| Rung | What it establishes | Tools |
|---|---|---|
| 1. Example tests | The cases someone thought of work | Boot self-tests, xUnit |
| 2. Property tests | A stated property holds for thousands of generated inputs, with failures shrunk to the smallest case | CsCheck (C#); a small generator library in X |
| 3. Fuzzing | Hostile and random input finds crashes and hangs no one imagined | libFuzzer through clang, on the host; the syscall fuzzer generated from `.xfil` |
| 4. Model checking | Every interleaving of a concurrent protocol, up to a bound, is safe and live | TLA+ with the TLC model checker |
| 5. Bounded proof of C | For all inputs up to a size: no overflow, no out-of-bounds access, assertions hold | CBMC |
| 6. Full proof | For **all** inputs, the code meets its specification | Frama-C/WP for C; verified libraries (HACL\*); later, X contracts proved by SMT |

**What full proof costs.** seL4, the one general-purpose kernel proved
correct end to end, took on the order of 20 person-years for about 10,000
lines of C. CXOS will not prove the whole kernel. It proves the small parts
everything else rests on, model-checks the protocols, and tests the rest hard.
That is the approach AWS and Microsoft use in production (TLA+ for protocols,
CBMC for critical C).

## 3. The tools, and where each applies

### 3.1 Property tests

For pure, deterministic code: parsers and writers (CXEX, CXFS, X Data, XFNT,
GPT), the allocators against a simple model, rights arithmetic, path
handling. Typical properties: writing then parsing returns the original; the
buddy allocator never hands out a page twice; a duplicated handle never has
more rights than its source.

### 3.2 Fuzzing

- **Every parser of outside data builds for the host** and has a libFuzzer
  target with sanitizers on (AddressSanitizer, UndefinedBehaviorSanitizer):
  CXEX, CXFS images, packets, fonts, images, X Data, archives. Corpora live in
  the repository; any crash found becomes a regression test.
- **The syscall fuzzer** is generated from the `.xfil` definitions, which
  know every argument's type and every handle's rights (syzkaller is driven by
  syscall descriptions the same way). It runs inside QEMU against a debug
  kernel.
- **The X compilers** are fuzzed with random valid programs; the C# compiler
  and `xc` must agree (`language/CX_X_PLAN.md` §6).

### 3.3 Model checking with TLA+

The bugs that tests miss are interleavings: two CPUs, a dying process, a
timeout arriving at the same moment as a reply. A TLA+ specification describes
the protocol, and TLC explores every interleaving up to a bound, checking
safety ("a reply token is used at most once") and liveness ("a waiter is
eventually woken"). Specifications live in `spec/` beside the code they
describe, and run in the CI container.

First specifications: the IPC call and reply protocol; the wait queue; futex
wait and wake; handle lifetime and reference counts; stop and kill; the CXFS
v3 commit; the boot rollback counter.

### 3.4 Proof

- **Bounded proofs of C with CBMC:** the user-pointer range check (no
  address wrap, never reaching the kernel half), rights attenuation, the CXEX
  header and section bounds checks, the buddy allocator's split and merge, the
  handle table's generation check. Each harness states the property as
  assertions; CBMC proves it for every input up to the bound, or produces the
  counterexample.
- **Deductive proof with Frama-C/WP** for the few functions worth it after
  CBMC: those where a bound is not enough.
- **Verified cryptography.** Signature checking is the root of the trust
  chain. HACL\* provides C implementations of Ed25519, SHA-2 and ChaCha20 that
  are machine-proved memory-safe, functionally correct and free of
  secret-dependent timing. They replace the hand-written RSA verifier under
  the Embed rule (`security/CX_SECURITY.md` §3.3).
- **X contracts** (`language/CX_X_PLAN.md` §5) are checked at run time first;
  proving them with an SMT solver, as SPARK does for Ada, is the long-term
  route to proofs over X code.

### 3.5 Runtime checking

Debug kernels run with: assertions on every invariant; a shadow-memory
checker for out-of-bounds and use-after-free (as Linux KASAN); UBSan; the
lock-order checker; poisoned frees. Release kernels keep the cheap invariant
checks; a failed invariant stops the kernel with a crash log rather than
continuing in an unknown state.

### 3.6 Static analysis

`-Werror` with a chosen warning set; the clang static analyzer and
clang-tidy on the kernel in the container; a short list of banned C functions
and patterns. In X, the language rules (`language/CX_X_PLAN.md` §5)
carry most of this.

### 3.7 Kernel code tested on the host

Today kernel code is tested only by booting. Code with no hardware
dependency (handle tables, rights arithmetic, allocators, the buddy and slab
logic, path handling, parsers) is also compiled for the host with clang and
run as unit tests under AddressSanitizer and UndefinedBehaviorSanitizer,
in seconds rather than a boot.

- A small C test runner (`kernel/test/host/`), driven by CMake and `cxk test
  host`; hardware is stubbed at a narrow seam (page allocation, locks, the
  clock), never mocked function by function.
- The same files are the CBMC harnesses (§3.4) and the libFuzzer targets
  (§3.2), so one build target serves three rungs of the ladder.
- The boot self-tests remain: they are what proves the code works on the
  machine. Host tests prove its logic; boot tests prove its integration.
- Built **before Phase 0 step 1**, so the object model is written against it.

## 4. Test discipline

1. **A test that passes tells nothing until it has been seen to fail.** Kept
   from x86-32, unchanged.
2. **A test on a policy is not a test that its caller applies it.** Kept.
3. **No flake is tolerated.** An intermittent failure is a bug with an owner;
   tests are never retried to green. `cxk os verify --repeat` measures the
   rate, QEMU record and replay captures one failure for exact replay.
4. **Mutation testing makes sabotage systematic.** Each check's sabotage is
   recorded and re-run by `cxk test sabotage --all` (`devkit/CX_CXK_PLAN.md`
   §3.4). Beyond the recorded ones, a mutation tool (Mull for C, Stryker.NET
   for C#) makes small changes across the code; a change no test notices marks
   code that nothing tests.
5. **Coverage is measured, not assumed.** Line and branch coverage of the
   host tests, and of the boot self-tests through QEMU, reported per area.
6. **Each mitigation tests itself** at boot: a write to read-only kernel
   memory must fault, an execute from a data page must fault, and so on
   (`security/CX_SECURITY.md` §4).
7. **Deterministic simulation for services.** A service with no clock or
   randomness of its own (the network stack first) runs on the host under a
   simulated world, and any failure replays exactly
   (`system/CX_NETWORK.md` §4, item 5).

## 5. The assurance register

Every property that the system's safety rests on, how it is established, and
its state. A new property is added when a design document relies on it.

| Property | Established by | State |
|---|---|---|
| A handle's rights never grow: duplicate, transfer, spawn | CBMC on the rights functions; TLA+ handle model; property tests | Planned (Phase 0.1) |
| A stale handle number never reaches a new object | CBMC on the generation check | Planned (Phase 0.1) |
| A kernel object is freed exactly once, never used after | TLA+ lifetime model; KASAN-style checker in debug | Planned (Phase 0.1) |
| A user pointer check never admits the kernel half, never wraps | CBMC, all 64-bit inputs | Planned (Phase 0.5) |
| No physical page is both free and in use | Property tests against a model; CBMC on the buddy allocator | Planned (Phase 0.5) |
| A program runs only if its signature verifies against a trusted key, checked before any field is trusted | Verified crypto (HACL\*); CBMC on header parsing; fuzzing; existing adversarial tests | Partly built (adversarial tests); rest Phase 1 |
| A reply token is used at most once; a dead peer never strands a caller | TLA+ IPC model | Planned (Phase 1) |
| A waiter is never missed | TLA+ wait-queue and futex models | Planned (Phase 1) |
| Kill always ends its target, in bounded steps, releasing everything | TLA+ model; boot self-test | Planned (Phase 1) |
| Input reaches only the focused window | Self-test; property tests of the router | Planned (Phase 2) |
| CXFS survives a crash at any point | Crash-consistency harness; TLA+ commit model | Planned (Phase 3) |
| A failed update rolls back by itself | TLA+ boot-counter model; QEMU test | Planned (Phase 3) |
| `xc` compiles exactly what the C# compiler does | Differential tests; random programs; fixed point | Built (differential, fixed point); random programs planned |
| Every parser survives hostile input | Fuzzing with sanitizers | Planned, per parser |

## 6. Tools in the container

TLA+ tools (Java), CBMC, libFuzzer and the sanitizers (already in clang),
Mull, Stryker.NET, CsCheck; Frama-C when the first deductive proof is
attempted. Everything runs by hand through `cxk ci` or `cxk test`, as CI does
today; nothing depends on GitHub Actions.

## 7. Order

Re-planned 2026-10-30 after an outside review. The first version of this
table put the host runner, CsCheck, the first libFuzzer targets and
`cxk test sabotage` at "Now", and TLA+, CBMC and the syscall fuzzer in
Phase 0. Only the host runner (§3.7) happened; Phase 0 closed without the
rest. Each item now has a roadmap step (`planning/CX_ROADMAP.md`, Phase 1),
so it cannot slip without the slip being visible there.

| Step | Work |
|---|---|
| 1.5d | The ring-3 X test runner; one structured line per self-test and `cxk verify-boot --json`; `cxk test all` (host tests, container Differential, boot variants) writing one summary; `cxk test sabotage` owning backup, restore, pattern counts, tree checks and "a failed build is not a verdict". Done 2026-10-30 (`devkit/CX_CXK_PLAN.md` §3.4) |
| 1.6 | 1.6a done 2026-10-30 (`diag/testhook.h`, roadmap); 1.6b done 2026-10-30 (`ktest_sweeps.c`: every syscall from ring 3, 13 swept, limits both sides). Test hooks in test builds: race points (a named point where a test can park a thread or deliver a kill or close), fail-the-Nth-allocation in `kmalloc`, `ocache_alloc`, `pmm_alloc`, `kstack_alloc` and `vm_map`, and live counts (each object cache, free pages, handles, mapped bytes, job charge) compared before and after. Sweeps over each syscall's failure paths; resource limits driven to both sides of the bound; the HANDOFF "untested" lists cleared |
| 1.7 | The kernel RNG (ChaCha20, Fortuna-style pools); seeded chaos (random yields at race points), reproducible from a printed seed |
| 1.8 | The hardware report: E820 map, ACPI tables and checksums, MADT and APIC IDs, PCI devices, CPUID, TSC and PM-timer calibration, in a fixed form to serial and the USB crash log; a `cxk` command diffs QEMU, Hyper-V and the rig. Before SMP, which relies on the MADT |
| 1.9 | SMP readiness, SMP, then the 1.6 race-point tests as the SMP stress suite (`-smp 8` under MTTCG, Hyper-V for real parallelism) |
| Ongoing, one item per step where it fits | libFuzzer targets on the host under ASan and UBSan (CXFS superblock and directories first, since attaching a disk is the attack; then CXEX, CXBI, packets), corpora in the tree, each crash a regression test; the ABI layout cross-check (C and X each print `sizeof`/`offsetof` for every ABI struct, compared); random X programs against both compilers and the IR interpreter; CsCheck in the host tests; the generated syscall fuzzer once 1.7 is in |
| Before Phase 2 | TLA+ for IPC, the wait queue, futex, stop and kill; CBMC harnesses for rights, generations and user pointers; KASAN-style checker in debug kernels; mutation testing in the container |
| Phase 3 | The crash-consistency harness and the CXFS v3 commit model |
| Later | HACL\* replaces the RSA verifier; Frama-C proofs where CBMC's bounds are not enough; X contracts proved by SMT |
