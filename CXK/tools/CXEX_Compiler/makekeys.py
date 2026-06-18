#!/usr/bin/env python3
# /CXK/tools/CXEX_Compiler/makekeys.py
# Aurora Tejeda / CATX SYSTEMS LLC
#
# One-time key generation for CXOS code signing (CX_EXTENSION_SYSTEM.md 10).
# Produces:
#   <name>.xksk   - RSA-2048 PRIVATE/signing key (PEM). NEVER ship this. It
#                   stays on the build machine and is used only to sign.
#   <name>.xkpk   - PUBLIC key in CXOS's simple raw format (header + modulus +
#                   exponent), so the kernel can parse it without an ASN.1/DER
#                   parser. This ships in /System.
#
# Requires openssl on PATH.
#
# Usage:  python makekeys.py <name>   (e.g. "kernel" -> kernel.xksk/.xkpk)

import sys, subprocess, struct, re

# .xkpk format (raw, kernel-friendly):
#   magic(4)="CXPK"  version(2)  key_bits(2)  exp(4)  modulus_len(2)  reserved(2)
#   then modulus_len bytes of modulus (big-endian, as RSA n)
XKPK_MAGIC = b"CXPK"
XKPK_VERSION = 1

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)

def main():
    if len(sys.argv) != 2:
        print("usage: python makekeys.py <name>   (e.g. kernel)")
        raise SystemExit(2)
    name = sys.argv[1]
    sk = name + ".xksk"
    pk = name + ".xkpk"

    # 1. generate the RSA-2048 private key (PEM) -> .xksk
    print(f"generating RSA-2048 private key -> {sk}")
    run(["openssl", "genrsa", "-out", sk, "2048"])

    # 2. extract modulus + exponent in text form
    out = run(["openssl", "rsa", "-in", sk, "-noout", "-modulus"]).stdout
    m = re.search(r"Modulus=([0-9A-Fa-f]+)", out)
    if not m:
        raise SystemExit("error: could not read modulus from openssl")
    modulus = bytes.fromhex(m.group(1))

    # exponent: parse the text dump
    txt = run(["openssl", "rsa", "-in", sk, "-noout", "-text"]).stdout
    em = re.search(r"publicExponent:\s*(\d+)", txt)
    exp = int(em.group(1)) if em else 65537

    key_bits = len(modulus) * 8
    print(f"  modulus = {len(modulus)} bytes ({key_bits} bits), exponent = {exp}")

    # 3. write the simple .xkpk the kernel will parse
    header = struct.pack("<4sHHIHH", XKPK_MAGIC, XKPK_VERSION, key_bits,
                         exp, len(modulus), 0)
    with open(pk, "wb") as f:
        f.write(header + modulus)
    print(f"wrote {pk}: {len(header)+len(modulus)} bytes (header + modulus)")

    print()
    print("DONE.")
    print(f"  KEEP SECRET (never ship):  {sk}")
    print(f"  ship in /System:           {pk}")

if __name__ == "__main__":
    main()