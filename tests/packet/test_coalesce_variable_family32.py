"""32 valid, variable-length TCP inputs per isolated IPv4/IPv6 process.

NDIS RSC admits sequence-contiguous data with different payload lengths; this
is distinct from URO's equal-sized datagrams plus optional short last datagram.
TCP options remain absent because the current reference coalescer deliberately
keeps them individual. Application markers below are not TCP timestamps,
scheduling clocks or session epochs. This tests common coalescer bytes/state,
not Submit/Commit, internal memcpy counts, CPU or real NDIS execution.
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/rules-for-coalescing-tcp-ip-packets
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/updating-the-ip-headers-for-coalesced-segments
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_coalesce_variable_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# Import helper types/functions only, not historical mixed-family TestCases.
from test_packet import DLL, checksum
from test_coalesce import State

COUNT, CAPACITY, INITIAL_SEQUENCE = 32, 65535, 0xffffff00
ACK, WINDOW, FIRST_ID, TRAFFIC, FLOW = 0x80000001, 65535, 0x7fff, 0x2e, 0x34567
IP_HEADER, HEADER = (20, 40) if IP == 4 else (40, 60)
ADDRESSES = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)
PSH_INDICES, MIN_HOP_INDEX = (4, 7, 19, 25), 13
SOURCE_SENTINEL, OUTPUT_SENTINEL, STATE_SENTINEL = 0x6d, 0xa5, 0xd3
MARKERS = (0, 0xffffffffffffffff, 0x0123456789abcdef, 0xfedcba9876543210)


class StateWithSentinel(C.Structure):
    _fields_ = [("state", State), ("sentinel", C.c_ubyte * 32)]


def pseudo(length):
    suffix = struct.pack("!BBH", 0, 6, length) if IP == 4 else struct.pack("!I3xB", length, 6)
    return SOURCE + DESTINATION + suffix


def wire(payload, sequence, flags, hop, ident):
    tcp = bytearray(struct.pack("!HHIIHHHH", 10001, 10002, sequence, ACK,
                                0x5000 | flags, WINDOW, 0, 0) + payload)
    struct.pack_into("!H", tcp, 16, checksum(pseudo(len(tcp)) + tcp))
    if IP == 4:
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, TRAFFIC,
            20 + len(tcp), ident, 0x4000, hop, 6, 0, SOURCE, DESTINATION))
        struct.pack_into("!H", header, 10, checksum(header))
    else:
        header = struct.pack("!IHBB16s16s", 0x60000000 | (TRAFFIC << 20) | FLOW,
                             len(tcp), 6, hop, SOURCE, DESTINATION)
    return bytes(header + tcp)


def payload(index, length):
    prefix = struct.pack("!IQQ", index, MARKERS[index % 4], MARKERS[(index + 1) % 4])
    return prefix + bytes([index + 0x30]) * (length - len(prefix))


class CoalesceVariableFamily32Tests(unittest.TestCase):
    def test_32_variable_tcp_payloads_preserve_psh_sequence_and_middle_minimum_hop(self):
        # Every packet changes size, and even/odd byte lengths alternate.
        sizes = [(31 + (index * 17) % 101) * 2 + (index & 1) for index in range(COUNT)]
        self.assertEqual([size % 2 for size in sizes], [index % 2 for index in range(COUNT)])
        self.assertEqual(len(set(sizes)), COUNT)
        parts = [payload(index, sizes[index]) for index in range(COUNT)]
        hops = [64 - index % 9 for index in range(COUNT)]
        hops[MIN_HOP_INDEX] = 1
        self.assertEqual(hops.index(min(hops)), MIN_HOP_INDEX)
        self.assertGreater(hops[0], 1)
        self.assertGreater(hops[-1], 1)
        self.assertNotIn(0, PSH_INDICES)
        self.assertNotIn(COUNT - 1, PSH_INDICES)
        packets, sequence = [], INITIAL_SEQUENCE
        for index, part in enumerate(parts):
            packet = wire(part, sequence, 0x18 if index in PSH_INDICES else 0x10,
                          hops[index], (FIRST_ID + index * 17) & 65535)
            self.assertEqual(checksum(pseudo(20 + sizes[index]) + packet[IP_HEADER:]), 0)
            if IP == 4:
                self.assertEqual(checksum(packet[:IP_HEADER]), 0)
            packets.append(packet)
            sequence = (sequence + sizes[index]) & 0xffffffff
        self.assertLess(sequence, INITIAL_SEQUENCE)
        source_bytes = [packet + bytes([SOURCE_SENTINEL]) * 64 for packet in packets]
        sources = [C.create_string_buffer(data, len(data)) for data in source_bytes]
        output = C.create_string_buffer(bytes([OUTPUT_SENTINEL]) * (CAPACITY + 64), CAPACITY + 64)
        control = StateWithSentinel()
        C.memset(C.byref(control), STATE_SENTINEL, C.sizeof(control))
        state, calls = control.state, []

        def check_pending(last):
            consumed = sum(sizes[:last + 1])
            self.assertEqual((state.buffer, state.capacity, state.length, state.count,
                state.mss, state.next_sequence, state.payload, state.header, state.l4,
                state.ip, state.protocol, state.short, state.final),
                (C.addressof(output), CAPACITY, HEADER + consumed, last + 1, sizes[0],
                 (INITIAL_SEQUENCE + consumed) & 0xffffffff, consumed, HEADER,
                 IP_HEADER, IP, 6, 0, 0))
            self.assertEqual(output.raw[HEADER:HEADER + consumed], b"".join(parts[:last + 1]))
            self.assertEqual(output.raw[IP_HEADER + 13], 0x18 if any(index <= last for index in PSH_INDICES) else 0x10)
            self.assertEqual(struct.unpack_from("!I", output.raw, IP_HEADER + 4)[0], INITIAL_SEQUENCE)
            self.assertEqual(output.raw[8 if IP == 4 else 7], min(hops[:last + 1]))
            self.assertEqual(output.raw[HEADER + consumed:], bytes([OUTPUT_SENTINEL]) * (CAPACITY + 64 - HEADER - consumed))
            self.assertEqual(bytes(control.sentinel), bytes([STATE_SENTINEL]) * 32)
            self.assertEqual([source.raw for source in sources], source_bytes)

        calls.append(("begin", 0))
        self.assertEqual(DLL.PbCoalesceBegin(C.byref(state), output, CAPACITY,
                                           sources[0], len(packets[0])), 0)
        check_pending(0)
        for index in range(1, COUNT):
            with self.subTest(input=index):
                calls.append(("append", index))
                self.assertEqual(DLL.PbCoalesceAppend(C.byref(state), sources[index], len(packets[index])), 0)
                check_pending(index)
        state_before_finish = State.from_buffer_copy(bytes(state))
        state_before_finish.final = 1
        calls.append(("finish", COUNT))
        self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
        expected = wire(b"".join(parts), INITIAL_SEQUENCE, 0x18, 1, FIRST_ID)
        self.assertEqual(output.raw[:len(expected)], expected)
        self.assertEqual(output.raw[len(expected):], bytes([OUTPUT_SENTINEL]) * (CAPACITY + 64 - len(expected)))
        self.assertEqual(state.length, len(expected))
        self.assertEqual(bytes(state), bytes(state_before_finish))
        self.assertEqual(bytes(control.sentinel), bytes([STATE_SENTINEL]) * 32)
        self.assertEqual([source.raw for source in sources], source_bytes)
        self.assertEqual(calls, [("begin", 0)] + [("append", index) for index in range(1, COUNT)] + [("finish", COUNT)])
        self.assertEqual(checksum(pseudo(20 + sum(sizes)) + output.raw[IP_HEADER:len(expected)]), 0)
        if IP == 4:
            self.assertEqual(checksum(output.raw[:IP_HEADER]), 0)
        print(f"COALESCE_VARIABLE32 ip={IP} protocol=6 inputs=32 units=1 payload_bytes={sum(sizes)} "
              "psh_segments=4 minhop_index=13 seq_wrap=1 direct_calls=33 source_state_sentinels=preserved")


if __name__ == "__main__":
    unittest.main(verbosity=2)
