"""Check loading prerequisites without loading/installing a kernel driver."""
import argparse
import json
import struct
from pathlib import Path


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("missing DOS signature")
        nt = self.u32(0x3c)
        if data[nt:nt + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        machine, sections = struct.unpack_from("<HH", data, nt + 4)
        self.machine = machine
        opt_size = self.u16(nt + 20)
        opt = nt + 24
        magic = self.u16(opt)
        if magic not in (0x10b, 0x20b):
            raise ValueError("unsupported optional header")
        self.width = 8 if magic == 0x20b else 4
        self.subsystem_version = (self.u16(opt + 48), self.u16(opt + 50))
        self.os_version = (self.u16(opt + 40), self.u16(opt + 42))
        self.directories = opt + (112 if self.width == 8 else 96)
        self.sections = []
        for i in range(sections):
            pos = opt + opt_size + i * 40
            size, va, raw_size, raw = struct.unpack_from("<IIII", data, pos + 8)
            self.sections.append((va, max(size, raw_size), raw, raw_size))

    def u16(self, pos):
        return struct.unpack_from("<H", self.data, pos)[0]

    def u32(self, pos):
        return struct.unpack_from("<I", self.data, pos)[0]

    def offset(self, rva):
        for va, size, raw, raw_size in self.sections:
            if va <= rva < va + size:
                if rva - va >= raw_size:
                    raise ValueError("RVA has no file-backed data")
                return raw + rva - va
        raise ValueError(f"RVA outside sections: {rva:#x}")

    def string(self, rva):
        pos = self.offset(rva)
        return self.data[pos:self.data.index(b"\0", pos)].decode("ascii")

    def imports(self, delay=False):
        rva = self.u32(self.directories + (13 if delay else 1) * 8)
        result = {}
        if not rva:
            return result
        pos = self.offset(rva)
        step = 32 if delay else 20
        while any(self.data[pos:pos + step]):
            if delay:
                attrs, name, _, _, thunk = struct.unpack_from("<IIIII", self.data, pos)
                if attrs != 1:
                    raise ValueError("delay descriptors must use RVAs")
            else:
                thunk, _, _, name, iat = struct.unpack_from("<IIIII", self.data, pos)
                thunk = thunk or iat
            module = self.string(name).lower()
            functions = []
            t = self.offset(thunk)
            while True:
                value = struct.unpack_from("<Q" if self.width == 8 else "<I", self.data, t)[0]
                if not value:
                    break
                if value & (1 << (self.width * 8 - 1)):
                    raise ValueError("ordinal imports require explicit compatibility review")
                functions.append(self.string(value + 2))
                t += self.width
            result[module] = functions
            pos += step
        return result


def verify(path, kernel):
    pe = PE(Path(path).read_bytes())
    if pe.machine != 0x8664 or pe.subsystem_version > (6, 1) or pe.os_version > (6, 1):
        raise ValueError("expected x64 image with a Windows 7 subsystem baseline")
    imports = pe.imports()
    allowed = {"ntoskrnl.exe", "ndis.sys", "hal.dll", "wmilib.sys"} if kernel else {"kernel32.dll", "ntdll.dll", "bcrypt.dll"}
    if set(imports) - allowed:
        raise ValueError(f"unexpected eager dependencies: {set(imports) - allowed}")
    forbidden = {"ExAllocatePool2", "ExAllocatePool3", "KeInitializeSpinLock", "GetSystemTimePreciseAsFileTime",
                 "WaitOnAddress", "WakeByAddressSingle", "WakeByAddressAll", "SwDeviceCreate", "SwDeviceClose"}
    bad = forbidden.intersection(f for values in imports.values() for f in values)
    if bad:
        raise ValueError(f"downlevel-incompatible eager imports: {bad}")
    return {"path": str(Path(path).resolve()), "subsystem_version": pe.subsystem_version, "os_version": pe.os_version,
            "imports": imports, "delay_imports": pe.imports(True), "kernel_runtime_tested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", required=True)
    parser.add_argument("--sys", required=True)
    args = parser.parse_args()
    print(json.dumps({"dll": verify(args.dll, False), "sys": verify(args.sys, True)}, indent=2))
