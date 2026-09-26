#!/bin/sh
# /CXK/boot/uefi/secureboot.sh
# Aurora Tejeda / CATX SYSTEMS LLC
#
# Generate a CXK Secure Boot key set, enroll it into an OVMF variable store,
# sign the UEFI stub with it, and boot the result under QEMU.
#
# ---- Why this exists ------------------------------------------------------
#
# cxboot.c reads the firmware's SecureBoot variable and sets
# CXBI_FLAG_SECURE_BOOT. That line cannot be tested on a machine with Secure
# Boot off (it reports off, which proves nothing) and it cannot be tested on
# an OVMF build carrying the Microsoft keys either - firmware refuses to
# launch an unsigned stub at all, so the code never runs. The only way to
# exercise it is to become the platform owner: enroll our own PK/KEK/db,
# sign the stub with our db key, and let firmware verify it.
#
# That is not just a test rig. It is the same procedure a user follows to run
# CXK on their own Secure Boot hardware, and the same key material that would
# eventually let the stub be a real root of trust: firmware verifies the stub,
# the stub verifies kernel.xkex, and CXBI_FLAG_KERNEL_VERIFIED means something.
#
# ---- The key hierarchy, and what each key is for --------------------------
#
#   PK  (Platform Key)      - one per machine. Owns the platform. Holding it
#                             is what lets you replace KEK. Firmware leaves
#                             Setup Mode the moment a PK is enrolled.
#   KEK (Key Exchange Key)  - authorises updates to db and dbx.
#   db  (Signature DB)      - the list firmware checks a binary against.
#                             THIS is the key that signs BOOTX64.EFI.
#
# Three separate keys rather than one reused three times, because that is the
# real shape: db is the one that touches binaries and therefore the one most
# exposed, and it must be replaceable without reflashing the platform.
#
# ---- These keys are throwaway --------------------------------------------
#
# 2048-bit RSA, no passphrase, written to sbkeys/ which .gitignore excludes.
# For real hardware use a 4096-bit key with a passphrase, generated somewhere
# you trust, and keep PK.key offline. A leaked PK is not a leaked test key -
# it lets anyone sign a bootloader that machine will trust forever.
#
# ---- Requirements ---------------------------------------------------------
#
#   openssl                             key generation
#   sbsigntool                          sbsign / sbverify
#   python3-virt-firmware               virt-fw-vars (builds the vars store)
#   qemu-system-x86 + ovmf              to run it
#
# Debian/Ubuntu:
#   apt-get install openssl sbsigntool python3-virt-firmware qemu-system-x86 ovmf
#
# Usage:  ./secureboot.sh [path/to/BOOTX64.EFI]
# Default input is ./BOOTX64.EFI (whatever build.bat or the clang line produced).

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
KEYS="$HERE/sbkeys"
IN=${1:-$HERE/BOOTX64.EFI}
OVMF_DIR=${OVMF_DIR:-/usr/share/OVMF}
CODE="$OVMF_DIR/OVMF_CODE_4M.secboot.fd"
VARS_TEMPLATE="$OVMF_DIR/OVMF_VARS_4M.fd"

# virt-fw-vars is a Python entry point installed against the distro's python3.
# If the system python3 is a different build (a pyenv or a container's own),
# its cffi backend is missing and the tool dies in a Rust panic rather than an
# ImportError. Find an interpreter that can actually import it.
VFV=""
for py in python3.13 python3.12 python3.11 python3; do
    if command -v "$py" >/dev/null 2>&1 &&
       "$py" -c 'import virt.firmware.efi.efivar' 2>/dev/null; then
        VFV="$py /usr/bin/virt-fw-vars"
        break
    fi
done
[ -n "$VFV" ] || { echo "error: virt-fw-vars not importable by any python3 found" >&2; exit 1; }

for t in openssl sbsign sbverify; do
    command -v "$t" >/dev/null 2>&1 || { echo "error: $t not found" >&2; exit 1; }
done
[ -f "$CODE" ] || { echo "error: $CODE not found; set OVMF_DIR" >&2; exit 1; }
[ -f "$IN" ]   || { echo "error: $IN not found; build the stub first" >&2; exit 1; }

# ---- 1. keys -------------------------------------------------------------
# Kept if they already exist: re-generating would invalidate a vars store a
# previous run enrolled, and there is no reason to churn them.
mkdir -p "$KEYS"
chmod 700 "$KEYS"

if [ -f "$KEYS/owner.guid" ]; then
    GUID=$(cat "$KEYS/owner.guid")
