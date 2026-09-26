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
| `build.bat` | MSVC build, matching the rest of the CXK Windows workflow |
| `secureboot.sh` | generate keys, enroll them in an OVMF variable store, sign the stub, boot it |

## Building

**MSVC** (Developer Command Prompt for x64):

    build.bat

**clang / lld** (what CI and the Linux dev box use):

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
sign the stub with our db key:

    apt-get install openssl sbsigntool python3-virt-firmware qemu-system-x86 ovmf
    ./secureboot.sh

The script runs two cases, and both matter:

    === signed stub (expect: it runs, Secure Boot ON) ===========
      Secure Boot : ON
      flags       : 0x0000000000000001

    === unsigned stub (expect: Access Denied) ===================
    BdsDxe: failed to load Boot0001 ...: Access Denied

The first shows the detection works. The second is the one that makes it mean
anything - without it, a run with SMM misconfigured looks identical, because
OVMF without SMM boots unsigned binaries while still reporting whatever the
`SecureBoot` variable happens to say.

`sbkeys/` is gitignored, along with `*.key`, `*.crt` and `*_VARS.fd`. A
committed PK lets anyone sign a bootloader that an enrolled machine trusts
forever, so these never go in the repo - and the ones the script makes are
throwaway 2048-bit keys with no passphrase. For real hardware, generate a
4096-bit key with a passphrase somewhere you trust and keep `PK.key` offline.

## Running CXK on your own Secure Boot machine

Same mechanism, no firmware disabling required:

1. Generate keys as above (or reuse `sbkeys/`).
2. Convert to DER, which is what firmware setup menus accept:
   `openssl x509 -in db.crt -outform DER -out db.cer`
3. Enroll from the firmware's own key-management screen - usually under
   Security, after clearing the factory PK to enter Setup Mode. Keep a record
   of how to restore the factory keys first; some firmware makes this
   awkward and a few make it one-way.
4. Sign every build:
   `sbsign --key db.key --cert db.crt --output BOOTX64.EFI BOOTX64.unsigned.efi`

Clearing the factory PK also drops the Microsoft keys, so anything else on
that machine that relied on them - most other operating systems' bootloaders -
stops booting until you add those keys back alongside ours.

## Why this is worth more than a passing test

It is the only route CXK has to a real root of trust. Today
`CXBI_FLAG_KERNEL_VERIFIED` is a promise nothing keeps: the BIOS chain cannot
verify anything, because whatever checks a signature is itself unverified.
Under Secure Boot the chain closes - firmware verifies the stub against db,
and a stub that firmware has already vouched for can meaningfully verify
`kernel.xkex` before jumping. The existing RSA-2048-over-SHA-256 signing in
the CXEX format becomes load-bearing instead of decorative.

That is part 2's job. This is the key material it will need.
