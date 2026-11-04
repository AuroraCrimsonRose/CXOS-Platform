# CX Random Numbers
### CX Design Spec: Aurora Tejeda / CATX Systems

The kernel's random number generator: what feeds it, how it turns that into
bytes, when it refuses, and what it can and cannot promise on each machine.
Built in roadmap step 1.7b (ABI 32); the seed file (§5) and seeded chaos at
race points (§8) in 1.7c. `CX_SECURITY.md` §3.2 lists what needs it: the stack
protector, KASLR, network sequence numbers, keys, and the syscall fuzzer.

## 1. Shape

| Part | Where | Host-tested |
|---|---|---|
| ChaCha20 block function (RFC 8439 §2.3) | `kernel/lib/crypto/chacha20.c` | yes, RFC vectors |
| Fortuna pools, seeding, reseeds, output | `kernel/lib/crypto/fortuna.c` | yes |
| The credit estimator, CPU word check | `kernel/lib/crypto/fortuna.c` | yes |
| Sources, the lock, `SYS_RANDOM_READ` | `kernel/drivers/char/random.c` | no: boot self-tests |
| RDSEED / RDRAND | `kernel/arch/x86_64/cpu.c` (`arch_cpu_random`) | no |

`random.c` is the only owner of the generator's state. Interrupts never
touch the generator: `irq_dispatch` (`kernel/arch/x86_64/idt.c`) stores the
low 32 bits of the cycle counter and the vector in a 128-entry ring and
returns. The samples are hashed in when someone asks for bytes, under the
generator's spinlock. This is XNU's split (`osfmk/prng/entropy.c`:
`entropy_collect` stores a sample per interrupt, `entropy_provide` hashes
them for the consumer), and not Serenity's, which takes the generator's lock
and runs a SHA-256 update inside the interrupt (`Kernel/Security/Random.h`,
`EntropySource::add_random_event`). A full ring drops new samples, as XNU's
does. Cost at an interrupt: one `rdtsc` and two stores. The timer runs on
deadlines while the CPU idles, so an idle machine gives few samples (a
20 ms sleep, one: its own wakeup); a busy one gives the tick's thousand a
second and every device's.

## 2. Two phases

**Unseeded.** Everything goes into one SHA-256 context with the bits its
source is credited. At 256 bits the key becomes
SHA-256(key || SHA-256(gathered)) and the generator is seeded. Before that,
nothing is generated: `random_bytes` and `SYS_RANDOM_READ` answer
`E_SHOULD_WAIT` and write nothing. **There is no fallback seed.** Serenity
seeds from the RTC when it has nothing better (`Kernel/Security/Random.cpp`),
which hands out bytes anyone who knows the time can reproduce; Haiku's
`secure_random_value` (`src/system/kernel/util/Random.cpp`) hashes the time,
thread and CPU numbers. A refusal is honest; a guessable seed is not.

**Seeded: Fortuna.** Samples go round 32 SHA-256 pools in turn, uncredited:
the point of Fortuna is not to trust an estimate. A request reseeds first
when pool 0 has taken 64 bytes and 100 ms have passed since the last reseed;
reseed *r*, counting from 1, takes pool *i* when 2^*i* divides *r*, so the
key becomes SHA-256(key || digests). Serenity counts its reseeds from 0, so
its first reseed takes every pool (`FortunaPRNG::reseed`: `m_reseed_number %
(1u << i)` with the number still 0) and pool 31's slow accumulation is spent
at once; here the first reseed takes pool 0 only (`fortuna_reseed_pools`,
host test `fortuna_reseed_r_takes_pool_i_when_2_to_the_i_divides_r`).

**Output.** ChaCha20 keystream with fast key erasure: a request's first
block gives the next key (its first 32 bytes) and the first 32 bytes out;
later blocks follow. The new key replaces the old before the call returns,
so a key read from memory afterwards tells nothing about bytes already
handed out. One key per request lets the nonce stay zero. Serenity takes
the next key from the keystream after the output (`get_random_bytes`), the
same idea in the other order.

## 3. Sources and their credit

| Source | When | Credit |
|---|---|---|
| `cxk_boot_info.rng_seed`, the TSC, the clock | boot | none: nothing vouches for them (the BIOS loader leaves `rng_seed` 0) |
| RDSEED, else RDRAND: 4 words, 10 tries each | boot | 64 bits a plausible word |
| Jitter: the TSC after each of up to 8192 rounds of data-dependent reads and writes over a page | boot, interrupts on | at most 1 bit a round |
| The TSC at every interrupt | always | at most 1 bit a sample, until seeded |
| The seed file the last boot left (§5) | boot, after the disks | 256 bits, once its rewrite reads back |

