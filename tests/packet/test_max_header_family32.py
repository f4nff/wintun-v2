"""One independent maximum-header TCP/LSOv2 case per IP-family process.

The required IPv6 extension-header boundary is 128 bytes; IPv4 and TCP options
can each occupy 40 bytes. The full wire oracle uses literal header geometry,
addresses and sequence rules, never offsets returned by the native plan.
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/combining-types-of-task-offloads
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/offloading-the-segmentation-of-large-tcp-packets

TCP Timestamp option bytes stay opaque and unchanged, with no clock, scheduling
or Epoch interpretation. Checked-call counts are not native CPU/copy profiling,
Submit/Commit counts, NDIS runtime execution or an OS resource-leak measurement.
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_max_header_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# Import only helpers/data, so the historical mixed-family TestCase is absent.
from test_packet import DLL, Plan, Output, checksum

COUNT, MSS = 32, 256
SEQUENCE, ACK, FIRST_ID = 0xfffffff0, 0x80000001, 0x7ff0
ADDRESSES = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)
TCP_OPTIONS = b"\x01\x01\x08\x0a" + struct.pack("!II", 0xfffffff0, 0x80000000) + b"\x01" * 28
# A single legal HBH header fills the 128-byte boundary: 2-byte header plus
# PadN(type=1, length=124) and 124 zero padding bytes. No routing pseudoheader.
IP_OPTIONS = b"\x01" * 40 if IP == 4 else b"\x06\x0f\x01\x7c" + b"\0" * 124
IP_HEADER, L4_OFFSET, HEADER = (60, 60, 120) if IP == 4 else (40, 168, 228)
CAPACITY, SENTINEL, SOURCE_SENTINEL = 600, 0xa5, 0x6d


def pseudo(length):
    suffix = struct.pack("!BBH", 0, 6, length) if IP == 4 else struct.pack("!I3xB", length, 6)
    return SOURCE + DESTINATION + suffix


def wire(payload, index, *, template=False):
    flags = 0x99 if template else 0x10 | (0x80 if index == 0 else 0) | (0x09 if index == COUNT - 1 else 0)
    tcp = bytearray(struct.pack("!HHIIHHHH", 10001, 10002,
        (SEQUENCE + index * MSS) & 0xffffffff, ACK, 0xf000 | flags, 8192, 0, 0)
        + TCP_OPTIONS + payload)
    # A valid LSO template carries the folded pseudoheader sum without TCP
    # length. Output packets instead carry complete, independently computed
    # wire checksums. LSOv2 takes its full size from NET_BUFFER, not IP length.
    value = (~checksum(pseudo(0))) & 65535 if template else checksum(pseudo(len(tcp)) + tcp)
    struct.pack_into("!H", tcp, 16, value)
    if IP == 4:
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x4f, 0x2e,
            0 if template else IP_HEADER + len(tcp), (FIRST_ID + index) & 0x7fff,
            0x4000, 64, 6, 0, SOURCE, DESTINATION) + IP_OPTIONS)
        if not template:
            struct.pack_into("!H", header, 10, checksum(header))
    else:
        header = struct.pack("!IHBB16s16s", 0x62e34567,
            0 if template else 128 + len(tcp), 0, 64, SOURCE, DESTINATION) + IP_OPTIONS
    return bytes(header + tcp)


class MaximumHeaderFamily32Tests(unittest.TestCase):
    def test_tcp_maximum_headers_preserve_one_checked_32_segment_batch(self):
        self.assertEqual(len(TCP_OPTIONS), 40)
        self.assertEqual(len(IP_OPTIONS), 40 if IP == 4 else 128)
        parts = [struct.pack("!IQQ", i, 0xffffffffffffffff if i % 2 else 0,
                             0x0123456789abcdef) + bytes([i]) * 236 for i in range(COUNT)]
        self.assertTrue(all(len(part) == MSS for part in parts))
        original = wire(b"".join(parts), 0, template=True)
        source = C.create_string_buffer(original + bytes([SOURCE_SENTINEL]) * 64, len(original) + 64)
        source_before = source.raw
        plan = Plan()
        self.assertEqual(DLL.PbPlanSegments(source, len(original), 1, MSS, C.byref(plan)), 0)
        self.assertEqual((plan.source, plan.payload, plan.count, plan.mss, plan.header,
            plan.l4, plan.l4_header, plan.pseudo_src, plan.pseudo_dst, plan.ip,
            plan.protocol, plan.kind, plan.udp_zero),
            (HEADER + COUNT * MSS, COUNT * MSS, COUNT, MSS, HEADER, L4_OFFSET, 60,
             12 if IP == 4 else 8, 16 if IP == 4 else 24, IP, 6, 1, 0))
        plan_before = bytes(plan)
        # Exact-size allocations: a ctypes-added NUL is not a payload sentinel.
        buffers = [C.create_string_buffer(bytes([SENTINEL]) * CAPACITY, CAPACITY) for _ in range(COUNT)]
        outputs = (Output * COUNT)(*(Output(C.addressof(buffer), CAPACITY, 0xffffffff) for buffer in buffers))
        written = C.c_uint32(0xffffffff)
        self.assertEqual(DLL.PbWriteSegmentBatchChecked(source, len(original),
            C.byref(plan), 0, outputs, COUNT, C.byref(written)), 0)
        self.assertEqual(written.value, COUNT)
        for index, buffer in enumerate(buffers):
            with self.subTest(segment=index):
                expected = wire(parts[index], index)
                self.assertEqual(outputs[index].data, C.addressof(buffer))
                self.assertEqual((outputs[index].capacity, outputs[index].length), (CAPACITY, HEADER + MSS))
                self.assertEqual(buffer.raw[:HEADER + MSS], expected)
                self.assertEqual(buffer.raw[HEADER + MSS:], bytes([SENTINEL]) * (CAPACITY - HEADER - MSS))
                self.assertEqual(checksum(pseudo(60 + MSS) + buffer.raw[L4_OFFSET:HEADER + MSS]), 0)
                if IP == 4:
                    self.assertEqual(checksum(buffer.raw[:IP_HEADER]), 0)
        self.assertEqual(source.raw, source_before)
        self.assertEqual(bytes(plan), plan_before)
        print(f"MAX_HEADER32 ip={IP} protocol=6 cases=1 outputs=32 checked_calls=1 "
              f"ip_options={40 if IP == 4 else 0} ipv6_extensions={128 if IP == 6 else 0} "
              "tcp_options=40 source_plan_metadata_sentinels=preserved")


if __name__ == "__main__":
    unittest.main(verbosity=2)
