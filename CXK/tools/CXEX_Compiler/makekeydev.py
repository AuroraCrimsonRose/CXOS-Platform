#!/usr/bin/env python3
# CXEX Key Generator / Key Loader
#
# This tool manages RSA-2048 signing keys for CXOS code signing.
#
# It can operate in two modes:
#
#   1. NEW KEY MODE
#      python makekeydev.py new <name>
#      - Generates a fresh RSA-2048 private key (<name>.xksk)
#      - Extracts modulus + exponent
#      - Builds a kernel-friendly public key file (<name>.xkpk)
#
#   2. EXISTING KEY MODE
#      python makekeydev.py use <existing.xksk>
#      - Loads an existing RSA private key
#      - Re-extracts modulus + exponent
#      - Rebuilds the matching .xkpk public key file
#
# OUTPUT FORMAT (.xkpk):
#   Binary kernel-readable public key:
#     magic        = "CXPK"
#     version      = 1
#     key_bits     = RSA key size in bits
#     exponent     = public exponent (usually 65537)
#     modulus_len  = length of modulus in bytes
#     reserved     = 0
#     modulus      = raw big-endian RSA modulus
#
# SECURITY NOTES:
# - .xksk is PRIVATE and must NEVER be shipped or exposed
# - .xkpk is safe to ship and is used by the kernel for signature verification
# - Trust is anchored to whichever private key generated the shipped .xkpk
#
# Requires:
#   - openssl available in PATH
#
# Usage:
#   python makekeys.py new kernel
#   python makekeys.py use kernel.xksk
import sys, subprocess, struct, re, os

XKPK_MAGIC = b"CXPK"
XKPK_VERSION = 1

def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True)

def extract_modulus_and_exp_from_key(sk_path):
    # extract modulus
    out = run(["openssl", "rsa", "-in", sk_path, "-noout", "-modulus"]).stdout
    m = re.search(r"Modulus=([0-9A-Fa-f]+)", out)
    if not m:
        raise SystemExit("error: could not read modulus")
    modulus = bytes.fromhex(m.group(1))

    # extract exponent
    txt = run(["openssl", "rsa", "-in", sk_path, "-noout", "-text"]).stdout
    em = re.search(r"publicExponent:\s*(\d+)", txt)
    exp = int(em.group(1)) if em else 65537

    return modulus, exp


def generate_new(name):
    sk = name + ".xksk"
    pk = name + ".xkpk"

    print(f"generating new RSA-2048 key -> {sk}")
    run(["openssl", "genrsa", "-out", sk, "2048"])

    modulus, exp = extract_modulus_and_exp_from_key(sk)
    return sk, pk, modulus, exp


def use_existing(sk):
    if not os.path.exists(sk):
        raise SystemExit(f"error: key not found: {sk}")

    print(f"using existing key -> {sk}")
    modulus, exp = extract_modulus_and_exp_from_key(sk)
    return sk, sk.replace(".xksk", ".xkpk"), modulus, exp


def write_xkpk(pk_path, modulus, exp):
    key_bits = len(modulus) * 8

    header = struct.pack(
        "<4sHHIHH",
        XKPK_MAGIC,
        XKPK_VERSION,
        key_bits,
        exp,
        len(modulus),
        0
    )

    with open(pk_path, "wb") as f:
        f.write(header + modulus)

    print(f"wrote {pk_path}: {len(header)+len(modulus)} bytes")


def main():
    if len(sys.argv) < 3:
        print("usage:")
        print("  python makekeys.py new <name>")
        print("  python makekeys.py use <existing.xksk>")
        raise SystemExit(2)

    mode = sys.argv[1]

    if mode == "new":
        name = sys.argv[2]
        sk, pk, modulus, exp = generate_new(name)

    elif mode == "use":
        sk_path = sys.argv[2]
        sk, pk, modulus, exp = use_existing(sk_path)

    else:
        raise SystemExit("error: mode must be 'new' or 'use'")

    key_bits = len(modulus) * 8
    print(f"  modulus = {len(modulus)} bytes ({key_bits} bits), exponent = {exp}")

    write_xkpk(pk, modulus, exp)

    print("\nDONE.")
    print(f"  private key: {sk}")
    print(f"  public key : {pk}")


if __name__ == "__main__":
    main()