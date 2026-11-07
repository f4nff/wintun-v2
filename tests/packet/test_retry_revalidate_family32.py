"""Partial31, stale structural source, rejected tail, then tail-only recovery.

Each address family runs in its own process. All source bytes are caller-owned
and change only between completed API calls; no native/Rust borrowed loan is
modified, and no concurrent mutation is modeled. Plan records derived geometry,
not a content hash. Changes here intentionally invalidate that geometry/type.
This is a generic checked-helper contract, not natural NDIS input or a change
to the SYS plain path. Output-prefix snapshots are test oracles only; direct
calls do not measure payload copying, Submit/Commit, clocks, CPU or OS leaks.
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_retry_revalidate_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# Import only data/functions, never the historical mixed-family TestCase.
from test_packet import DLL, Plan, Output, checksum

COUNT, MSS, CAPACITY, GUARD, UNWRITTEN = 32, 64, 128, 64, 0xdeadbeef
IP_HEADER = 20 if IP == 4 else 40
INITIAL_SEQUENCE, ACK, WINDOW = 0xfffffff0, 0x80000001, 65535
SOURCE_SENTINEL, OUTPUT_SENTINEL, CONTROL_SENTINEL = 0x6d, 0xa5, 0xb7
ADDRESSES = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)
MARKERS = (0, 0xffffffffffffffff, 0x0123456789abcdef, 0xfedcba9876543210)


def pseudo(protocol, length):
    suffix = struct.pack("!BBH", 0, protocol, length) if IP == 4 else struct.pack("!I3xB", length, protocol)
    return SOURCE + DESTINATION + suffix


def wire(protocol, payload, index=None):
    """Independent fixed wire layout, with no native-plan offset as oracle."""
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


class RetryRevalidateFamily32Tests(unittest.TestCase):
    def scenario(self, protocol):
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
            outputs[index] = Output(C.addressof(buffer), header + MSS - 1 if index == COUNT - 1 else CAPACITY, UNWRITTEN)
        count_storage = C.create_string_buffer(bytes([CONTROL_SENTINEL]) * (C.sizeof(C.c_uint32) + GUARD), C.sizeof(C.c_uint32) + GUARD)
        written = C.c_uint32.from_buffer(count_storage)
        calls = []

        def checked(first, pointer, count, expected_source):
            written.value = UNWRITTEN
            result = DLL.PbWriteSegmentBatchChecked(source, len(packet), C.byref(plan),
                                                  first, pointer, count, C.byref(written))
            calls.append((first, count, result, written.value))
            self.assertEqual(source.raw, expected_source)
            self.assertEqual(plan_storage.raw, plan_before)
            self.assertEqual(metadata.raw[metadata_size:], bytes([CONTROL_SENTINEL]) * GUARD)
            self.assertEqual(count_storage.raw[C.sizeof(written):], bytes([CONTROL_SENTINEL]) * GUARD)
            return result

        def check_wire(last):
            for index in range(last):
                expected = wire(protocol, parts[index], index)
                self.assertEqual((outputs[index].data, outputs[index].length), (C.addressof(buffers[index]), header + MSS))
                self.assertEqual(buffers[index].raw[:header + MSS], expected)
                self.assertEqual(buffers[index].raw[header + MSS:], bytes([OUTPUT_SENTINEL]) * (CAPACITY + GUARD - header - MSS))
                self.assertEqual(checksum(pseudo(protocol, len(expected) - IP_HEADER) + expected[IP_HEADER:]), 0)
                if IP == 4:
                    self.assertEqual(checksum(buffers[index].raw[:IP_HEADER]), 0)

        self.assertEqual(checked(0, outputs, COUNT, source_bytes), 3)
        self.assertEqual(written.value, COUNT - 1)
        check_wire(COUNT - 1)
        self.assertEqual(buffers[-1].raw, bytes([OUTPUT_SENTINEL]) * (CAPACITY + GUARD))
        self.assertEqual(outputs[-1].length, UNWRITTEN)
        prefix_bytes = [buffer.raw for buffer in buffers[:-1]]
        prefix_metadata = C.string_at(C.addressof(outputs), (COUNT - 1) * C.sizeof(Output))
        # From here the last output is large enough. Rejection must come from
        # revalidating the source rather than repeating its capacity failure.
        outputs[-1].capacity = CAPACITY
        before_bad = metadata.raw
        changed_at = IP_HEADER + 12 if protocol == 6 else 9 if IP == 4 else 6
        saved_byte = source.raw[changed_at]
        if protocol == 6:
            # DOFF5->6 is entirely readable. Four zero bytes at payload start
            # even form EOL/padding, but the canonical header/payload changes.
            self.assertEqual(source.raw[header:header + 4], bytes(4))
            source[changed_at] = b"\x60"
        else:
            # A TCP IP protocol/NextHeader is incompatible with UDP GSO.
            # UDP Length is deliberately not used as a GSO stale-plan test.
            source[changed_at] = b"\x06"
        changed_source = source.raw
        tail = C.cast(C.byref(outputs, (COUNT - 1) * C.sizeof(Output)), C.POINTER(Output))
        self.assertEqual(checked(COUNT - 1, tail, 1, changed_source), 1)
        self.assertEqual(written.value, 0)
        self.assertEqual(metadata.raw, before_bad)
        self.assertEqual([buffer.raw for buffer in buffers[:-1]], prefix_bytes)
        self.assertEqual(buffers[-1].raw, bytes([OUTPUT_SENTINEL]) * (CAPACITY + GUARD))
        source[changed_at] = bytes([saved_byte])
        self.assertEqual(source.raw, source_bytes)
        self.assertEqual(checked(COUNT - 1, tail, 1, source_bytes), 0)
        self.assertEqual(written.value, 1)
        self.assertEqual([buffer.raw for buffer in buffers[:-1]], prefix_bytes)
        self.assertEqual(C.string_at(C.addressof(outputs), (COUNT - 1) * C.sizeof(Output)), prefix_metadata)
        self.assertTrue(all(output.capacity == CAPACITY for output in outputs))
        check_wire(COUNT)
        self.assertEqual(calls, [(0, COUNT, 3, COUNT - 1), (COUNT - 1, 1, 1, 0), (COUNT - 1, 1, 0, 1)])
        print(f"RETRY_REVALIDATE32 ip={IP} protocol={protocol} outputs=32 initial_written=31 "
              "invalid_written=0 retry_written=1 direct_calls=3 prefix_source_plan_sentinels=preserved")

    def test_tcp_partial_prefix_revalidates_changed_doff_before_tail_retry(self):
        self.scenario(6)

    def test_udp_partial_prefix_revalidates_changed_ip_protocol_before_tail_retry(self):
        self.scenario(17)


if __name__ == "__main__":
    unittest.main(verbosity=2)
