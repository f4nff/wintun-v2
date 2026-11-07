"""One isolated IPv4/IPv6 TCP32 case with computed checksum0000 on every output.

TCP places the one's-complement result in its checksum field. UDP's special
computed0->FFFF rule is covered separately by test_segment_edge_family32.py;
TCP checksum0000 is not a request to omit checksum calculation. A fixed valid
LSOv2 template, odd payloads, ECN flags, timestamp options and sequence/ID wrap
are checked against independent literal wire headers, not native Plan offsets.
These are caller-owned C buffers, not mutated native/Rust loans. Direct C call
counts do not measure internal copies, Submit/Commit, clocks, CPU or NDIS.
https://www.rfc-editor.org/rfc/rfc9293.html#section-3.1
https://www.rfc-editor.org/rfc/rfc1071.html#section-1
https://www.rfc-editor.org/rfc/rfc768.html
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/offloading-the-segmentation-of-large-tcp-packets
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_tcp_zero_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
from test_packet import DLL, Plan, Output, checksum

COUNT, MSS, CAPACITY, GUARD, UNWRITTEN = 32, 63, 160, 64, 0xdeadbeef
IP_HEADER, TCP_HEADER = (20 if IP == 4 else 40), 32
HEADER = IP_HEADER + TCP_HEADER
SEQUENCE, ACK, FIRST_ID, WINDOW = 0xfffffff0, 0x80000001, 0x7ff0, 65535
TIMESTAMP = b"\x01\x01\x08\x0a" + struct.pack("!II", 0xfffffff0, 0x80000001)
WIRE_FLAGS = (0xd0,) + (0x50,) * 30 + (0x59,)
ADDRESSES = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(address).packed for address in ADDRESSES)
MARKERS = (0, 0xffffffffffffffff, 0x0123456789abcdef, 0xfedcba9876543210)


def pseudo(length):
    return SOURCE + DESTINATION + (struct.pack("!BBH", 0, 6, length) if IP == 4
                                   else struct.pack("!I3xB", length, 6))


def tcp(payload, index, flags, *, template=False):
    sequence = SEQUENCE if template else (SEQUENCE + index * MSS) & 0xffffffff
    transport = bytearray(struct.pack("!HHIIHHHH", 10001, 10002, sequence, ACK,
        0x8000 | flags, WINDOW, 0, 0) + TIMESTAMP + payload)
    value = (~checksum(pseudo(0))) & 65535 if template else checksum(pseudo(len(transport)) + transport)
    struct.pack_into("!H", transport, 16, value)
    return bytes(transport)


def wire(payload, index, *, template=False):
    transport = tcp(payload, index, 0xd9 if template else WIRE_FLAGS[index], template=template)
    ident = FIRST_ID if template else (FIRST_ID + index) & 0x7fff
    if IP == 4:
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0x2e,
            0 if template else IP_HEADER + len(transport), ident, 0x4000, 64, 6, 0,
            SOURCE, DESTINATION))
        if not template:
            struct.pack_into("!H", header, 10, checksum(header))
    else:
        header = struct.pack("!IHBB16s16s", 0x62e34567,
                             0 if template else len(transport), 6, 64, SOURCE, DESTINATION)
    return bytes(header + transport)


class TcpZeroFamily32Tests(unittest.TestCase):
    def test_32_tcp_computed_zero_checksums_stay_zero_in_one_checked_batch(self):
        parts = []
        for index in range(COUNT):
            prefix = struct.pack("!IQQ", index, MARKERS[index % 4], MARKERS[(index + 1) % 4])
            part = bytearray(prefix + bytes([index + 0x30]) * (MSS - len(prefix)))
            # Adjust one aligned word outside the opaque marker. This is a
            # Python construction step, not an output fix after the C writer.
            part[40:42] = bytes(2)
            transport = struct.pack("!HHIIHHHH", 10001, 10002,
                (SEQUENCE + index * MSS) & 0xffffffff, ACK,
                0x8000 | WIRE_FLAGS[index], WINDOW, 0, 0) + TIMESTAMP + part
            adjustment = checksum(pseudo(len(transport)) + transport)
            struct.pack_into("!H", part, 40, adjustment)
            expected = wire(part, index)
            self.assertEqual(expected[IP_HEADER + 16:IP_HEADER + 18], bytes(2))
            self.assertEqual(checksum(pseudo(TCP_HEADER + MSS) + expected[IP_HEADER:]), 0)
            self.assertEqual(part[:len(prefix)], prefix)
            parts.append(bytes(part))
        original = wire(b"".join(parts), 0, template=True)
        self.assertNotEqual(original[IP_HEADER + 16:IP_HEADER + 18], bytes(2))
        self.assertEqual(original[IP_HEADER + 13], 0xd9)
        source_bytes = b"\x6d" * GUARD + original + b"\x6d" * GUARD
        source = C.create_string_buffer(source_bytes, len(source_bytes))
        source_pointer = C.addressof(source) + GUARD
        plan_store = C.create_string_buffer(b"\xb7" * (C.sizeof(Plan) + 2 * GUARD), C.sizeof(Plan) + 2 * GUARD)
        plan = Plan.from_buffer(plan_store, GUARD)
        self.assertEqual(DLL.PbPlanSegments(source_pointer, len(original), 1, MSS, C.byref(plan)), 0)
        self.assertEqual((plan.source, plan.payload, plan.count, plan.mss, plan.header,
            plan.l4, plan.l4_header, plan.pseudo_src, plan.pseudo_dst, plan.ip,
            plan.protocol, plan.kind, plan.udp_zero),
            (len(original), COUNT * MSS, COUNT, MSS, HEADER, IP_HEADER, TCP_HEADER,
             12 if IP == 4 else 8, 16 if IP == 4 else 24, IP, 6, 1, 0))
        plan_before = plan_store.raw
        buffers = [C.create_string_buffer(b"\xa5" * (CAPACITY + 2 * GUARD), CAPACITY + 2 * GUARD) for _ in range(COUNT)]
        metadata_size = C.sizeof(Output) * COUNT
        metadata = C.create_string_buffer(b"\xb7" * (metadata_size + 2 * GUARD), metadata_size + 2 * GUARD)
        outputs = (Output * COUNT).from_buffer(metadata, GUARD)
        for index, buffer in enumerate(buffers):
            outputs[index] = Output(C.addressof(buffer) + GUARD, CAPACITY, UNWRITTEN)
        controls = [(item.data, item.capacity) for item in outputs]
        count_store = C.create_string_buffer(b"\xb7" * (C.sizeof(C.c_uint32) + 2 * GUARD), C.sizeof(C.c_uint32) + 2 * GUARD)
        written = C.c_uint32.from_buffer(count_store, GUARD)
        written.value = UNWRITTEN
        self.assertEqual(DLL.PbWriteSegmentBatchChecked(source_pointer, len(original), C.byref(plan),
                                                      0, outputs, COUNT, C.byref(written)), 0)
        self.assertEqual(written.value, COUNT)
        self.assertEqual(source.raw, source_bytes)
        self.assertEqual(plan_store.raw, plan_before)
        for store, size in ((plan_store, C.sizeof(Plan)), (metadata, metadata_size),
                            (count_store, C.sizeof(written))):
            self.assertEqual(store.raw[:GUARD], b"\xb7" * GUARD)
            self.assertEqual(store.raw[GUARD + size:], b"\xb7" * GUARD)
        self.assertEqual([(item.data, item.capacity) for item in outputs], controls)
        for index, (item, buffer) in enumerate(zip(outputs, buffers)):
            expected = wire(parts[index], index)
            actual = buffer.raw[GUARD:GUARD + item.length]
            self.assertEqual(item.length, HEADER + MSS)
            self.assertEqual(actual, expected)
            self.assertEqual(actual[IP_HEADER + 16:IP_HEADER + 18], bytes(2))
            self.assertEqual(checksum(pseudo(TCP_HEADER + MSS) + actual[IP_HEADER:]), 0)
            self.assertEqual(actual[IP_HEADER + 20:HEADER], TIMESTAMP)
            self.assertEqual(actual[IP_HEADER + 13], WIRE_FLAGS[index])
            self.assertEqual(buffer.raw[:GUARD], b"\xa5" * GUARD)
            self.assertEqual(buffer.raw[GUARD + item.length:], b"\xa5" * (CAPACITY + GUARD - item.length))
            if IP == 4:
                self.assertEqual(checksum(actual[:IP_HEADER]), 0)
        print(f"TCP_ZERO32 ip={IP} outputs=32 computed_zero=32 checksum_field=0000 "
              "odd_payload=63 plan_calls=1 checked_calls=1 source_plan_metadata_sentinels=preserved")


if __name__ == "__main__":
    unittest.main(verbosity=2)
