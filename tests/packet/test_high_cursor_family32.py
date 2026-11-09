"""Public checked packet API: the final32 MSS1 segments of a65536-byte source.

Each selected IP family runs TCP and UDP separately. The final window starts
above65400, rather than traversing the preceding segments. Literal wire headers
and independent checksums cover odd one-byte payloads, sequence/ID wrap and TCP
timestamp bytes. Those bytes are opaque protocol fields, not API clock values.

This exercises the public packet engine's65536-byte readable-span contract;
it does not claim that the SYS GSO payload cap32768 produces these sources.
Sources/outputs are caller-owned and remain stable during each call. Direct
call counts do not measure internal copies, Submit/Commit, CPU or native NDIS.
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_high_cursor_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
from test_packet import DLL, Plan, Output

COUNT, SPAN, MSS, CAPACITY, GUARD = 32, 65536, 1, 128, 64
UNWRITTEN, PAYLOAD_SENTINEL, CONTROL_SENTINEL = 0xdeadbeef, 0xa5, 0xb7
IP_HEADER, ACK, WINDOW = (20 if IP == 4 else 40), 0x80000001, 12345
TIMESTAMP = b"\x01\x01\x08\x0a" + struct.pack("!II", 0xfffffff0, 0x80000001)
ADDRESSES = ("10.1.2.3", "10.5.6.7") if IP == 4 else ("2001:db8:1::1234", "2001:db8:2::5678")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)


def checksum(data):
    data += b"\0" * (len(data) & 1)
    value = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return ~value & 65535


def pseudo(protocol, length):
    return SOURCE + DESTINATION + (struct.pack("!BBH", 0, protocol, length) if IP == 4
                                   else struct.pack("!I3xB", length, protocol))


def wire(protocol, payload, sequence, identity, *, index=None, final=False):
    if protocol == 6:
        flags = 0xd9 if index is None else 0x50 | (0x09 if final else 0) | (0x80 if index == 0 else 0)
        transport = bytearray(struct.pack("!HHIIHHHH", 10001, 20002,
            sequence if index is None else (sequence + index) & 0xffffffff,
            ACK, 0x8000 | flags, WINDOW, 0, 0) + TIMESTAMP + payload)
        at = 16
    else:
        transport = bytearray(struct.pack("!HHHH", 10001, 20002, 8 + len(payload), 0) + payload)
        at = 6
    # A GSO template carries a nonzero pseudoheader seed. Expected wire output
    # is independently finalized; TCP computed0 remains0000, UDP becomesFFFF.
    value = checksum(pseudo(protocol, 0)) ^ 65535 if index is None else checksum(pseudo(protocol, len(transport)) + transport)
    struct.pack_into("!H", transport, at, value or (65535 if protocol == 17 else 0))
    if IP == 4:
        ident = identity if index is None else (identity + index) & (0x7fff if protocol == 6 else 0xffff)
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0x2e,
            0 if index is None else IP_HEADER + len(transport), ident, 0x4000, 61,
            protocol, 0, SOURCE, DESTINATION))
        if index is not None:
            struct.pack_into("!H", header, 10, checksum(bytes(header)))
        return bytes(header + transport)
    return struct.pack("!IHBB16s16s", 0x62e34567,
        0 if index is None else len(transport), protocol, 61, SOURCE, DESTINATION) + transport


def guarded(size, value):
    return C.create_string_buffer(bytes([value]) * (size + 2 * GUARD), size + 2 * GUARD)


class HighCursorFamily32Tests(unittest.TestCase):
    def test_tcp_final32_at_maximum_source_span(self):
        self.check_protocol(6, 1)

    def test_udp_final32_at_maximum_source_span(self):
        self.check_protocol(17, 2)

    def check_protocol(self, protocol, kind):
        transport_header = 32 if protocol == 6 else 8
        header = IP_HEADER + transport_header
        segments = SPAN - header
        first = segments - COUNT
        # Place both wraps inside this high-index32-output window. The source
        # constants come from the literal source geometry, not native Plan.
        sequence = (0xfffffff0 - first) & 0xffffffff
        identity = ((0x7ff0 if protocol == 6 else 0xfff0) - first) & (0x7fff if protocol == 6 else 0xffff)
        payload = bytes((index * 11 + 7) & 255 for index in range(segments))
        original = wire(protocol, payload, sequence, identity)
        self.assertEqual(len(original), SPAN)
        self.assertGreater(first, 65400)
        source = guarded(SPAN, PAYLOAD_SENTINEL)
        source_pointer = C.addressof(source) + GUARD
        C.memmove(source_pointer, original, SPAN)
        source_before = source.raw
        plan_store = guarded(C.sizeof(Plan), CONTROL_SENTINEL)
        plan = Plan.from_buffer(plan_store, GUARD)
        self.assertEqual(DLL.PbPlanSegments(source_pointer, SPAN, kind, MSS, C.byref(plan)), 0)
        self.assertEqual((plan.source, plan.payload, plan.count, plan.mss, plan.header,
            plan.l4, plan.l4_header, plan.pseudo_src, plan.pseudo_dst, plan.ip,
            plan.protocol, plan.kind, plan.udp_zero),
            (SPAN, segments, segments, MSS, header, IP_HEADER, transport_header,
             12 if IP == 4 else 8, 16 if IP == 4 else 24, IP, protocol, kind, 0))
        plan_before = plan_store.raw
        buffers = [guarded(CAPACITY, PAYLOAD_SENTINEL) for _ in range(COUNT)]
        metadata_size = C.sizeof(Output) * COUNT
        metadata = guarded(metadata_size, CONTROL_SENTINEL)
        outputs = (Output * COUNT).from_buffer(metadata, GUARD)
        for output, buffer in zip(outputs, buffers):
            output.data, output.capacity, output.length = C.addressof(buffer) + GUARD, CAPACITY, UNWRITTEN
        count_store = guarded(C.sizeof(C.c_uint32), CONTROL_SENTINEL)
        written = C.c_uint32.from_buffer(count_store, GUARD)
        checked_calls = 0

        def unchanged_inputs():
            self.assertEqual(source.raw, source_before)
            self.assertEqual(plan_store.raw, plan_before)
            for store, size in ((metadata, metadata_size), (count_store, C.sizeof(written))):
                self.assertEqual(store.raw[:GUARD], bytes([CONTROL_SENTINEL]) * GUARD)
                self.assertEqual(store.raw[GUARD + size:], bytes([CONTROL_SENTINEL]) * GUARD)

        def invoke(cursor, status, expected_count):
            nonlocal checked_calls
            written.value = UNWRITTEN
            checked_calls += 1
            self.assertEqual(DLL.PbWriteSegmentBatchChecked(source_pointer, SPAN, C.byref(plan),
                cursor, outputs, COUNT, C.byref(written)), status)
            self.assertEqual(written.value, expected_count)
            unchanged_inputs()

        def no_output_change(cursor, status):
            metadata_before = metadata.raw
            buffers_before = [buffer.raw for buffer in buffers]
            invoke(cursor, status, 0)
            self.assertEqual(metadata.raw, metadata_before)
            self.assertEqual([buffer.raw for buffer in buffers], buffers_before)

        def check_outputs(cursor, accepted, previous_metadata, previous_buffers):
            expected_metadata = bytearray(previous_metadata)
            for offset, (output, buffer) in enumerate(zip(outputs, buffers)):
                if offset >= accepted:
                    self.assertEqual(buffer.raw, previous_buffers[offset])
                    continue
                index = cursor + offset
                expected = wire(protocol, payload[index:index + MSS], sequence, identity,
                                index=index, final=index == segments - 1)
                self.assertEqual(output.length, header + MSS)
                self.assertEqual(buffer.raw, bytes([PAYLOAD_SENTINEL]) * GUARD + expected
                    + bytes([PAYLOAD_SENTINEL]) * (CAPACITY - len(expected) + GUARD))
                struct.pack_into("=I", expected_metadata,
                    GUARD + offset * C.sizeof(Output) + Output.length.offset, len(expected))
                self.assertEqual(checksum(pseudo(protocol, transport_header + MSS)
                                          + expected[IP_HEADER:]), 0)
                if IP == 4:
                    self.assertEqual(checksum(expected[:IP_HEADER]), 0)
                    self.assertEqual(struct.unpack_from("!H", expected, 4)[0],
                        (identity + index) & (0x7fff if protocol == 6 else 0xffff))
                if protocol == 6:
                    self.assertEqual(expected[IP_HEADER + 20:header], TIMESTAMP)
                    self.assertEqual(expected[IP_HEADER + 13], 0x59 if index == segments - 1 else 0x50)
                    self.assertEqual(struct.unpack_from("!I", expected, IP_HEADER + 4)[0],
                                     (sequence + index) & 0xffffffff)
            # Pointer, capacity, padding, inactive Length entries and both
            # metadata guards are checked, not just each emitted wire packet.
            self.assertEqual(metadata.raw, bytes(expected_metadata))

        with self.subTest(boundary="one-past-end-with-zero-capacity"):
            outputs[0].capacity = 0
            no_output_change(segments, 1)  # INVALID, before any segment write.
        with self.subTest(boundary="zero-capacity-then-retry32"):
            no_output_change(first, 3)  # NO_SPACE with no accepted prefix.
            outputs[0].capacity = CAPACITY
            before_metadata, before_buffers = metadata.raw, [buffer.raw for buffer in buffers]
            invoke(first, 0, COUNT)
            check_outputs(first, COUNT, before_metadata, before_buffers)
        with self.subTest(boundary="remaining31-clamps-request32"):
            for output, buffer in zip(outputs, buffers):
                C.memset(C.addressof(buffer), PAYLOAD_SENTINEL, C.sizeof(buffer))
                output.length = UNWRITTEN
            before_metadata, before_buffers = metadata.raw, [buffer.raw for buffer in buffers]
            invoke(first + 1, 0, COUNT - 1)
            check_outputs(first + 1, COUNT - 1, before_metadata, before_buffers)
        with self.subTest(boundary="one-past-end-with-positive-capacity"):
            no_output_change(segments, 1)
        self.assertEqual(checked_calls, 5)
        print(f"HIGH_CURSOR32 ip={IP} protocol={protocol} source={SPAN} mss={MSS} "
              f"segments={segments} first={first} scenarios=4 plan_calls=1 checked_calls=5 "
              "outputs=63 tail_request32_written31 source_plan_metadata_sentinels=preserved")


if __name__ == "__main__":
    unittest.main(verbosity=2)
