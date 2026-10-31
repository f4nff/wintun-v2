"""NDIS RSC/URO reference outputs, checked with an independent packet/checksum oracle."""
import ctypes as C
import struct
import unittest
from test_packet import DLL, packet, checksum


class State(C.Structure):
    _fields_ = [("buffer", C.c_void_p)] + [(n, C.c_uint32) for n in (
        "capacity", "length", "count", "mss", "next_sequence", "payload")] + [
        ("header", C.c_uint16), ("l4", C.c_uint16)] + [(n, C.c_uint8) for n in ("ip", "protocol", "short", "final")]


DLL.PbCoalesceBegin.argtypes = [C.POINTER(State), C.c_void_p, C.c_uint32, C.c_void_p, C.c_uint32]
DLL.PbCoalesceAppend.argtypes = [C.POINTER(State), C.c_void_p, C.c_uint32]
DLL.PbCoalesceFinish.argtypes = [C.POINTER(State)]


def repair(data):
    data = bytearray(data)
    ip = data[0] >> 4
    offset = (data[0] & 15) * 4 if ip == 4 else 40
    protocol = data[9] if ip == 4 else data[6]
    if ip == 4:
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


def wire(ip, protocol, data, seq=0xfffffff0, flags=0x10):
    return repair(packet(ip, protocol, data, seq=seq, flags=flags, gso=False))


