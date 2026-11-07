"""Independent IPv4 or IPv6 process; never selects both address families."""
import ctypes as C
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_packet_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# The existing packet module consumes only the DLL argument. Imported helpers
# are data/functions; no imported TestCase is exposed to unittest discovery.
from test_packet import DLL, Plan, Output, packet, checksum
from test_coalesce import State


def headers(protocol, timestamp=False, extra=True):
    tcp = (b"\x01\x01\x08\x0a" + struct.pack("!II", 0xfffffff0, 0x80000001)) if timestamp else b""
    options = b"\x01" * 20 if IP == 4 and extra else b""
    extension = bytes([protocol, 0]) + b"\0" * 6 if IP == 6 and extra else b""
    return options, extension, tcp


def source(protocol, payload, *, timestamp=False, extra=True, seq=0xfffffff0, gso=True):
    options, extension, tcp = headers(protocol, timestamp, extra)
    return packet(IP, protocol, payload, flags=0x10, seq=seq,
                  ip_options=options, extension=extension, tcp_options=tcp, gso=gso)


def repair(data, protocol, offset):
    data = bytearray(data)
    if IP == 4:
        data[10:12] = b"\0\0"
        struct.pack_into("!H", data, 10, checksum(bytes(data[:offset])))
        pseudo = data[12:20] + struct.pack("!BBH", 0, protocol, len(data) - offset)
    else:
        pseudo = data[8:40] + struct.pack("!I3xB", len(data) - offset, protocol)
    at = offset + (16 if protocol == 6 else 6)
    data[at:at + 2] = b"\0\0"
    value = checksum(bytes(pseudo + data[offset:]))
    struct.pack_into("!H", data, at, value if value or protocol == 6 else 65535)
    return bytes(data)