A CPU word is plausible when it is not 0, not all ones, and not the word
before it: some AMD parts returned all ones from RDRAND after a resume while
reporting success (from memory: the Zen 2 erratum fixed in microcode, 2019;
not in the indexed trees).

A timing sample earns its bit only when its first, second and third
differences from the samples before all reach 16 cycles
(`jitter_credit`). One bit a sample is XNU's assumption
(`entropy_provide`: "Assuming one bit of entropy per sample"); the
three-difference test is Linux's `add_timer_randomness` (from memory; Linux
is not indexed), made stricter: no fractional credit, and nothing for a
small wander. A counter that is stuck, moves in fixed steps, or wanders by a
few cycles earns nothing.

## 4. What each machine gets

- **The rig (FX-8350):** no RDRAND or RDSEED (Piledriver predates both on AMD; roadmap step 7). The
  boot jitter and the interrupt timing carry it; how many bits the jitter
  loop earns there is unmeasured until the hardware report (roadmap step 8).
  The seed file (§5) makes every boot after the first start seeded, once
  the disks are up.
- **QEMU with `-icount`** (every verification boot): the cycle counter is a
  function of the instruction count. The jitter loop still earns its 256
  bits there (measured on the first boot: `jitter bits 256` with the default
  model), from the rounds the timer interrupt lands in, so the boot is
  seeded; but the whole run is replayable, and so is that seed. The
  estimator cannot see this, and no estimator could: see the end of this
  section.
- **QEMU `--cpu max`:** RDRAND and RDSEED come from the host's generator, so
  the boot seeds from them.
- **Hyper-V and real hardware with RDSEED/RDRAND:** seeded at boot.

No estimator can tell a deterministic stream that wanders from a random
one: credit is a lower bound on what the machine is believed to offer, not
a measurement.

## 5. The seed file

