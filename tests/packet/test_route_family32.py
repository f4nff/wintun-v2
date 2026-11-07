"""One address family per process; independent route-aware 32-segment oracle."""
import ctypes as C
from dataclasses import dataclass
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_route_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# Import data/functions, not the old module's mixed-family TestCase.
from test_packet import DLL, Plan, Output, checksum

MSS, COUNT, CAPACITY = 1000, 32, 1200
INITIAL_SEQUENCE, INITIAL_ID, UNWRITTEN = 0xfffffff0, 0xfffe, 0xdeadbeef
TIMESTAMP = b"\x01\x01\x08\x0a" + struct.pack("!II", 0xfffffff0, 0x80000001)
# Opaque application markers, not packet scheduling clocks.
PARTS = tuple(struct.pack("!IQQ", i, 0x0123456789abcdef, 0xfedcba9876543210)
              + bytes([i]) * 980 for i in range(COUNT))


@dataclass(frozen=True)
class RoutePacket:
    name: str
    data: bytes
    offset: int
    source: bytes
    destination: bytes
    pseudo_offsets: tuple[int, int]


def pseudo(source, destination, length):
    suffix = (struct.pack("!BBH", 0, 6, length) if IP == 4
              else struct.pack("!I3xB", length, 6))
    return source + destination + suffix


def route_packets():
    """Build packet layouts from constants, independently of the C plan.

    TCP source routing uses the ultimate destination (RFC9293, 3.9.2.1):
    https://www.rfc-editor.org/rfc/rfc9293#section-3.9.2.1
    RH2's processed form swaps the destination and decrements Segments Left;
    it is an internal packet-state model, not a claim about native NDIS input:
    https://www.rfc-editor.org/rfc/rfc6275#section-11.3.3
    HAO is placed after RH2 and has option alignment8n+6 (RFC6275, 6.3):
    https://www.rfc-editor.org/rfc/rfc6275#section-6.3
    """
    names = ("lsrr-active", "lsrr-consumed", "ssrr-active", "ssrr-consumed") if IP == 4 else (
        "rh2-active", "rh2-consumed", "home-only", "home-rh2")
    packets = []
    for name in names:
        tcp = bytearray(struct.pack("!HHIIHHHH", 10001, 10002, INITIAL_SEQUENCE,
                                    42, (8 << 12) | 0x19, 8192, 0, 0)
                        + TIMESTAMP + b"".join(PARTS))
        if IP == 4:
            source = ipaddress.IPv4Address("10.0.0.1").packed
            hop = ipaddress.IPv4Address("10.0.0.2").packed
            destination = ipaddress.IPv4Address("203.0.113.5").packed
            consumed = name.endswith("consumed")
            kind = 131 if name.startswith("lsrr") else 137
            # One route address: pointer4 selects it; pointer8 means consumed.
            options = bytes([kind, 7, 8 if consumed else 4]) + (
                hop if consumed else destination) + b"\0"
            offset = 28
            header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x47, 0,
                offset + len(tcp), INITIAL_ID, 0x4000, 64, 6, 0, source,
                destination if consumed else hop) + options)
            struct.pack_into("!H", header, 10, checksum(bytes(header)))
            offsets = (12, 16 if consumed else 23)
        else:
            care_source = ipaddress.IPv6Address("2001:db8::1").packed
            hop = ipaddress.IPv6Address("2001:db8::2").packed
            home_source = ipaddress.IPv6Address("2001:db8:aa::9").packed
            home_destination = ipaddress.IPv6Address("2001:db8:bb::7").packed
            has_routing = name.startswith("rh2") or name == "home-rh2"
            has_home = name.startswith("home")
            consumed = name == "rh2-consumed"
            # Two header bytes + four PadN bytes put HAO's type at8n+6.
            home = (bytes([6, 2, 1, 2, 0, 0, 0xc9, 16]) + home_source
                    if has_home else b"")
            routing = (bytes([60 if home else 6, 2, 2, 0 if consumed else 1])
                       + bytes(4) + (hop if consumed else home_destination)
                       if has_routing else b"")
            extension = routing + home
            wire_destination = home_destination if consumed else hop
            offset = 40 + len(extension)
            header = bytearray(struct.pack("!IHBB16s16s", 0x60000000,
                len(extension) + len(tcp), 43 if routing else 60, 64,
                care_source, wire_destination) + extension)
            source = home_source if home else care_source
            destination = home_destination if routing else wire_destination
            offsets = (40 + len(routing) + 8 if home else 8,
                       24 if consumed or not routing else 48)
            if home:
                assert (40 + len(routing) + 6) % 8 == 6
        struct.pack_into("!H", tcp, 16, checksum(pseudo(source, destination, len(tcp)) + tcp))
        packets.append(RoutePacket(name, bytes(header + tcp), offset,
                                   source, destination, offsets))
    return packets


