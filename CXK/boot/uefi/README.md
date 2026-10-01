# CXK UEFI boot stub

A PE32+ EFI application that gathers what a 32-bit kernel cannot get for
itself under UEFI - the memory map, the GOP framebuffer, the ACPI RSDP and
SMBIOS anchors - and fills the CXBI structure defined in `/CXK/abi/cxk_boot.h`.

Status: **part 1**. It gathers and reports. It does not yet load the kernel,
call `ExitBootServices`, or leave long mode.

| file | |
|---|---|
| `efi.h` | the minimal UEFI subset the stub uses; service tables in spec order with `void*` placeholders for the entries we never call |
| `cxboot.c` | the stub |
| `build.bat` | MSVC build. **Being replaced by `cxk uefi build`**; see below |
| `secureboot.bat` | generate keys, enroll them in an OVMF variable store, sign the stub, boot it. **Being replaced by the `cxk secureboot` commands it already calls** |

> **Scripts are being retired.** CXK is moving from `.bat`/`.sh` scripts to
> `cxk` commands, so building and testing work the same way on every host
> (`docs/HARDENING_PLAN.md`, decision D2). `secureboot.bat` goes once this page
> gives its `cxk` sequence, which it does below. `build.bat` goes when
> `cxk uefi build` lands. That command uses clang and lld-link on every host,
> the route given below (decision D5).

## Building

**Planned:** `cxk uefi build`, on every host.

**MSVC** (Developer Command Prompt for x64), until then (being retired):

    build.bat

**clang / lld**, the supported route, and what `cxk uefi build` will run:

    clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
          -mno-red-zone -Wall -Wextra -I../../abi -c cxboot.c
    lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib \
             -out:BOOTX64.EFI cxboot.obj

`-mno-red-zone` is required, not a preference: firmware delivers interrupts on
the stack the stub is running on, and the red zone would be clobbered.
`-fshort-wchar` makes `L"..."` UTF-16, which is what every UEFI string is.

Drop the result at `EFI/BOOT/BOOTX64.EFI` on a FAT ESP. Under QEMU, with no
Secure Boot:

    qemu-system-x86_64 -m 512 -bios /usr/share/OVMF/OVMF.fd \
        -drive file=fat:rw:esp,format=raw -net none

## Testing the Secure Boot path

`cxboot.c` reads the `SecureBoot` variable and sets `CXBI_FLAG_SECURE_BOOT`.
That line is untestable two ways at once: Secure Boot off reports off, which
proves nothing, and an OVMF build carrying the Microsoft keys refuses to
launch the unsigned stub at all, so the code never runs.

The way through is to become the platform owner - enroll our own PK/KEK/db and
sign the stub with our db key. Until it is deleted, `secureboot.bat` runs the
whole sequence below in one go.

Everything it needs is in `tools\cxk.exe`: key generation, the EFI variable
store, and Authenticode signing. **No OpenSSL, no Python, no Windows SDK, no
`signtool`.** The only external requirement is QEMU, whose installer also
supplies the OVMF firmware under its `share\` directory. Pass
`--firmware-dir` if yours lives somewhere unusual.

The same commands work anywhere `cxk` runs, and they are the supported way to
do this:

    cxk secureboot keygen        # PK/KEK/db -> sbkeys\ (.pfx private, .cer/.pem public)
    cxk secureboot varstore      # enroll them into an OVMF vars image, Secure Boot on
    cxk secureboot sign in.efi out.efi
    cxk secureboot verify out.efi
    cxk secureboot test BOOTX64.EFI

`test` runs three boots, and none of them is redundant:

    signed stub - expect it to run and report Secure Boot on
      Secure Boot : ON
      flags       : 0x0000000000000001

    unsigned stub - expect firmware to refuse it
      BdsDxe: failed to load Boot0001 ...: Access Denied

    stub signed by an unenrolled key - expect firmware to refuse it
      BdsDxe: failed to load Boot0001 ...: Access Denied

      PASS the signed stub was loaded
      PASS the unsigned stub was refused, so enforcement is real
      PASS an unenrolled signer was refused, so db is really consulted

There is no Secure Boot "screen" to get past - verification is silent, and the
only visible difference is `starting Boot0001` instead of `failed to load ...
Access Denied`. `starting` means DxeImageVerificationLib hashed the image,
parsed the Authenticode signature, matched the signer against db, and let
`LoadImage` succeed. (A prompt only appears when chaining shim, which CXK does
not - it *is* the bootloader.)

Each run answers a different question, and only the third answers the one that
matters:

- The first shows the detection works.
- The second shows firmware would have stopped an unsigned binary. Without it a
  run with SMM misconfigured looks identical, because OVMF without SMM boots
  unsigned binaries while still reporting whatever the `SecureBoot` variable
  happens to say. That is why `test` passes `smm=on` together with `secure=on`
  on the flash device.
- The third is signed with a key generated on the spot and thrown away, so it
  cannot be in db by accident. It is a perfectly valid signature from an
  authority the platform does not trust. The first two together only show that
  firmware tells signed from unsigned; this one shows it checks *who signed* -
  the difference between a platform that verifies and one that merely notices a
  signature is present.

The run fails unless all three hold.

`sbkeys/` is gitignored, along with `*.pfx`, `*.pem`, `*.cer` and `*_VARS.fd`. A
committed PK lets anyone sign a bootloader that an enrolled machine trusts
forever, so these never go in the repo - and the ones `keygen` makes are
throwaway 2048-bit keys. For real hardware, use `--bits 4096` and generate them
somewhere you trust, keeping `PK.pfx` offline.

## Running CXK on your own Secure Boot machine

Same mechanism, no firmware disabling required:

1. Generate keys as above (or reuse `sbkeys\`).
2. Enroll `PK.cer`, `KEK.cer` and `db.cer` from the firmware's own key
   management screen - usually under Security, after clearing the factory PK to
   enter Setup Mode. These are already DER, which is the format those menus
   accept. Note how to restore the factory keys first; some firmware makes that
   awkward and a few make it one-way.
3. Sign every build: `tools\cxk.exe secureboot sign BOOTX64.EFI`

Clearing the factory PK also drops the Microsoft keys, so anything else on that
machine that relied on them - most other operating systems' bootloaders - stops
booting until you add those keys back alongside ours.

## Why this is worth more than a passing test

It is the only route CXK has to a real root of trust. Today
`CXBI_FLAG_KERNEL_VERIFIED` is a promise nothing keeps: the BIOS chain cannot
verify anything, because whatever checks a signature is itself unverified.
Under Secure Boot the chain closes - firmware verifies the stub against db,
and a stub that firmware has already vouched for can meaningfully verify
`kernel.xkex` before jumping. The existing RSA-2048-over-SHA-256 signing in
the CXEX format becomes load-bearing instead of decorative.

That is part 2's job. This is the key material it will need.
