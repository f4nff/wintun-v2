"""Odd-segment/zero-checksum contracts; one IPv4 or IPv6 process at a time."""
import ctypes as C
from dataclasses import dataclass
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_segment_edge_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
from test_packet import DLL, Plan, Output, packet, checksum

MSS, COUNT, CAPACITY, UNWRITTEN = 63, 32, 160, 0xdeadbeef
SEQUENCE = 0xfffffff0
TIMESTAMP = b"\x01\x01\x08\x0a" + struct.pack("!II", 0xfffffff0, 0x80000001)
SOURCE = ipaddress.ip_address("10.0.0.1" if IP == 4 else "2001:db8::1").packed
DESTINATION = ipaddress.ip_address("10.0.0.2" if IP == 4 else "2001:db8::2").packed


def pseudo(protocol, length):
    suffix = (struct.pack("!BBH", 0, protocol, length) if IP == 4
              else struct.pack("!I3xB", length, protocol))
    return SOURCE + DESTINATION + suffix


def payloads(tail_one):
    # The two64-bit values are opaque application bytes, not API timestamps.
    parts = [struct.pack("!IQQ", i, 0x0123456789abcdef, 0xfedcba9876543210)
             + bytes([i]) * 43 for i in range(COUNT)]
    if tail_one:
        parts[-1] = bytes([COUNT - 1])
    return parts


@dataclass(frozen=True)
class SegmentedInput:
    protocol: int
    flags: int
    tail_one: bool
    disabled: bool
    parts: tuple[bytes, ...]
    data: bytes

    @property
    def offset(self):
        return 20 if IP == 4 else 40

    @property
    def header(self):
        return self.offset + (32 if self.protocol == 6 else 8)


def build_input(protocol, flags, tail_one, disabled=False):
    parts = payloads(tail_one)
    if protocol == 17:
        # Force one's-complement checksum result0 in segment17, independently
        # of the C engine. Change a payload word outside the opaque markers.
        tuned = bytearray(parts[17])
        tuned[40:42] = b"\0\0"
        udp = struct.pack("!HHHH", 10001, 10002, 8 + MSS, 0)
        struct.pack_into("!H", tuned, 40, checksum(pseudo(17, 8 + MSS) + udp + tuned))
        assert checksum(pseudo(17, 8 + MSS) + udp + tuned) == 0
        parts[17] = bytes(tuned)
    raw = bytearray(packet(IP, protocol, b"".join(parts), flags=flags, seq=SEQUENCE,
        tcp_options=TIMESTAMP if protocol == 6 else b"", gso=protocol == 6))
    if IP == 4:
        struct.pack_into("!H", raw, 4, 0x7ffe if protocol == 6 else 0xfffe)
    # NDIS LSO/USO supplies one's-complement pseudoheader sum excluding length,
    # not the final packet checksum. Inputzero is a separate USO directive.
    seed = checksum(pseudo(protocol, 0)) ^ 65535
    assert seed != 0
    offset = 20 if IP == 4 else 40
    struct.pack_into("!H", raw, offset + (16 if protocol == 6 else 6), 0 if disabled else seed)
    return SegmentedInput(protocol, flags, tail_one, disabled, tuple(parts), bytes(raw))


def expected_segment(source, index):
    # Offsets/flags/endpoints come from our fixed packet layout and protocol
    # contract. The native Plan is checked separately and is never the oracle.
    output = bytearray(source.data[:source.header] + source.parts[index])
    if IP == 4:
        mask = 0x7fff if source.protocol == 6 else 0xffff
        initial = 0x7ffe if source.protocol == 6 else 0xfffe
        struct.pack_into("!HH", output, 2, len(output), (initial + index) & mask)
        output[10:12] = b"\0\0"
        struct.pack_into("!H", output, 10, checksum(bytes(output[:20])))
    else:
        struct.pack_into("!H", output, 4, len(output) - 40)
    if source.protocol == 6:
        struct.pack_into("!I", output, source.offset + 4, (SEQUENCE + index * MSS) & 0xffffffff)
        # Independent wire expectations: CWR only first, ECE on every segment,
        # ACK on every segment, FIN/PSH only last. No URG/RST/SYN is requested.
        first, middle, last = {0x99: (0x90, 0x10, 0x19), 0xd9: (0xd0, 0x50, 0x59)}[source.flags]
        output[source.offset + 13] = first if index == 0 else last if index == COUNT - 1 else middle
    else:
        struct.pack_into("!H", output, source.offset + 4, len(output) - source.offset)
    at = source.offset + (16 if source.protocol == 6 else 6)
    output[at:at + 2] = b"\0\0"
    value = checksum(pseudo(source.protocol, len(output) - source.offset) + output[source.offset:])
    if source.disabled:
        value = 0
    elif source.protocol == 17 and value == 0:
        value = 65535
    struct.pack_into("!H", output, at, value)
    return bytes(output)


