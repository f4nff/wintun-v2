"""Last-output aliases must reject the whole checked 32-output request.

IPv4 and IPv6 run separately. Each source/plan/metadata/count alias is one
owned, accessible byte at the end of its control span. A wrong implementation
can fail assertions without writing a full packet into a control object.
This is caller validation, not a claim that such aliases are legal requests.
Packet.h distinguishes geometric INVALID (no writes, even to written) from
packet/plan validation failure (written zero) and NO_SPACE (accepted prefix).
Recovery restores only the last entry and generates all 32 wire packets once.
Payload markers are opaque data, not timestamps, epochs or scheduling clocks.
Direct checked-call counts are not memcpy, Submit/Commit, CPU or leak metrics.
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_request_alias_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# Import only data/functions, never the historical mixed-family TestCase.
from test_packet import DLL, Plan, Output, checksum

COUNT, MSS, CAPACITY, GUARD = 32, 64, 128, 64
UNWRITTEN, SOURCE_SENTINEL, OUTPUT_SENTINEL, CONTROL_SENTINEL = 0xdeadbeef, 0x6d, 0xa5, 0xb7
INITIAL_SEQUENCE, ACK, WINDOW = 0xfffffff0, 0x80000001, 65535
IP_HEADER = 20 if IP == 4 else 40
ADDRESSES = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)
ALIASES = ("source", "plan", "metadata", "written")
MARKERS = (0, 0xffffffffffffffff, 0x0123456789abcdef, 0xfedcba9876543210)


def pseudo(protocol, length):
    suffix = struct.pack("!BBH", 0, protocol, length) if IP == 4 else struct.pack("!I3xB", length, protocol)
    return SOURCE + DESTINATION + suffix


def wire(protocol, payload, index=None):
    """Complete wire bytes from fixed geometry/endpoints, never native Plan."""
    if protocol == 6:
        flags = 0x99 if index is None else 0x10 | (0x80 if index == 0 else 0) | (0x09 if index == COUNT - 1 else 0)
        sequence = (INITIAL_SEQUENCE + (0 if index is None else index * MSS)) & 0xffffffff
        transport = bytearray(struct.pack("!HHIIHHHH", 10001, 10002, sequence,
                                         ACK, 0x5000 | flags, WINDOW, 0, 0) + payload)
        at = 16
    else:
        transport = bytearray(struct.pack("!HHHH", 10001, 10002, 8 + len(payload), 0) + payload)
        at = 6
    value = checksum(pseudo(protocol, len(transport)) + transport)
    struct.pack_into("!H", transport, at, value or (65535 if protocol == 17 else 0))
    if IP == 4:
        initial, mask = (0x7ff0, 0x7fff) if protocol == 6 else (0xfff0, 0xffff)
        ident = (initial + (0 if index is None else index)) & mask
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0x2e,
            IP_HEADER + len(transport), ident, 0x4000, 64, protocol, 0, SOURCE, DESTINATION))
        struct.pack_into("!H", header, 10, checksum(header))
    else:
        header = struct.pack("!IHBB16s16s", 0x62e34567, len(transport), protocol, 64, SOURCE, DESTINATION)
    return bytes(header + transport)


class RequestAliasFamily32Tests(unittest.TestCase):
    def scenario(self, protocol, alias):
        parts = [struct.pack("!IQQB", index, MARKERS[index % 4], MARKERS[(index + 1) % 4],
                             0xa1) + bytes([index + 0x30]) * 43 for index in range(COUNT)]
        self.assertTrue(all(len(part) == MSS for part in parts))
        packet = wire(protocol, b"".join(parts))
        source_bytes = packet + bytes([SOURCE_SENTINEL]) * GUARD
        source = C.create_string_buffer(source_bytes, len(source_bytes))
        plan_storage = C.create_string_buffer(bytes([CONTROL_SENTINEL]) * (C.sizeof(Plan) + GUARD), C.sizeof(Plan) + GUARD)
        plan = Plan.from_buffer(plan_storage)
        self.assertEqual(DLL.PbPlanSegments(source, len(packet), 1 if protocol == 6 else 2, MSS, C.byref(plan)), 0)
        header = IP_HEADER + (20 if protocol == 6 else 8)
        self.assertEqual((plan.source, plan.payload, plan.count, plan.mss, plan.header,
            plan.l4, plan.l4_header, plan.pseudo_src, plan.pseudo_dst, plan.ip,
            plan.protocol, plan.kind, plan.udp_zero),
            (len(packet), COUNT * MSS, COUNT, MSS, header, IP_HEADER, 20 if protocol == 6 else 8,
             12 if IP == 4 else 8, 16 if IP == 4 else 24, IP, protocol, 1 if protocol == 6 else 2, 0))
        self.assertEqual(plan_storage.raw[C.sizeof(Plan):], bytes([CONTROL_SENTINEL]) * GUARD)
        plan_before = plan_storage.raw
        buffers = [C.create_string_buffer(bytes([OUTPUT_SENTINEL]) * (CAPACITY + GUARD), CAPACITY + GUARD) for _ in range(COUNT)]
        metadata_size = C.sizeof(Output) * COUNT
        metadata = C.create_string_buffer(bytes([CONTROL_SENTINEL]) * (metadata_size + GUARD), metadata_size + GUARD)
        outputs = (Output * COUNT).from_buffer(metadata)
        for index, buffer in enumerate(buffers):
            outputs[index] = Output(C.addressof(buffer), CAPACITY, UNWRITTEN)
        metadata_original = metadata.raw
        count_storage = C.create_string_buffer(bytes([CONTROL_SENTINEL]) * (C.sizeof(C.c_uint32) + GUARD), C.sizeof(C.c_uint32) + GUARD)
        written = C.c_uint32.from_buffer(count_storage)
        written.value = UNWRITTEN
        count_before = count_storage.raw
        ends = {"source": C.addressof(source) + len(packet),
                "plan": C.addressof(plan),
                "metadata": C.addressof(outputs) + metadata_size,
                "written": C.addressof(written) + C.sizeof(written)}
        ends["plan"] += C.sizeof(Plan)
        outputs[-1].data, outputs[-1].capacity = ends[alias] - 1, 1
        metadata_before = metadata.raw
        calls = []

        def checked():
            calls.append((0, COUNT))
            return DLL.PbWriteSegmentBatchChecked(source, len(packet), C.byref(plan),
                                                  0, outputs, COUNT, C.byref(written))

        self.assertEqual(checked(), 1)
        self.assertEqual(written.value, UNWRITTEN)
        self.assertEqual(count_storage.raw, count_before)
        self.assertEqual(source.raw, source_bytes)
        self.assertEqual(plan_storage.raw, plan_before)
        self.assertEqual(metadata.raw, metadata_before)
        self.assertTrue(all(buffer.raw == bytes([OUTPUT_SENTINEL]) * (CAPACITY + GUARD) for buffer in buffers))
        # Restore only the invalid data pointer/capacity. The rejected request
        # must not require reinitializing any source, length or control output.
        outputs[-1].data, outputs[-1].capacity = C.addressof(buffers[-1]), CAPACITY
        self.assertEqual(metadata.raw, metadata_original)
        self.assertEqual(checked(), 0)
        self.assertEqual(written.value, COUNT)
        self.assertEqual(count_storage.raw[C.sizeof(written):], count_before[C.sizeof(written):])
        for index, buffer in enumerate(buffers):
            with self.subTest(segment=index):
                expected = wire(protocol, parts[index], index)
                self.assertEqual((outputs[index].data, outputs[index].capacity, outputs[index].length),
                                 (C.addressof(buffer), CAPACITY, header + MSS))
                self.assertEqual(buffer.raw[:header + MSS], expected)
                self.assertEqual(buffer.raw[header + MSS:], bytes([OUTPUT_SENTINEL]) * (CAPACITY + GUARD - header - MSS))
                self.assertEqual(checksum(pseudo(protocol, len(expected) - IP_HEADER) + expected[IP_HEADER:]), 0)
                if IP == 4:
                    self.assertEqual(checksum(buffer.raw[:IP_HEADER]), 0)
        self.assertEqual(source.raw, source_bytes)
        self.assertEqual(plan_storage.raw, plan_before)
        self.assertEqual(metadata.raw[metadata_size:], bytes([CONTROL_SENTINEL]) * GUARD)
        self.assertEqual(calls, [(0, COUNT), (0, COUNT)])
        print(f"REQUEST_ALIAS32 ip={IP} protocol={protocol} alias={alias} rejected=invalid "
              "written=preserved outputs=32 recovery_checked=1 direct_calls=2 source_plan_meta_sentinels=preserved")

    def test_tcp_32_outputs_reject_last_control_alias_then_recover_atomically(self):
        for alias in ALIASES:
            with self.subTest(alias=alias):
                self.scenario(6, alias)

    def test_udp_32_outputs_reject_last_control_alias_then_recover_atomically(self):
        for alias in ALIASES:
            with self.subTest(alias=alias):
                self.scenario(17, alias)


if __name__ == "__main__":
    unittest.main(verbosity=2)