else
    # The owner GUID identifies who put a cert in the database. It is not a
    # secret and firmware does not verify it; it exists so a machine with
    # several vendors' keys can tell them apart.
    GUID=$(python3 -c 'import uuid; print(uuid.uuid4())')
    echo "$GUID" > "$KEYS/owner.guid"
fi

for k in PK KEK db; do
    if [ ! -f "$KEYS/$k.key" ]; then
        echo "generating $k"
        openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
            -subj "/CN=CXK Secure Boot $k/O=CATX SYSTEMS LLC/" \
            -keyout "$KEYS/$k.key" -out "$KEYS/$k.crt" 2>/dev/null
        chmod 600 "$KEYS/$k.key"
    fi
done

# ---- 2. variable store ---------------------------------------------------
# --no-microsoft is deliberate: with the Microsoft keys also enrolled a
# signature failure could be masked by shim or by a Microsoft-signed binary
# on the same ESP. Our db alone means a successful boot can only have come
# from our own signature.
#
# --sb sets SecureBootEnable. Enrolling a PK is what takes firmware out of
# Setup Mode, and only then is SecureBoot actually reported on - which is the
# variable cxboot.c reads.
echo "enrolling into $(basename "$VARS_TEMPLATE")"
$VFV -i "$VARS_TEMPLATE" \
    --set-pk  "$GUID" "$KEYS/PK.crt" \
    --add-kek "$GUID" "$KEYS/KEK.crt" \
    --add-db  "$GUID" "$KEYS/db.crt" \
    --no-microsoft --sb \
    -o "$KEYS/CXK_VARS.fd"

# ---- 3. sign -------------------------------------------------------------
# sbsign appends an Authenticode signature to the PE's certificate table and
# points the data directory at it. The stub's own code is untouched, so the
# signed and unsigned binaries behave identically once running - which is
# what makes the negative control below meaningful.
echo "signing $(basename "$IN") with db"
sbsign --key "$KEYS/db.key" --cert "$KEYS/db.crt" \
       --output "$HERE/BOOTX64.signed.efi" "$IN"
sbverify --cert "$KEYS/db.crt" "$HERE/BOOTX64.signed.efi"

# ---- 4. boot -------------------------------------------------------------
# smm=on plus secure=on on the flash device is not optional. Without SMM the
# variable store is writable from outside SMRAM and OVMF will not enforce
# Secure Boot at all - it boots anything and reports whatever the variable
# says, which would make this whole exercise a lie.
run_qemu() {
    esp=$1; vars=$2; log=$3
    cp "$KEYS/CXK_VARS.fd" "$vars"
    timeout 60 qemu-system-x86_64 \
        -machine q35,smm=on -m 512 \
        -global driver=cfi.pflash01,property=secure,value=on \
        -global ICH9-LPC.disable_s3=1 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
        -drive if=pflash,format=raw,unit=1,file="$vars" \
        -drive file=fat:rw:"$esp",format=raw \
        -net none -display none -serial file:"$log" >/dev/null 2>&1 || true
    # Strip the ANSI the UEFI console emits so the log is greppable.
    tr -d '\r' < "$log" | sed 's/\x1b\[[0-9;=]*[A-Za-z]//g'
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/esp_signed/EFI/BOOT" "$WORK/esp_unsigned/EFI/BOOT"
cp "$HERE/BOOTX64.signed.efi" "$WORK/esp_signed/EFI/BOOT/BOOTX64.EFI"
cp "$IN"                      "$WORK/esp_unsigned/EFI/BOOT/BOOTX64.EFI"

echo
echo "=== signed stub (expect: it runs, Secure Boot ON) ==========="
run_qemu "$WORK/esp_signed" "$WORK/vars_s.fd" "$WORK/s.log" | grep -v '^[[:space:]]*$'

# The negative control. "Secure Boot : ON" above proves cxboot.c read the
# variable; it does NOT prove firmware would have stopped an unsigned binary.
# Only this does, and without it a misconfigured run looks like a pass.
echo
echo "=== unsigned stub (expect: Access Denied) ==================="
run_qemu "$WORK/esp_unsigned" "$WORK/vars_u.fd" "$WORK/u.log" | grep -v '^[[:space:]]*$'

echo
echo "keys and vars store: $KEYS"
echo "  CXK_VARS.fd  - pass to QEMU as pflash unit 1 to boot signed builds"
echo "  db.key/.crt  - sign further builds:"
echo "      sbsign --key $KEYS/db.key --cert $KEYS/db.crt --output out.efi in.efi"
echo "  PK/KEK/db.crt - enroll on real hardware from the firmware setup menu"
echo "      (convert first: openssl x509 -in db.crt -outform DER -out db.cer)"