class CoalescingTests(unittest.TestCase):
    def start(self, source, capacity=65535):
        output = C.create_string_buffer(65536)
        output.raw = b"\xa5" * len(output)
        state = State()
        status = DLL.PbCoalesceBegin(C.byref(state), output, capacity, source, len(source))
        return state, output, status

    def append(self, state, source):
        return DLL.PbCoalesceAppend(C.byref(state), source, len(source))

    def test_tcp_payload_sequence_checksums_and_minimum_ttl(self):
        for ip in (4, 6):
            payloads = [b"a" * 900, b"b" * 1200, b"c" * 200]
            source = wire(ip, 6, payloads[0])
            state, output, status = self.start(source)
            self.assertEqual(status, 0)
            sequence = (0xfffffff0 + 900) & 0xffffffff
            for index in (1, 2):
                data = bytearray(wire(ip, 6, payloads[index], sequence))
                data[8 if ip == 4 else 7] = 64 - index
                self.assertEqual(self.append(state, repair(data)), 0)
                sequence = (sequence + len(payloads[index])) & 0xffffffff
            self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
            data = output.raw[:state.length]
            self.assertEqual(data[state.header:], b"".join(payloads))
            self.assertEqual(state.count, 3)
            self.assertEqual(data[8 if ip == 4 else 7], 62)
            self.assertEqual(struct.unpack_from("!I", data, state.l4 + 4)[0], 0xfffffff0)
            self.assertEqual(repair(data), data)
            self.assertEqual(data[state.l4 + 13], 0x10)

    def test_udp_boundaries_short_final_and_ndis_checksum_representation(self):
        for ip in (4, 6):
            payloads = [b"a" * 1000, b"b" * 1000, b"c" * 77]
            state, output, status = self.start(wire(ip, 17, payloads[0]))
            self.assertEqual(status, 0)
            for payload in payloads[1:]: self.assertEqual(self.append(state, wire(ip, 17, payload)), 0)
            self.assertEqual(state.short, 1)
            saved = output.raw
            self.assertNotEqual(self.append(state, wire(ip, 17, b"d")), 0)
            self.assertEqual(output.raw, saved)
            self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
            data = output.raw[:state.length]
            self.assertEqual(data[state.header:], b"".join(payloads))
            self.assertEqual((state.count, state.mss), (3, 1000))
            self.assertEqual(struct.unpack_from("!H", data, state.l4 + 4)[0], len(data) - state.l4)
            self.assertEqual(data[state.l4 + 6:state.l4 + 8], b"\0\0")
            if ip == 4: self.assertEqual(data[10:12], b"\0\0")
            else: self.assertEqual(struct.unpack_from("!H", data, 4)[0], len(data) - 40)

    def test_checksum_failure_is_a_boundary_and_does_not_mutate_pending_unit(self):
        for protocol in (6, 17):
            first = wire(4, protocol, b"a" * 20)
            state, output, _ = self.start(first)
            bad = bytearray(wire(4, protocol, b"b" * 20, 4))
            bad[-1] ^= 1
            self.assertEqual(self.append(state, bytes(bad)), 1)
            self.assertEqual((state.count, output.raw[:len(first)]), (1, first))
            self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
            self.assertEqual(output.raw[:len(first)], first)
            bad = bytearray(first); bad[10] ^= 1
            self.assertEqual(self.start(bytes(bad))[2], 1)

    def test_changed_flow_header_ack_window_and_ecn_are_not_coalesced(self):
        for ip in (4, 6):
            for protocol in (6, 17):
                first = wire(ip, protocol, b"a" * 20)
                offset = 20 if ip == 4 else 40
                mutations = [(offset, 1), (12 if ip == 4 else 8, 1), (1, 4), (6 if ip == 4 else 2, 0x40)]
                if protocol == 6: mutations += [(offset + 8, 1), (offset + 14, 1)]
                else: mutations += [(8 if ip == 4 else 7, 1)]
                for at, bit in mutations:
                    with self.subTest(ip=ip, protocol=protocol, at=at):
                        state, output, _ = self.start(first)
                        next_packet = bytearray(wire(ip, protocol, b"b" * 20, 4)); next_packet[at] ^= bit
                        self.assertNotEqual(self.append(state, repair(next_packet)), 0)
                        self.assertEqual((state.count, output.raw[:len(first)]), (1, first))

    def test_tcp_gaps_duplicates_control_packets_and_pure_ack_are_individual(self):
        for ip in (4, 6):
            first = wire(ip, 6, b"a" * 20)
            for sequence in (3, 5, 0xfffffff0):
                state, output, _ = self.start(first)
                self.assertEqual(self.append(state, wire(ip, 6, b"b" * 20, sequence)), 2)
                self.assertEqual(output.raw[:len(first)], first)
            for flags in (0x00, 0x08, 0x11, 0x12, 0x14, 0x30, 0x50, 0x90):
                self.assertEqual(self.start(wire(ip, 6, b"x", flags=flags))[2], 2)
            self.assertEqual(self.start(wire(ip, 6, b""))[2], 2)

    def test_tcp_psh_is_ored_without_changing_payload_sequence_or_reject_boundaries(self):
        for ip in (4, 6):
            for flags in [(0x10, 0x18, 0x10), (0x18, 0x10, 0x10), (0x18, 0x18, 0x18)]:
                with self.subTest(ip=ip, flags=flags):
                    payloads = [b"a" * 20, b"b" * 30, b"c" * 7]
                    sequence = 0xfffffff0
                    state, output, status = self.start(wire(ip, 6, payloads[0], sequence, flags[0]))
                    self.assertEqual(status, 0)
                    for part in (1, 2):
                        sequence = (sequence + len(payloads[part - 1])) & 0xffffffff
                        self.assertEqual(self.append(state, wire(ip, 6, payloads[part], sequence, flags[part])), 0)
                    self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
                    data = output.raw[:state.length]
                    self.assertEqual(data[state.header:], b"".join(payloads))
                    self.assertEqual(data[state.l4 + 13], flags[0] | flags[1] | flags[2])
                    self.assertEqual(struct.unpack_from("!I", data, state.l4 + 4)[0], 0xfffffff0)
                    self.assertEqual(repair(data), data)
            first = wire(ip, 6, b"a" * 20)
            state, output, status = self.start(first)
            self.assertEqual(status, 0)
            before = output.raw
            # PSH on an out-of-sequence or bad-checksum candidate must not leak into the SCU.
            self.assertNotEqual(self.append(state, wire(ip, 6, b"b", 9, 0x18)), 0)
            self.assertEqual(output.raw, before)
            bad = bytearray(wire(ip, 6, b"b", 4, 0x18)); bad[-1] ^= 1
            self.assertNotEqual(self.append(state, bytes(bad)), 0)
            self.assertEqual(output.raw, before)

    def test_zero_checksum_udp_and_zero_payload_stay_individual(self):
        for ip in (4, 6):
            data = bytearray(wire(ip, 17, b"x")); at = (20 if ip == 4 else 40) + 6
            data[at:at + 2] = b"\0\0"
            self.assertEqual(self.start(bytes(data))[2], 2)
            self.assertEqual(self.start(wire(ip, 17, b""))[2], 2)

    def test_capacity_segment_count_and_finalize_limits(self):
        source = wire(4, 6, b"x")
        state, output, status = self.start(source, len(source))
        self.assertEqual(status, 0)
        self.assertEqual(self.append(state, wire(4, 6, b"y", 0xfffffff1)), 3)
        self.assertEqual(output.raw[:len(source)], source)
        state, output, _ = self.start(source)
        for i in range(1, 32): self.assertEqual(self.append(state, wire(4, 6, b"x", (0xfffffff0 + i) & 0xffffffff)), 0)
        self.assertEqual(self.append(state, wire(4, 6, b"x", 16)), 3)
        self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
        self.assertNotEqual(self.append(state, source), 0)
        self.assertNotEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
        state, output, status = self.start(source, len(source) - 1)
        self.assertEqual(status, 3)
        self.assertEqual(output.raw, b"\xa5" * len(output))

    def test_truncated_packets_are_rejected_without_writing(self):
        for ip in (4, 6):
            for protocol in (6, 17):
                source = wire(ip, protocol, b"payload")
                for size in range(len(source)):
                    _, output, status = self.start(source[:size])
                    self.assertNotEqual(status, 0)
                    self.assertEqual(output.raw, b"\xa5" * len(output))


if __name__ == "__main__": unittest.main(verbosity=2)