def expected_segment(packet, index):
    """Wire bytes from input/header constants and endpoint constants, not Plan."""
    offset = packet.offset
    output = bytearray(packet.data[:offset + 32] + PARTS[index])
    if IP == 4:
        struct.pack_into("!HH", output, 2, len(output), (INITIAL_ID + index) & 0x7fff)
        output[10:12] = b"\0\0"
        struct.pack_into("!H", output, 10, checksum(bytes(output[:offset])))
    else:
        struct.pack_into("!H", output, 4, len(output) - 40)
    struct.pack_into("!I", output, offset + 4, (INITIAL_SEQUENCE + index * MSS) & 0xffffffff)
    output[offset + 13] = 0x19 if index == COUNT - 1 else 0x10
    output[offset + 16:offset + 18] = b"\0\0"
    struct.pack_into("!H", output, offset + 16, checksum(
        pseudo(packet.source, packet.destination, len(output) - offset) + output[offset:]))
    return bytes(output)


class RouteFamily32Tests(unittest.TestCase):
    def check_outputs(self, packet, buffers, outputs, count):
        for i in range(count):
            expected = expected_segment(packet, i)
            data = buffers[i].raw
            self.assertEqual(outputs[i].length, len(expected))
            self.assertEqual(data[:len(expected)], expected)
            self.assertEqual(data[len(expected):], b"\xa5" * (CAPACITY - len(expected)) + b"\0")
            self.assertEqual(data[packet.offset + 20:packet.offset + 32], TIMESTAMP)
            self.assertEqual(checksum(pseudo(packet.source, packet.destination,
                len(expected) - packet.offset) + data[packet.offset:len(expected)]), 0)
            if IP == 4:
                self.assertEqual(checksum(data[:packet.offset]), 0)

    def check_case(self, packet, short):
        self.assertTrue(all(len(part) == MSS for part in PARTS))
        self.assertEqual(checksum(pseudo(packet.source, packet.destination,
            len(packet.data) - packet.offset) + packet.data[packet.offset:]), 0)
        if IP == 4:
            self.assertEqual(checksum(packet.data[:packet.offset]), 0)
        readable = C.create_string_buffer(packet.data)
        source_before = readable.raw
        plan = Plan()
        self.assertEqual(DLL.PbPlanSegments(readable, len(packet.data), 1, MSS, C.byref(plan)), 0)
        self.assertEqual((plan.source, plan.payload, plan.count, plan.mss, plan.header,
            plan.l4, plan.l4_header, plan.pseudo_src, plan.pseudo_dst,
            plan.ip, plan.protocol, plan.kind),
            (len(packet.data), COUNT * MSS, COUNT, MSS, packet.offset + 32,
             packet.offset, 32, *packet.pseudo_offsets, IP, 6, 1))
        plan_before = bytes(plan)
        buffers = [C.create_string_buffer(b"\xa5" * CAPACITY) for _ in range(COUNT)]
        length = packet.offset + 32 + MSS
        outputs = (Output * COUNT)(*(Output(C.addressof(buffer),
            length - 1 if short and i == COUNT - 1 else CAPACITY, UNWRITTEN)
            for i, buffer in enumerate(buffers)))
        controls = [(item.data, item.capacity) for item in outputs]
        written = C.c_uint32(UNWRITTEN)
        calls = []

        def write(first, pointer, count):
            calls.append((first, count))
            result = DLL.PbWriteSegmentBatchChecked(readable, len(packet.data),
                C.byref(plan), first, pointer, count, C.byref(written))
            self.assertEqual(readable.raw, source_before)
            self.assertEqual(bytes(plan), plan_before)
            return result

        self.assertEqual(write(0, outputs, COUNT), 3 if short else 0)
        self.assertEqual(written.value, COUNT - 1 if short else COUNT)
        self.assertEqual([(item.data, item.capacity) for item in outputs], controls)
        if short:
            self.check_outputs(packet, buffers, outputs, COUNT - 1)
            self.assertEqual(buffers[-1].raw, b"\xa5" * CAPACITY + b"\0")
            self.assertEqual(outputs[-1].length, UNWRITTEN)
            prefix = [buffer.raw for buffer in buffers[:-1]]
            outputs[-1].capacity = CAPACITY
            controls[-1] = (outputs[-1].data, CAPACITY)
            tail = C.cast(C.byref(outputs, (COUNT - 1) * C.sizeof(Output)), C.POINTER(Output))
            self.assertEqual(write(COUNT - 1, tail, 1), 0)
            self.assertEqual(written.value, 1)
            self.assertEqual([buffer.raw for buffer in buffers[:-1]], prefix)
        self.check_outputs(packet, buffers, outputs, COUNT)
        self.assertEqual([(item.data, item.capacity) for item in outputs], controls)
        self.assertEqual(calls, [(0, COUNT), (COUNT - 1, 1)] if short else [(0, COUNT)])
        print(f"ROUTE32 ip={IP} route={packet.name} short31={int(short)} outputs=32 "
              f"checked_calls={len(calls)} route_checksum=independent source_timestamp_sentinels=preserved")

    def test_32_route_outputs_match_independent_endpoint_oracle(self):
        for packet in route_packets():
            with self.subTest(ip=IP, route=packet.name):
                self.check_case(packet, False)

    def test_short_output_31_retries_only_the_unwritten_last_segment(self):
        for packet in route_packets():
            with self.subTest(ip=IP, route=packet.name):
                self.check_case(packet, True)


if __name__ == "__main__":
    print(f"FAMILY: IPv{IP}", flush=True)
    unittest.main(verbosity=2)
