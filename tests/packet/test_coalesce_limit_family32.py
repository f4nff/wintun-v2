"""32-input SCUs at this implementation's 65535-byte total-length bound.

The caller offers 65536 bytes; the actual coalescer clamps its private capacity
to 65535. A valid last candidate exceeding that bound by one byte must leave
the pending unit unchanged. Only the last input is then retried at the exact
bound. IPv6's general protocol limit is not redefined by this implementation's
more conservative bound. URO outputs use NDIS checksum-success OOB semantics,
including zero checksum fields, rather than pretending to be ordinary wire
packets with recomputed UDP checksums. No actual NDIS, clocks, Submit/Commit,
internal memcpy counts, CPU or handle/thread leak claim is made here.
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/indicating-coalesced-segments
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/exception-conditions-that-terminate-coalescing
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_coalesce_limit_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
from test_packet import DLL, checksum
from test_coalesce import State

COUNT, MSS, OFFERED, LIMIT, GUARD = 32, 2048, 65536, 65535, 64
IP_HEADER = 20 if IP == 4 else 40
SEQUENCE, ACK, WINDOW, FIRST_ID = 0xffffff00, 0x80000001, 65535, 0x7fff
TRAFFIC, FLOW, SENTINEL = 0x2e, 0x34567, 0xa5
ADDRESSES = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)
MARKERS = (0, 0xffffffffffffffff, 0x0123456789abcdef, 0xfedcba9876543210)


class GuardedState(C.Structure):
    _fields_ = [("before", C.c_ubyte * GUARD), ("state", State), ("after", C.c_ubyte * GUARD)]


def pseudo(protocol, length):
    suffix = (struct.pack("!BBH", 0, protocol, length) if IP == 4
              else struct.pack("!I3xB", length, protocol))
    return SOURCE + DESTINATION + suffix


def wire(protocol, payload, sequence, flags, hop, ident, *, uro=False):
    if protocol == 6:
        transport = bytearray(struct.pack("!HHIIHHHH", 10001, 10002, sequence,
            ACK, 0x5000 | flags, WINDOW, 0, 0) + payload)
    else:
        transport = bytearray(struct.pack("!HHHH", 10001, 10002, 8 + len(payload), 0) + payload)
    at = 16 if protocol == 6 else 6
    value = checksum(pseudo(protocol, len(transport)) + transport)
    struct.pack_into("!H", transport, at, 0 if uro else value or (65535 if protocol == 17 else 0))
    if IP == 4:
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, TRAFFIC,
            20 + len(transport), ident, 0x4000, hop, protocol, 0, SOURCE, DESTINATION))
        struct.pack_into("!H", header, 10, 0 if uro else checksum(header))
    else:
        header = struct.pack("!IHBB16s16s", 0x60000000 | (TRAFFIC << 20) | FLOW,
                             len(transport), protocol, hop, SOURCE, DESTINATION)
    return bytes(header + transport)


def payload(index, size):
    marker = struct.pack("!IQQ", index, MARKERS[index % 4], MARKERS[(index + 1) % 4])
    return marker + bytes([index + 0x30]) * (size - len(marker))


class CoalesceLimitFamily32Tests(unittest.TestCase):
    def scenario(self, protocol):
        header = IP_HEADER + (20 if protocol == 6 else 8)
        sizes = [MSS] * 31 + [LIMIT - header - 31 * MSS]
        self.assertTrue(20 < sizes[-1] < MSS)
        parts = [payload(index, size) for index, size in enumerate(sizes)]
        packets, sequence = [], SEQUENCE
        for index, part in enumerate(parts):
            packets.append(wire(protocol, part, sequence,
                0x18 if protocol == 6 and index == 31 else 0x10,
                1 if protocol == 6 and index == 31 else 64,
                (FIRST_ID + index * 17) & 65535))
            sequence = (sequence + len(part)) & 0xffffffff
        bad_last = wire(protocol, parts[-1] + b"\x7d", (SEQUENCE + 31 * MSS) & 0xffffffff,
                        0x18 if protocol == 6 else 0x10, 1 if protocol == 6 else 64,
                        (FIRST_ID + 31 * 17) & 65535)
        all_packets = packets + [bad_last]
        for packet in all_packets:
            self.assertEqual(checksum(pseudo(protocol, len(packet) - IP_HEADER) + packet[IP_HEADER:]), 0)
            if IP == 4:
                self.assertEqual(checksum(packet[:IP_HEADER]), 0)
        # These are fully owned, separate wire buffers. No native/Rust loan
        # changes and no source mutation during a C call are involved.
        sources_raw = [b"\x6d" * GUARD + packet + b"\x6d" * GUARD for packet in all_packets]
        sources = [C.create_string_buffer(raw, len(raw)) for raw in sources_raw]
        source_pointers = [C.addressof(source) + GUARD for source in sources]
        output = C.create_string_buffer(bytes([SENTINEL]) * (GUARD + OFFERED + GUARD), GUARD + OFFERED + GUARD)
        output_pointer = C.addressof(output) + GUARD
        control = GuardedState()
        C.memset(C.byref(control), 0xd3, C.sizeof(control))
        state, calls = control.state, []

        def unchanged_sources_and_guards():
            self.assertEqual([source.raw for source in sources], sources_raw)
            self.assertEqual(bytes(control.before), b"\xd3" * GUARD)
            self.assertEqual(bytes(control.after), b"\xd3" * GUARD)
            self.assertEqual(output.raw[:GUARD], bytes([SENTINEL]) * GUARD)
            self.assertEqual(output.raw[GUARD + state.length:], bytes([SENTINEL]) * (OFFERED + GUARD - state.length))

        def pending(last):
            consumed = sum(sizes[:last + 1])
            self.assertEqual((state.buffer, state.capacity, state.length, state.count,
                state.mss, state.next_sequence, state.payload, state.header, state.l4,
                state.ip, state.protocol, state.short, state.final),
                (output_pointer, LIMIT, header + consumed, last + 1, MSS,
                 ((SEQUENCE + consumed) & 0xffffffff) if protocol == 6 else 0,
                 consumed, header, IP_HEADER, IP, protocol, int(protocol == 17 and last == 31), 0))
            self.assertEqual(output.raw[GUARD + header:GUARD + state.length], b"".join(parts[:last + 1]))
            if protocol == 6:
                self.assertEqual(output.raw[GUARD + IP_HEADER + 13], 0x18 if last == 31 else 0x10)
            self.assertEqual(output.raw[GUARD + (8 if IP == 4 else 7)], 1 if protocol == 6 and last == 31 else 64)
            unchanged_sources_and_guards()

        calls.append(("begin", 0))
        self.assertEqual(DLL.PbCoalesceBegin(C.byref(state), output_pointer, OFFERED,
                                           source_pointers[0], len(packets[0])), 0)
        pending(0)
        for index in range(1, 31):
            calls.append(("append", index))
            self.assertEqual(DLL.PbCoalesceAppend(C.byref(state), source_pointers[index], len(packets[index])), 0)
            pending(index)
        self.assertEqual(state.count, 31)
        saved_control, saved_output = bytes(control), output.raw
        calls.append(("reject", 31))
        self.assertEqual(DLL.PbCoalesceAppend(C.byref(state), source_pointers[32], len(bad_last)), 3)
        self.assertEqual(bytes(control), saved_control)
        self.assertEqual(output.raw, saved_output)
        unchanged_sources_and_guards()
        calls.append(("append", 31))
        self.assertEqual(DLL.PbCoalesceAppend(C.byref(state), source_pointers[31], len(packets[31])), 0)
        pending(31)
        self.assertEqual((state.length, state.capacity), (LIMIT, LIMIT))
        final_state = State.from_buffer_copy(bytes(state))
        final_state.final = 1
        calls.append(("finish", 32))
        self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
        expected = wire(protocol, b"".join(parts), SEQUENCE, 0x18,
                        1 if protocol == 6 else 64, FIRST_ID, uro=protocol == 17)
        self.assertEqual(len(expected), LIMIT)
        self.assertEqual(output.raw[GUARD:GUARD + LIMIT], expected)
        self.assertEqual(bytes(state), bytes(final_state))
        unchanged_sources_and_guards()
        if protocol == 6:
            self.assertEqual(checksum(pseudo(protocol, LIMIT - IP_HEADER) + expected[IP_HEADER:]), 0)
            if IP == 4:
                self.assertEqual(checksum(expected[:IP_HEADER]), 0)
        else:
            self.assertEqual(struct.unpack_from("!H", expected, IP_HEADER + 4)[0], LIMIT - IP_HEADER)
            self.assertEqual(expected[IP_HEADER + 6:IP_HEADER + 8], bytes(2))
            if IP == 4:
                self.assertEqual(expected[10:12], bytes(2))
        self.assertEqual(calls, [("begin", 0)] + [("append", index) for index in range(1, 31)]
                         + [("reject", 31), ("append", 31), ("finish", 32)])
        print(f"COALESCE_LIMIT32 ip={IP} protocol={protocol} inputs=32 rejected_candidates=1 "
              f"offered=65536 capacity=65535 result_bytes=65535 tail_payload={sizes[-1]} "
              "prefix_replayed=0 direct_calls=34 source_state_output_sentinels=preserved")

    def test_tcp_32_exact_total_limit_and_oversized_last_candidate(self):
        self.scenario(6)

    def test_udp_32_exact_total_limit_and_oversized_last_candidate(self):
        self.scenario(17)


if __name__ == "__main__":
    unittest.main(verbosity=2)
