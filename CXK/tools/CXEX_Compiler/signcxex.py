#!/usr/bin/env python3
# /CXK/tools/CXEX_Compiler/signcxex.py
# Aurora Tejeda / CATX SYSTEMS LLC
#
# Sign a CXEX artifact (CX_EXTENSION_SYSTEM.md 10). Computes SHA-256 over the
# image, RSA-signs the digest with the private .xksk, and appends a CXSG
# signature block; sets the SIGNED flag and signature_offset in the header.
#
# The hash covers the whole file EXCEPT the appended signature block. We patch
# the header (SIGNED flag + signature_offset) BEFORE hashing, so the digest is
# over the file exactly as it will finally exist minus the block itself.
#
# Requires openssl on PATH.
#
# Usage:  python signcxex.py <file.xkex> <key.xksk> <key.xkpk>
#   (.xkpk is read only to compute the key fingerprint stored in the block)

import sys, struct, subprocess, hashlib, os, tempfile

HEADER_FMT = "<4sHHHHIIIIIHHIII8s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
FLAG_SIGNED = 1 << 2

# signature block (10.3): magic(4)="CXSG" sig_algo(2) hash_algo(2)
#   key_fingerprint(32) sig_len(2) signature(sig_len)
CXSG_MAGIC = b"CXSG"
SIG_ALGO_RSA2048_SHA256 = 1
HASH_ALGO_SHA256 = 1
BLOCK_FIXED = struct.calcsize("<4sHH32sH")   # before the variable signature

def rsa_sign(digest, sk_path):
    # sign the raw 32-byte SHA-256 digest with PKCS#1 v1.5 using openssl.
    # 'openssl pkeyutl -sign' over a precomputed digest needs the digest in a
    # file and -pkeyopt digest:sha256.
    with tempfile.NamedTemporaryFile(delete=False) as df:
        df.write(digest); dpath = df.name
    try:
        out = subprocess.run(
            ["openssl", "pkeyutl", "-sign", "-inkey", sk_path,
             "-in", dpath, "-pkeyopt", "digest:sha256",
             "-pkeyopt", "rsa_padding_mode:pkcs1"],
            check=True, capture_output=True).stdout
    finally:
        os.unlink(dpath)
    return out

def main():
    if len(sys.argv) != 4:
        print("usage: python signcxex.py <file.xkex> <key.xksk> <key.xkpk>")
        raise SystemExit(2)
    target, sk, pk = sys.argv[1], sys.argv[2], sys.argv[3]

    data = bytearray(open(target, "rb").read())
    hdr = list(struct.unpack_from(HEADER_FMT, data, 0))
    if hdr[0] != b"CXEX":
        raise SystemExit("error: not a CXEX file")

    # key fingerprint = SHA-256 of the .xkpk file
    pk_bytes = open(pk, "rb").read()
    fingerprint = hashlib.sha256(pk_bytes).digest()

    # the signature block will be appended at the current end of file
    sig_offset = len(data)

    # patch header: set SIGNED flag + signature_offset, BEFORE hashing
    hdr[5] = hdr[5] | FLAG_SIGNED       # flags
    hdr[12] = sig_offset                # signature_offset field
    struct.pack_into(HEADER_FMT, data, 0, *hdr)

    # hash the image as it now stands (header patched, no block yet)
    digest = hashlib.sha256(bytes(data)).digest()

    # RSA-sign the digest
    signature = rsa_sign(digest, sk)
    sig_len = len(signature)

    block = struct.pack("<4sHH32sH", CXSG_MAGIC, SIG_ALGO_RSA2048_SHA256,
                        HASH_ALGO_SHA256, fingerprint, sig_len) + signature

    with open(target, "wb") as f:
        f.write(bytes(data) + block)

    print(f"signed {target}")
    print(f"  digest (sha256)   = {digest.hex()}")
    print(f"  signature_offset  = {sig_offset}")
    print(f"  signature length  = {sig_len} bytes")
    print(f"  key fingerprint   = {fingerprint.hex()[:32]}...")

if __name__ == "__main__":
    main()