class Family32Tests(unittest.TestCase):
    def assert_wire(self, data, protocol, offset):
        self.assertEqual(data[0] >> 4, IP)
        if IP == 4:
            self.assertEqual(struct.unpack_from("!H", data, 2)[0], len(data))
            self.assertEqual(checksum(data[:offset]), 0)
        else:
            self.assertEqual(struct.unpack_from("!H", data, 4)[0], len(data) - 40)
        if protocol == 17:
            self.assertEqual(struct.unpack_from("!H", data, offset + 4)[0], len(data) - offset)
        self.assertEqual(repair(data, protocol, offset), data)

    def test_32_checked_outputs_preserve_family_headers_timestamp_and_payload(self):
        for protocol, kind in ((6, 1), (17, 2)):
            # IPv6 UDP extensions explicitly use the non-GSO path; test that
            # boundary separately below instead of pretending USO supports it.
            extra = IP == 4 or protocol == 6
            parts = [struct.pack("!IQQ", i, 0x0123456789abcdef, 0xfedcba9876543210)
                     + bytes([i]) * 980 for i in range(32)]
            raw = source(protocol, b"".join(parts), timestamp=protocol == 6, extra=extra)
            readable = C.create_string_buffer(raw)
            before = readable.raw
            plan = Plan()
            self.assertEqual(DLL.PbPlanSegments(readable, len(raw), kind, 1000, C.byref(plan)), 0)
            self.assertEqual(plan.count, 32)
            offset = 40 if IP == 4 else (48 if extra else 40)
            self.assertEqual(plan.l4, offset)
            buffers = [C.create_string_buffer(b"\xa5" * 1200) for _ in range(32)]
            outputs = (Output * 32)(*(Output(C.addressof(b), 1200, 0xdeadbeef) for b in buffers))
            written = C.c_uint32(0xdeadbeef)
            self.assertEqual(DLL.PbWriteSegmentBatchChecked(readable, len(raw), C.byref(plan),
                             0, outputs, 32, C.byref(written)), 0)
            self.assertEqual(written.value, 32)
            self.assertEqual(readable.raw, before)
            for i, output in enumerate(outputs):
                data = buffers[i].raw[:output.length]
                self.assert_wire(data, protocol, offset)
                self.assertEqual(data[plan.header:], parts[i])
                self.assertEqual(buffers[i].raw[output.length:1200], b"\xa5" * (1200 - output.length))
                if IP == 4:
                    self.assertEqual(data[20:40], b"\x01" * 20)
                elif extra:
                    self.assertEqual(data[40:48], bytes([protocol, 0]) + b"\0" * 6)
                if protocol == 6:
                    self.assertEqual(data[offset + 20:plan.header], headers(protocol, True, extra)[2])
                    self.assertEqual(struct.unpack_from("!I", data, offset + 4)[0],
                                     (0xfffffff0 + i * 1000) & 0xffffffff)
            print(f"FAMILY32 ip={IP} protocol={protocol} checked_calls=1 outputs=32 source_and_time_fields=preserved")

    def test_32_timestamp_and_outer_options_are_coalesce_boundaries(self):
        offset = 40 if IP == 4 else 48
        first = repair(source(6, b"a" * 1000, extra=False, gso=False), 6, 20 if IP == 4 else 40)
        pending = State()
        output = C.create_string_buffer(65536)
        output.raw = b"\xa5" * len(output)
        self.assertEqual(DLL.PbCoalesceBegin(C.byref(pending), output, 65535, first, len(first)), 0)
        saved_state, saved_output = bytes(pending), output.raw
        for i in range(32):
            payload = struct.pack("!IQQ", i, 0x0123456789abcdef, 0xfedcba9876543210)
            payload += bytes([i]) * (1500 - offset - 32 - len(payload))
            raw = bytearray(source(6, payload, timestamp=True, seq=0x3d8, gso=False))
            struct.pack_into("!II", raw, offset + 24, (0xfffffff0 + i) & 0xffffffff, 0x80000000 + i)
            raw = repair(raw, 6, offset)
            self.assertEqual(len(raw), 1500)
            self.assert_wire(raw, 6, offset)
            state = State()
            untouched = C.create_string_buffer(65536)
            untouched.raw = b"\xa5" * len(untouched)
            self.assertEqual(DLL.PbCoalesceBegin(C.byref(state), untouched, 65535, raw, len(raw)), 2)
            self.assertEqual((bytes(state), untouched.raw), (bytes(State()), b"\xa5" * len(untouched)))
            self.assertEqual(DLL.PbCoalesceAppend(C.byref(pending), raw, len(raw)), 2)
            self.assertEqual((bytes(pending), output.raw), (saved_state, saved_output))
        self.assertEqual(DLL.PbCoalesceFinish(C.byref(pending)), 0)
        self.assertEqual((pending.count, output.raw[:pending.length]), (1, first))
        print(f"FAMILY32 ip={IP} tcp_timestamp_candidates=32 coalesce=unsupported state_and_output=unchanged")

    def test_32_fragment_and_family_offload_rejections_do_not_write(self):
        for protocol, kind in ((6, 1), (17, 2)):
            for i in range(32):
                original = repair(source(protocol, bytes([i]) * 2000, extra=False, gso=False),
                                  protocol, 20 if IP == 4 else 40)
                plan = Plan()
                self.assertEqual(DLL.PbPlanSegments(original, len(original), kind, 500, C.byref(plan)), 0)
                if IP == 4:
                    # First fragment of a larger checksum-correct datagram;
                    # its1024-byte fragment data is an8-byte multiple.
                    raw = bytearray(original[:20 + 1024])
                    struct.pack_into("!HH", raw, 2, len(raw), i)
                    raw[6:8] = b"\x20\x00"
                    raw[10:12] = b"\0\0"
                    struct.pack_into("!H", raw, 10, checksum(bytes(raw[:20])))
                    expected = 1
                else:
                    fragment = bytes([protocol, 0, 0, 1]) + struct.pack("!I", i)
                    raw = bytearray(original[:40] + fragment + original[40:40 + 1024])
                    raw[6] = 44
                    struct.pack_into("!H", raw, 4, len(raw) - 40)
                    expected = 2
                readable = C.create_string_buffer(bytes(raw))
                before = readable.raw
                self.assertEqual(DLL.PbPlanSegments(readable, len(raw), kind, 500, C.byref(Plan())), expected)
                plan.source = len(raw)  # Untrusted length agrees; parser must still reject the fragment.
                buffers = [C.create_string_buffer(b"\xa5" * 1200) for _ in range(32)]
                outputs = (Output * 32)(*(Output(C.addressof(b), 1200, 0xdeadbeef) for b in buffers))
                written = C.c_uint32(0xdeadbeef)
                self.assertEqual(DLL.PbWriteSegmentBatchChecked(readable, len(raw), C.byref(plan),
                                 0, outputs, 32, C.byref(written)), 1)
                self.assertEqual(readable.raw, before)
                self.assertEqual(written.value, 0)
                self.assertTrue(all(b.raw[:1200] == b"\xa5" * 1200 for b in buffers))
                self.assertTrue(all(o.length == 0xdeadbeef for o in outputs))
                if IP == 6 and protocol == 17:
                    udp_extension = source(17, bytes([i]) * 1000, extra=True)
                    self.assertEqual(DLL.PbPlanSegments(udp_extension, len(udp_extension), 2, 500, C.byref(Plan())), 2)
            print(f"FAMILY32 ip={IP} protocol={protocol} fragment_packets=32 checked_written=0 outputs=unchanged")


if __name__ == "__main__":
    print(f"FAMILY: IPv{IP}", flush=True)
    unittest.main(verbosity=2)
