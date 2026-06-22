"""
CXKDUMP

Usage:
  python cxkdump.py file.xkex
  python cxkdump.py file.xkpk
  python cxkdump.py file.xksk

Options:
  --hex
      Hex dump entire file

  --json
      JSON metadata output

  --extract
      Extract sections plus metadata.json and signature.json

  --verify key.xkpk
      Verify that CXSG fingerprint matches the supplied XKPK.
      (Full RSA verification reserved for future implementation.)

Examples:
  python cxkdump.py kernel.xkex
  python cxkdump.py kernel.xkex --extract
  python cxkdump.py kernel.xkex --verify kernel.xkpk
"""

import sys,json,hashlib
from pathlib import Path
from dataclasses import asdict
from cxkparse import *

def banner(t):
    print("="*60)
    print(t)
    print("="*60)

def hexdump(data):
    for i in range(0,len(data),16):
        c=data[i:i+16]
        print(f"{i:08X}  {' '.join(f'{b:02X}' for b in c)}")

def main():
    if len(sys.argv)<2:
        print(__doc__)
        return

    path=Path(sys.argv[1])
    args=sys.argv[2:]
    obj=open_cxk(path)

    if isinstance(obj,CXEXFile):
        h=obj.header
        banner("CXEX EXECUTABLE")
        print(f"Entry Point : 0x{h.entry_point:08X}")
        print(f"Phys Base   : 0x{h.phys_base:08X}")
        print(f"Image Range : 0x{h.image_min:08X} -> 0x{h.image_max:08X}")
        print(f"Sections    : {h.section_count}\n")

        for s in obj.sections:
            print(f"{s.name:<8} VA=0x{s.virt_addr:08X} FILE={s.file_size} MEM={s.mem_size}")

        sig=obj.get_signature()
        if sig:
            print("\nSignature Present")
            print("Fingerprint:",sig["fingerprint"])

        if "--verify" in args:
            key=args[args.index("--verify")+1]
            pk=load_xkpk(key)
            banner("CXEX SIGNATURE VERIFICATION")
            if sig and sig["fingerprint"]==pk["fingerprint"]:
                print("RESULT: VALID KEY FINGERPRINT MATCH")
            else:
                print("RESULT: FAILED")

        if "--extract" in args:
            out=Path(path.stem+"_dump")
            out.mkdir(exist_ok=True)
            for s in obj.sections:
                if s.file_offset:
                    (out/f"{s.name}.bin").write_bytes(obj.get_section_data(s.name))
            (out/"metadata.json").write_text(json.dumps({
                "header":asdict(h),
                "sections":[asdict(x) for x in obj.sections]
            },indent=2,default=str))
            (out/"signature.json").write_text(json.dumps(sig or {},indent=2))
            print("\nExtracted to",out)

        if "--json" in args:
            print(json.dumps({
                "header":asdict(h),
                "sections":[asdict(x) for x in obj.sections],
                "signature":sig
            },indent=2,default=str))

        if "--hex" in args:
            hexdump(obj.data)

    elif isinstance(obj,dict) and obj.get("magic")=="CXPK":
        banner("XKPK PUBLIC KEY")
        print(f"Version        : {obj['version']}")
        print(f"Key Bits       : {obj['key_bits']}")
        print(f"Exponent       : {obj['exponent']}")
        print(f"Modulus Length : {obj['modulus_len']} bytes")
        print("\nFingerprint:")
        print(obj["fingerprint"])

    else:
        banner("XKSK PRIVATE KEY")
        print(f"Format    : {obj['pem']}")
        print(f"File Size : {obj['size_bytes']} bytes")
        print("\nWARNING: Contains private key material. Never distribute this file.")

if __name__=="__main__":
    main()
