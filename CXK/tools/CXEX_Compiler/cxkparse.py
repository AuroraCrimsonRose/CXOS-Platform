"""
cxkparse.py
CXK parsing library.

Supports:
- CXEX (XKEX/XCEX/XBEX)
- XKPK public keys
- XKSK private keys (metadata only)
"""

import struct, hashlib
from pathlib import Path
from dataclasses import dataclass, asdict

CXEX_MAGIC=b"CXEX"
XKPK_MAGIC=b"CXPK"
CXSG_MAGIC=b"CXSG"

HEADER_FMT="<4sHHHHIIIIIHHIIII4s"
SECTION_FMT="<8sIIIII"

@dataclass
class CXEXHeader:
    magic: bytes; type_code:int; format_version:int; arch_target:int
    abi_version:int; flags:int; entry_point:int; load_base:int
    image_min:int; image_max:int; section_count:int; section_offset:int
    reloc_offset:int; signature_offset:int; dependency_offset:int
    phys_base:int

@dataclass
class CXEXSection:
    name:str; file_offset:int; virt_addr:int
    file_size:int; mem_size:int; flags:int

class CXEXFile:
    def __init__(self,path):
        self.path=Path(path)
        self.data=self.path.read_bytes()
        r=struct.unpack_from(HEADER_FMT,self.data,0)
        self.header=CXEXHeader(*r[:-1])
        self.sections=[]
        off=self.header.section_offset
        size=struct.calcsize(SECTION_FMT)
        for _ in range(self.header.section_count):
            s=struct.unpack_from(SECTION_FMT,self.data,off)
            self.sections.append(CXEXSection(
                s[0].rstrip(b"\0").decode(errors="ignore"),
                s[1],s[2],s[3],s[4],s[5]
            ))
            off+=size

    def get_section_data(self,name):
        for s in self.sections:
            if s.name==name and s.file_offset:
                return self.data[s.file_offset:s.file_offset+s.file_size]
        return b""

    def get_signature(self):
        if not self.header.signature_offset:
            return None
        off=self.header.signature_offset
        magic,sig_algo,hash_algo=struct.unpack_from("<4sHH",self.data,off)
        if magic!=CXSG_MAGIC:
            return None
        off+=8
        fp=self.data[off:off+32].hex()
        off+=32
        sig_len=struct.unpack_from("<H",self.data,off)[0]
        return {"fingerprint":fp,"sig_algorithm":sig_algo,
                "hash_algorithm":hash_algo,"signature_size":sig_len}

def load_xkpk(path):
    data=Path(path).read_bytes()
    m,v,bits,e,mlen,_=struct.unpack_from("<4sHHIHH",data,0)
    return {
        "magic":m.decode(),
        "version":v,
        "key_bits":bits,
        "exponent":e,
        "modulus_len":mlen,
        "fingerprint":hashlib.sha256(data).hexdigest()
    }

def load_xksk(path):
    data=Path(path).read_bytes()
    txt=data.decode(errors="ignore")
    return {
        "type":"XKSK",
        "pem":"PRIVATE KEY" if "PRIVATE KEY" in txt else "UNKNOWN",
        "sha256":"hidden",
        "size_bytes":len(data)
    }

def open_cxk(path):
    data=Path(path).read_bytes()
    if data[:4]==CXEX_MAGIC: return CXEXFile(path)
    if data[:4]==XKPK_MAGIC: return load_xkpk(path)
    if b"PRIVATE KEY" in data: return load_xksk(path)
    raise ValueError("Unknown format")