The rig has neither RDRAND nor RDSEED, and an idle machine gives few
interrupt samples, so a boot there may wait a long time to be seeded. Each
boot therefore leaves 64 bytes for the next: a record in the crash
partition (`kernel/diag/crashrec.h`: kind `XCRL_KIND_SEED` at sector 48, a
header and one sector, sealed with the crash records' CRC). Raw sectors,
not a file on CXFS: the area is found before the system volume is mounted,
no program can read it (there is no raw-disk call), and the panic and boot
log already write it the same way, text first and header last.

At boot, after `crashlog_boot` and before the self-tests
(`random_seed_file_load`):

1. Read the record. None, or a damaged one: nothing is credited, and the
   generator's bytes are written for the next boot if it is seeded already.
2. Rewrite the record **before** the seed counts: with the generator's next
   bytes if it is seeded, else with SHA-256 of a label, a counter and the
   seed, so the file never holds the same seed twice and the new one does
   not give back the old.
3. Read both sectors back. Only if they are what was written is the seed
   mixed in with 256 bits of credit; otherwise it is mixed in uncredited.
4. If the rewrite came from the seed itself and the generator is now
   seeded, write the generator's bytes over it. A boot that was not seeded
   at all tries again from kmain's idle loop, as often as the boot log.

The read-back is the point of step 3: a USB stick that takes writes and
drops them would otherwise hand every boot the same seed, credited in full
each time. The read sees what the disk says it holds; a drive whose cache
answers for it is not caught. Linux's `systemd-random-seed` makes the same
rule (credit only after the file is rewritten; from memory, not indexed).
XNU takes its seed from the loader (`PE_get_random_seed`,
`pexpert/gen/pe_gen.c`: the `random-seed` property of `/chosen`, zeroed
after the copy, all zeros counting as none); Serenity mixes the TSC,
RDRAND and RDSEED and two multiboot addresses into one word in the
prekernel (`generate_secure_seed`, `Kernel/Prekernel/Random.cpp`) and keeps
nothing between boots; Haiku's random driver takes bytes written to it
(`random_write`, `src/add-ons/kernel/bus_managers/random/driver.cpp`).

What the seed file cannot fix:

- **A copied disk.** Two sticks written from one image after a boot share
  its seed. A fresh image carries none (`cxk os build` leaves the area
  zeroed), so this needs a copy of a used stick.
- **A disk read between boots.** Whoever reads the stick knows the next
  boot's seed; the boot is then only as strong as its other sources. The
  seed costs nothing against that reader and helps against everyone else.
- **VM snapshots** (`-snapshot`, Hyper-V checkpoints) replay the same seed
  into each run. Verification boots start from an image with no seed, so
  they are not credited one; `cxk verify-seed` boots a writable copy
  twice and checks that the second credits the seed the first left, and
  leaves another.

## 6. The interface

- Kernel: `random_bytes(out, len)` (0 or `E_SHOULD_WAIT`), `random_seeded()`,
  `random_stats()` (`kernel/drivers/char/random.h`). Not from an interrupt
  handler.
- Programs: `SYS_RANDOM_READ(buf, len)`, unprivileged, at most
  `RANDOM_READ_MAX` (256) bytes; 0 for 0 bytes; `E_OUT_OF_RANGE` past the
  limit; `E_SHOULD_WAIT` until seeded; `E_BAD_ADDRESS` from the copy (the
  bytes made for an unwritable buffer are discarded, never reused).
  `CX_SYSCALLS_IPC.md` has the call among the others.

## 7. Tests

Host (`kernel/test/host/test_random.c`): RFC 8439 §2.3.2 and A.1 #1; the
reseed schedule; nothing before 256 credited bits, and never from uncredited
input; key erasure pinned to the keystream layout; reseeds on pool 0 and the
interval, and not one byte short; the estimator on fixed, tiny, stuck,
wandering and wrapping counters; the CPU word check.

Boot (`kernel/ktest_random.c`): the RFC block as the kernel was built;
interrupts sampled during a 20 ms sleep and every sample hashed in by a
request; `SYS_RANDOM_READ` from ring 3 against a held generator (the test
hook `random_test_hold`), unseeded (refused, nothing written) and seeded
from a fixed word (exactly the bytes a generator seeded the same way gives,
reads differing, a kernel address refused without a fault), the real
generator back after; the boot seed credited the CPU's words where it has
them and none where it does not (judged under `--cpu max`). X
(`os/apps/xtest.xfxn`, "random: random_read"): the prelude's call, both
outcomes. Sabotages: `tests/sabotage/1.7b.sab`; the host-tested rules were
each broken by hand and seen red (the commit lists them).

The seed file (`ktest_random_seed_file`, against a held generator and the
real crash area): a seed whose rewrite reads back is credited 256 bits, and
the file ends holding exactly the first bytes of a generator seeded the
same way, the generator going on from there; a rewrite the disk drops (the
race point `RP_SEED_WRITTEN` puts the old record back) is not credited and
the generator stays unseeded; a damaged record is neither credited nor
replaced while unseeded; seeded, the file takes the generator's bytes. The
real generator's bytes are left in the file after. Host
(`test_crashrec.c`): a seed record is exactly 64 bytes. Across boots:
`cxk verify-seed`. Sabotages: `tests/sabotage/1.7c.sab`.

## 8. Chaos at race points

The race points of step 6 (`kernel/diag/testhook.h`) take a second use:
while chaos is armed, each arrival at a chosen point draws from a
splitmix64 sequence seeded with a 32-bit number and, so many times in a
thousand, yields one to three times there, so other threads run in the
middle of the operation. The draws depend on the seed alone, and the boot
prints it, so a run that went red replays its decisions. Asked for in
`/System/Config/kernel.xkco` (`chaos_permille`, 0 to 1000, off by
default; `chaos_seed`, 0 for one from this generator), it is armed at every
point before the self-tests and stays for the boot. Not this generator's
bytes: chaos wants speed and replay, and nothing in it is secret.
Needs `CXK_TEST_HOOKS`.

Replay is exact only where the arrivals come in the same order, which
`-icount` boots give and real hardware does not; on the rig the seed
names the decisions, not the interleaving. Test (`ktest_chaos`): the
yields match the documented rule for a fixed seed, the same seed gives the
same ones and another seed others, 1000 yields at every arrival and lets
another thread run each time, a point not chosen never yields, an armed
race point still fires once, bad arguments are refused.

## 9. Not yet

- A `cxk` switch for a chaos boot: today it is an edit of `kernel.xkco` and
  a rebuild.
- Per-CPU sample rings: SMP (step 9).
- A periodic drain: samples reach the pools only when someone asks for
  bytes, so a long quiet spell keeps only the ring's first 128.
- The stack protector and KASLR seeded from it.