class SegmentEdgeFamily32Tests(unittest.TestCase):
    def check_outputs(self, source, buffers, outputs, count):
        for index in range(count):
            expected = expected_segment(source, index)
            actual = buffers[index].raw
            self.assertEqual(outputs[index].length, len(expected))
            self.assertEqual(actual[:len(expected)], expected)
            self.assertEqual(actual[len(expected):], b"\xa5" * (CAPACITY - len(expected)) + b"\0")
            if IP == 4:
                self.assertEqual(checksum(actual[:20]), 0)
            if not source.disabled:
                self.assertEqual(checksum(pseudo(source.protocol, len(expected) - source.offset)
                                          + actual[source.offset:len(expected)]), 0)
            if source.protocol == 6:
                self.assertEqual(actual[source.offset + 20:source.header], TIMESTAMP)
            if source.protocol == 17 and index == 17:
                self.assertEqual(actual[source.offset + 6:source.offset + 8],
                                 b"\0\0" if source.disabled else b"\xff\xff")

    def check_case(self, source, short):
        self.assertEqual([len(p) for p in source.parts], [MSS] * 31 + [1 if source.tail_one else MSS])
        readable = C.create_string_buffer(source.data)
        source_before = readable.raw
        plan = Plan()
        self.assertEqual(DLL.PbPlanSegments(readable, len(source.data), 1 if source.protocol == 6 else 2,
                                         MSS, C.byref(plan)), 0)
        self.assertEqual(readable.raw, source_before)
        self.assertEqual((plan.source, plan.payload, plan.count, plan.mss, plan.header, plan.l4,
            plan.l4_header, plan.pseudo_src, plan.pseudo_dst, plan.ip, plan.protocol, plan.kind, plan.udp_zero),
            (len(source.data), sum(map(len, source.parts)), COUNT, MSS, source.header, source.offset,
             32 if source.protocol == 6 else 8, 12 if IP == 4 else 8, 16 if IP == 4 else 24,
             IP, source.protocol, 1 if source.protocol == 6 else 2, int(source.disabled)))
        plan_before = bytes(plan)
        buffers = [C.create_string_buffer(b"\xa5" * CAPACITY) for _ in range(COUNT)]
        last_length = source.header + len(source.parts[-1])
        outputs = (Output * COUNT)(*(Output(C.addressof(buffer),
            last_length - 1 if short and i == COUNT - 1 else CAPACITY, UNWRITTEN)
            for i, buffer in enumerate(buffers)))
        controls = [(item.data, item.capacity) for item in outputs]
        written = C.c_uint32(UNWRITTEN)
        calls = []

        def write(first, pointer, count):
            calls.append((first, count))
            result = DLL.PbWriteSegmentBatchChecked(readable, len(source.data), C.byref(plan),
                                                   first, pointer, count, C.byref(written))
            self.assertEqual(readable.raw, source_before)
            self.assertEqual(bytes(plan), plan_before)
            return result

        self.assertEqual(write(0, outputs, COUNT), 3 if short else 0)
        self.assertEqual(written.value, COUNT - 1 if short else COUNT)
        self.assertEqual([(item.data, item.capacity) for item in outputs], controls)
        if short:
            self.check_outputs(source, buffers, outputs, COUNT - 1)
            self.assertEqual(buffers[-1].raw, b"\xa5" * CAPACITY + b"\0")
            self.assertEqual(outputs[-1].length, UNWRITTEN)
            prefix = [b.raw for b in buffers[:-1]]
            outputs[-1].capacity = CAPACITY
            controls[-1] = (outputs[-1].data, CAPACITY)
            tail = C.cast(C.byref(outputs, (COUNT - 1) * C.sizeof(Output)), C.POINTER(Output))
            self.assertEqual(write(COUNT - 1, tail, 1), 0)
            self.assertEqual(written.value, 1)
            self.assertEqual([b.raw for b in buffers[:-1]], prefix)
        self.check_outputs(source, buffers, outputs, COUNT)
        self.assertEqual([(item.data, item.capacity) for item in outputs], controls)
        self.assertEqual(calls, [(0, COUNT), (COUNT - 1, 1)] if short else [(0, COUNT)])
        variant = f"flags{source.flags:02x}" if source.protocol == 6 else "input-zero" if source.disabled else "computed-zero"
        print(f"SEGMENT_EDGE32 ip={IP} protocol={source.protocol} variant={variant} tail1={int(source.tail_one)} "
              f"short31={int(short)} outputs=32 checked_calls={len(calls)} source_plan_sentinels=preserved")

    def test_tcp_ecn_timestamp_and_wrap_with_odd_mss_and_one_byte_tail(self):
        # LSO requirements specify CWR/FIN/PSH and unchanged timestamp options:
        # https://learn.microsoft.com/en-us/windows-hardware/drivers/network/offloading-the-segmentation-of-large-tcp-packets
        for flags in (0x99, 0xd9):
            for short in (False, True):
                with self.subTest(ip=IP, flags=flags, short31=short):
                    self.check_case(build_input(6, flags, True), short)

    def test_udp_ndis_zero_seed_is_distinct_from_computed_zero_checksum(self):
        # USO explicitly preserves inputzero; this is not a claim that ordinary
        # IPv6 wire packets may omit checksums without a configured exception.
        # SubMSS final support is advertised by this project only at NDIS6.84+;
        # the divisible32*63 shape also fits its NDIS6.83 USO contract.
        # https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-segmentation-offload-uso-
        for tail_one in (False, True):
            for disabled in (False, True):
                for short in (False, True):
                    with self.subTest(ip=IP, tail1=tail_one, input_zero=disabled, short31=short):
                        self.check_case(build_input(17, 0x10, tail_one, disabled), short)


if __name__ == "__main__":
    print(f"FAMILY: IPv{IP}", flush=True)
    unittest.main(verbosity=2)
