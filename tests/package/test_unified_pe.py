import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

source = Path(__file__).resolve().parents[2] / "tools/package/verify-unified-pe.py"
spec = importlib.util.spec_from_file_location("unified_pe", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture(function=b"CloseHandle", dependency=b"KERNEL32.dll"):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 1)
    struct.pack_into("<H", data, 0x94, 240)
    struct.pack_into("<H", data, 0x98, 0x20b)
    struct.pack_into("<HH", data, 0x98+48, 6, 1)
    struct.pack_into("<II", data, 0x98+112+8, 0x1000, 40)
    struct.pack_into("<IIII", data, 0x188+8, 512, 0x1000, 512, 512)
    struct.pack_into("<IIIII", data, 512, 0x1040, 0, 0, 0x1100, 0x1040)
    struct.pack_into("<Q", data, 576, 0x1080)
    data[642:642+len(function)+1] = function+b"\0"
    data[768:768+len(dependency)+1] = dependency+b"\0"
    return data


class PEChecks(unittest.TestCase):
    def test_parse_actual_import_layout(self):
        pe = module.PE(fixture())
        self.assertEqual(pe.imports(), {"kernel32.dll": ["CloseHandle"]})
        self.assertEqual(pe.imports(True), {})

    def test_truncated_and_invalid_images_fail(self):
        for length in (0, 1, 2, 63, 127, 159, 400, 513, 578):
            with self.subTest(length=length):
                with self.assertRaises((ValueError, struct.error)):
                    module.PE(fixture()[:length]).imports()
        data = fixture()
        struct.pack_into("<I", data, 576, 0xffff0000)
        with self.assertRaises(ValueError):
            module.PE(data).imports()

    def test_policy_rejects_new_imports_dependencies_and_subsystems(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/"test.dll"
            path.write_bytes(fixture())
            self.assertEqual(module.verify(path, False)["subsystem_version"], (6, 1))
            for data in (fixture(b"WaitOnAddress"), fixture(dependency=b"new.dll"), fixture(b"ExAllocatePool2", b"ntoskrnl.exe")):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    module.verify(path, False)
            data = fixture()
            struct.pack_into("<HH", data, 0x98+48, 10, 0)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                module.verify(path, False)
            data = fixture()
            struct.pack_into("<HH", data, 0x98+40, 10, 0)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                module.verify(path, False)


if __name__ == "__main__":
    unittest.main()
