# CX Random Numbers
### CX Design Spec: Aurora Tejeda / CATX Systems

The kernel's random number generator: what feeds it, how it turns that into
bytes, when it refuses, and what it can and cannot promise on each machine.
Built in roadmap step 1.7b (ABI 32); the seed file and seeded chaos at race
points follow in 1.7c. `CX_SECURITY.md` §3.2 lists what needs it: the stack
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
  The seed file (1.7c) makes the next boot start seeded.
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

## 5. The interface

- Kernel: `random_bytes(out, len)` (0 or `E_SHOULD_WAIT`), `random_seeded()`,
  `random_stats()` (`kernel/drivers/char/random.h`). Not from an interrupt
  handler.
- Programs: `SYS_RANDOM_READ(buf, len)`, unprivileged, at most
  `RANDOM_READ_MAX` (256) bytes; 0 for 0 bytes; `E_OUT_OF_RANGE` past the
  limit; `E_SHOULD_WAIT` until seeded; `E_BAD_ADDRESS` from the copy (the
  bytes made for an unwritable buffer are discarded, never reused).
  `CX_SYSCALLS_IPC.md` has the call among the others.

## 6. Tests

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

## 7. Not yet

- The seed file on the boot disk, rewritten at each boot, and seeded chaos
  at race points: 1.7c.
- Per-CPU sample rings: SMP (step 9).
- A periodic drain: samples reach the pools only when someone asks for
  bytes, so a long quiet spell keeps only the ring's first 128.
- The stack protector and KASLR seeded from it.
