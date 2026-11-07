"""32 valid wire inputs per case; IPv4 and IPv6 run in separate processes.

Python builds complete wire/checksum oracles without a native plan or finalizer.
Direct common-coalescer calls are counted; this is not Submit/Commit, internal
memcpy profiling, a CPU result, NDIS indication, or old-system execution.
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 3 or sys.argv[2] not in ("4", "6"):
    raise SystemExit("usage: test_coalesce_edge_family32.py <packet-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(2))
# Import data/functions only; no old mixed-family TestCase is discovered here.
from test_packet import DLL, checksum
from test_coalesce import State

COUNT, SIZE, SEQUENCE = 32, 63, 0xfffffff0
ACK, WINDOW, FIRST_ID, TRAFFIC, FLOW = 0x80000001, 65535, 0xfffe, 0x2e, 0x34567
ADDRESS = ("10.0.0.1", "10.0.0.2") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESS)
IP_HEADER = 20 if IP == 4 else 40
MARKERS = (0, 0xffffffffffffffff, 0x0123456789abcdef, 0xfedcba9876543210)
SENTINEL, SOURCE_SENTINEL = 0xa5, 0x6d


def pseudo(protocol, length):
    if IP == 4:
        return SOURCE + DESTINATION + struct.pack("!BBH", 0, protocol, length)
    return SOURCE + DESTINATION + struct.pack("!I3xB", length, protocol)


def wire(protocol, data, *, sequence=SEQUENCE, flags=0x10, hop=64, ident=FIRST_ID, uro=False):
    """Independent fixed-header wire encoder; URO uses documented OOB form."""
    if protocol == 6:
        transport = bytearray(struct.pack("!HHIIHHHH", 10001, 10002, sequence, ACK,
                                          0x5000 | flags, WINDOW, 0, 0) + data)
        checksum_offset = 16
    else:
        transport = bytearray(struct.pack("!HHHH", 10001, 10002, 8 + len(data), 0) + data)
        checksum_offset = 6
    value = checksum(pseudo(protocol, len(transport)) + transport)
    struct.pack_into("!H", transport, checksum_offset, 0 if uro else value or (65535 if protocol == 17 else 0))
    if IP == 4:
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, TRAFFIC, 20 + len(transport),
                                        ident, 0x4000, hop, protocol, 0, SOURCE, DESTINATION))
        struct.pack_into("!H", header, 10, 0 if uro else checksum(header))
    else:
        header = struct.pack("!IHBB16s16s", 0x60000000 | (TRAFFIC << 20) | FLOW,
                             len(transport), protocol, hop, SOURCE, DESTINATION)
    return bytes(header + transport)


def payload(index, length, protocol):
    if length == 1:
        return bytes([0x80 | index])
    # Opaque application markers, not a send-time API, TCP Timestamp, or epoch.
    data = bytearray(struct.pack("!IQQ", index, MARKERS[index % 4], MARKERS[(index + 1) % 4]))
    data += bytes([0x30 + index]) * (length - len(data))
    if protocol == 17 and index == 17:
        # A legitimate nonzero wire checksum FFFF can represent computed zero.
        # Keep this correction outside the sequence/opaque marker bytes.
        data[40:42] = b"\0\0"
        udp = struct.pack("!HHHH", 10001, 10002, 8 + length, 0)
        struct.pack_into("!H", data, 40, checksum(pseudo(17, 8 + length) + udp + data))
        assert checksum(pseudo(17, 8 + length) + udp + data) == 0
    return bytes(data)


class CoalesceEdgeFamily32Tests(unittest.TestCase):
    def scenario(self, protocol, shape):
        short = shape == "short1"
        boundary = shape in ("capacity31", "hop-boundary")
        header = IP_HEADER + (20 if protocol == 6 else 8)
        capacity = header + 31 * SIZE if shape == "capacity31" else 65535
        parts = [payload(index, 1 if short and index == 31 else SIZE, protocol) for index in range(COUNT)]
        # Rejected TCP32 is the only PSH/min-hop candidate: premature mutation
        # would visibly contaminate the complete31-packet SCU snapshot below.
        flags = [0x10 | (8 if (index == 31 if boundary else index in (7, 17, 31)) else 0)
                 for index in range(COUNT)]
        hops = [(1 if index == (31 if boundary else 17) else 64) if protocol == 6
                else (63 if shape == "hop-boundary" and index == 31 else 64) for index in range(COUNT)]
        packets = [wire(protocol, part, sequence=(SEQUENCE + sum(map(len, parts[:index]))) & 0xffffffff,
                        flags=flags[index], hop=hops[index], ident=(FIRST_ID + index) & 0xffff)
                   for index, part in enumerate(parts)]
        for packet in packets:
            if IP == 4:
                self.assertEqual(checksum(packet[:20]), 0)
            self.assertEqual(checksum(pseudo(protocol, len(packet) - IP_HEADER) + packet[IP_HEADER:]), 0)
            if protocol == 17:
                self.assertNotEqual(packet[IP_HEADER + 6:IP_HEADER + 8], b"\0\0")
        if protocol == 17:
            self.assertEqual(packets[17][IP_HEADER + 6:IP_HEADER + 8], b"\xff\xff")
        sources = [C.create_string_buffer(len(packet) + 32) for packet in packets]
        originals = [packet + bytes([SOURCE_SENTINEL]) * 32 for packet in packets]
        for source, original in zip(sources, originals):
            source.raw = original
        calls = []

        def unchanged_sources():
            self.assertEqual([source.raw for source in sources], originals)

        def begin(index, limit):
            state, output = State(), C.create_string_buffer(limit + 64)
            output.raw = bytes([SENTINEL]) * len(output)
            calls.append(("begin", index))
            self.assertEqual(DLL.PbCoalesceBegin(C.byref(state), output, limit, sources[index], len(packets[index])), 0)
            unchanged_sources()
            return state, output

        def pending(state, output, accepted):
            joined = b"".join(parts[index] for index in accepted)
            self.assertEqual((state.capacity, state.length, state.count, state.mss, state.payload,
                              state.header, state.l4, state.ip, state.protocol, state.short, state.final),
                             (capacity, header + len(joined), len(accepted), len(parts[accepted[0]]), len(joined),
                              header, IP_HEADER, IP, protocol, int(protocol == 17 and short and 31 in accepted), 0))
            self.assertEqual(state.next_sequence, ((SEQUENCE + len(joined)) & 0xffffffff) if protocol == 6 else 0)
            self.assertEqual(output.raw[header:state.length], joined)
            self.assertEqual(output.raw[state.length:], bytes([SENTINEL]) * (len(output) - state.length))

        def finish(state, output, accepted):
            calls.append(("finish", len(accepted)))
            self.assertEqual(DLL.PbCoalesceFinish(C.byref(state)), 0)
            unchanged_sources()
            first = accepted[0]
            joined = b"".join(parts[index] for index in accepted)
            aggregate_flags = 0x10 | (8 if any(flags[index] & 8 for index in accepted) else 0)
            expected = wire(protocol, joined, sequence=(SEQUENCE + sum(map(len, parts[:first]))) & 0xffffffff,
                            flags=aggregate_flags, hop=min(hops[index] for index in accepted),
                            ident=(FIRST_ID + first) & 0xffff, uro=protocol == 17 and len(accepted) > 1)
            self.assertEqual((state.count, state.length, state.final), (len(accepted), len(expected), 1))
            self.assertEqual(output.raw[:state.length], expected)
            self.assertEqual(output.raw[state.length:], bytes([SENTINEL]) * (len(output) - state.length))
            return joined

        state, output = begin(0, capacity)
        accepted = [0]
        pending(state, output, accepted)
        for index in range(1, COUNT):
            state_before = bytes(state)
            output_before = output.raw
            if protocol == 6 and boundary and index == 31:
                self.assertEqual(output_before[IP_HEADER + 13], 0x10)
                self.assertEqual(output_before[8 if IP == 4 else 7], 64)
                self.assertEqual((packets[index][IP_HEADER + 13], hops[index]), (0x18, 1))
            calls.append(("append", index))
            status = DLL.PbCoalesceAppend(C.byref(state), sources[index], len(packets[index]))
            unchanged_sources()
            if boundary and index == 31:
                self.assertEqual(status, 3 if shape == "capacity31" else 2)
                self.assertEqual(bytes(state), state_before)
                self.assertEqual(output.raw, output_before)
            else:
                self.assertEqual(status, 0)
                accepted.append(index)
                pending(state, output, accepted)
        self.assertEqual(accepted, list(range(31 if boundary else 32)))
        copied = finish(state, output, accepted)
        if boundary:
            # Continue with only the rejected last original; no prefix replay.
            tail_state, tail_output = begin(31, 65535)
            copied += finish(tail_state, tail_output, [31])
        self.assertEqual(copied, b"".join(parts))
        expected_calls = [("begin", 0)] + [("append", index) for index in range(1, 32)]
        expected_calls += [("finish", 31 if boundary else 32)]
        if boundary:
            expected_calls += [("begin", 31), ("finish", 1)]
        self.assertEqual(calls, expected_calls)
        unchanged_sources()
        print(f"COALESCE_EDGE32 ip={IP} protocol={protocol} shape={shape} inputs=32 "
              f"prefix={31 if boundary else 32} units={2 if boundary else 1} "
              f"api_calls={len(calls)} source_state_output_sentinels=preserved", flush=True)

    def test_tcp_32_odd_payload_sequence_wrap_and_rejected_last_psh(self):
        # Microsoft RSC requires PSH OR, minimum TTL/hop and the first IPv4 ID:
        # https://learn.microsoft.com/en-us/windows-hardware/drivers/network/rules-for-coalescing-tcp-ip-packets
        # https://learn.microsoft.com/en-us/windows-hardware/drivers/network/updating-the-ip-headers-for-coalesced-segments
        for shape in ("full", "capacity31"):
            with self.subTest(ip=IP, shape=shape):
                self.scenario(6, shape)

    def test_udp_32_odd_datagrams_short_final_capacity_and_hop_boundary(self):
        # URO permits only a shorter final datagram, requires identical hop
        # limits, and indicates checksum-success with zero header checksums.
        # These SCUs are NDIS representations, not ordinary IPv6 zero-CS wire.
        # https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload
        for shape in ("full", "short1", "capacity31", "hop-boundary"):
            with self.subTest(ip=IP, shape=shape):
                self.scenario(17, shape)


if __name__ == "__main__":
    print(f"FAMILY: IPv{IP}", flush=True)
    unittest.main(verbosity=2)
