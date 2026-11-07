"""Address-only TUN classification: independent IPv4/IPv6 processes, no Open.

The API shim supplies only its public V2 table and real common/flow.c selector;
no session, adapter or test API is invoked. Ethernet parsing uses the real
packet engine. Fixed FNV vectors test current address-only queue policy, not a
cross-version load-balancing guarantee or an OS/CPU/performance result.
"""
import ctypes as C
import ipaddress
import struct
import sys
import unittest

if len(sys.argv) < 4 or sys.argv[3] not in ("4", "6"):
    raise SystemExit("usage: test_flow_family32.py <packet-dll> <api-shim-dll> <4|6> [unittest arguments]")
IP = int(sys.argv.pop(3))
API_PATH = sys.argv.pop(2)
# Data/functions only: importing no mixed-family TestCase.
from test_packet import DLL, checksum


class Interface(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("size", "major", "minor", "version")] + [
        ("source", C.c_char * 65), ("reserved", C.c_uint8 * 7), ("functions", C.c_void_p * 24)]


# Keep both loader objects live while their borrowed function pointers exist.
API = C.WinDLL(API_PATH)
API.PbGetInterfaceV2.argtypes = [C.c_uint32, C.c_uint32, C.POINTER(Interface)]
API.PbGetInterfaceV2.restype = C.c_int
TABLE = Interface()
if API.PbGetInterfaceV2(2, C.sizeof(TABLE), C.byref(TABLE)) != 0:
    raise RuntimeError("Cannot obtain the public V2 function table")
if (TABLE.size, TABLE.major, TABLE.minor, TABLE.version) != (C.sizeof(TABLE), 2, 1, 0x02000000):
    raise RuntimeError("Unexpected V2 ABI or project version")
SELECT_QUEUE = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint32, C.c_uint32)(TABLE.functions[15])
DLL.PbEthernetInspect.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32)]
DLL.PbEthernetInspect.restype = C.c_int

ADDRESSES = ("10.0.0.1", "192.0.2.1") if IP == 4 else ("2001:db8::1", "2001:db8::2")
SOURCE, DESTINATION = (ipaddress.ip_address(value).packed for value in ADDRESSES)
LOCAL, PEER = bytes.fromhex("025001020304"), bytes.fromhex("02d001020304")
ADDRESS_HASH = 0x693c2411 if IP == 4 else 0xe0af4c2c
QUEUE_VECTOR = (0, 1, 2, 1, 3, 5, 1, 1) if IP == 4 else (0, 0, 0, 0, 4, 0, 5, 4)
COUNT, PIECE, SENTINEL = 32, 128, 0x6d


def fnv_reference(data):
    # RFC 9923: independent integer arithmetic and parameters, fixed below
    # against its published32-bit string vectors before any selector call.
    # https://www.rfc-editor.org/rfc/rfc9923.html
    value = 0x811c9dc5
    for octet in data:
        value = ((value ^ octet) * 0x01000193) % (1 << 32)
    return value


def pseudo(protocol, length):
    if IP == 4:
        return SOURCE + DESTINATION + struct.pack("!BBH", 0, protocol, length)
    return SOURCE + DESTINATION + struct.pack("!I3xB", length, protocol)


def transport(protocol, payload, index=0):
    if protocol == 6:
        segment = bytearray(struct.pack("!HHIIHHHH", 10001 + index, 20002 - index,
                                        (0xfffffff0 + index * 63) & 0xffffffff, 42,
                                        0x5018, 32768, 0, 0) + payload)
        at = 16
    else:
        segment = bytearray(struct.pack("!HHHH", 10001 + index, 20002 - index, 8 + len(payload), 0) + payload)
        at = 6
    value = checksum(pseudo(protocol, len(segment)) + segment)
    struct.pack_into("!H", segment, at, value or (65535 if protocol == 17 else 0))
    return bytes(segment)


def ip_packet(protocol, body, index=0, *, fragmented=False, offset=0, more=False):
    if IP == 4:
        options = b"" if fragmented else b"\x01" * (4 * (index % 11))
        size = 20 + len(options)
        flags = (0x2000 if more else 0) | (offset // 8) if fragmented else 0x4000
        header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45 + len(options) // 4,
                                        0 if fragmented else index * 7, size + len(body),
                                        0x7000 + protocol if fragmented else 0xffe0 + index,
                                        flags, 64 if fragmented else 64 - index, protocol, 0,
                                        SOURCE, DESTINATION) + options)
        struct.pack_into("!H", header, 10, checksum(header))
        return bytes(header) + body
    extension = struct.pack("!BBHI", protocol, 0, offset | int(more), 0x89abcdef + protocol) if fragmented else (
        bytes([protocol, 0]) + bytes(6) if index % 2 else b"")
    next_header = 44 if fragmented else 0 if extension else protocol
    flow = 0 if fragmented else ((index * 7) << 20) | (0x34560 + index)
    return struct.pack("!IHBB16s16s", 0x60000000 | flow, len(extension) + len(body),
                       next_header, 64 if fragmented else 64 - index, SOURCE, DESTINATION) + extension + body


class FlowFamily32Tests(unittest.TestCase):
    def setUp(self):
        # The literal expected address/vector values are not obtained from C.
        self.assertEqual([fnv_reference(value) for value in (b"", b"a", b"foobar")],
                         [0x811c9dc5, 0xe40c292c, 0xbf9cf968])
        self.assertEqual(fnv_reference(SOURCE + DESTINATION), ADDRESS_HASH)
        self.assertEqual(tuple(ADDRESS_HASH % queues for queues in range(1, 9)), QUEUE_VECTOR)
        self.assertEqual(bytes(TABLE.reserved), bytes(7))
        self.assertEqual(len(TABLE.source), 64)
        self.assertTrue(all(value in b"0123456789abcdef" for value in TABLE.source))

    def classify32(self, packets, protocol, shape):
        self.assertEqual(len(packets), COUNT)
        inspections = selections = 0
        for index, packet in enumerate(packets):
            tagged = bool(index & 1)
            kind = 0x0800 if IP == 4 else 0x86dd
            l2 = PEER + LOCAL + (struct.pack("!HHH", 0x8100, ((index % 8) << 13) | 100, kind)
                                  if tagged else struct.pack("!H", kind))
            frame = l2 + packet
            source = C.create_string_buffer(len(frame) + 64)
            original = frame + bytes([SENTINEL]) * 64
            source.raw = original
            offset = C.c_uint32(0xdeadbeef)
            self.assertEqual(DLL.PbEthernetInspect(source, len(frame), C.byref(offset)), 1)
            inspections += 1
            self.assertEqual(offset.value, 18 if tagged else 14)
            self.assertEqual(source.raw, original)
            pointer = C.c_void_p(C.addressof(source) + offset.value)
            prefix_length = 59 - offset.value
            self.assertGreaterEqual(prefix_length, 40 if IP == 6 else 20)
            for queues, expected in enumerate(QUEUE_VECTOR, 1):
                full = SELECT_QUEUE(pointer, len(packet), queues)
                self.assertEqual(source.raw, original)
                prefix = SELECT_QUEUE(pointer, prefix_length, queues)
                self.assertEqual(source.raw, original)
                self.assertEqual((full, prefix), (expected, expected))
                self.assertLess(full, queues)
                selections += 2
        self.assertEqual((inspections, selections), (32, 512))
        print(f"FLOW32 ip={IP} protocol={protocol} shape={shape} packets=32 "
              f"inspect32 select512 prefix59 tags14and18 queues1to8 source_sentinels=preserved", flush=True)

    def test_32_first_and_nonfirst_fragments_share_queue_for_every_valid_count(self):
        # Legal non-overlapping32-way IP fragmentation, first fragment contains
        # the entire L4 header. Both protocols have a valid reassembled checksum.
        # https://www.rfc-editor.org/rfc/rfc791.html (section3.1)
        # https://www.rfc-editor.org/rfc/rfc8200.html (section4.5)
        for protocol in (6, 17):
            with self.subTest(ip=IP, protocol=protocol):
                l4_size = 20 if protocol == 6 else 8
                payload = bytearray(b"\x35" * (COUNT * PIECE - l4_size))
                # Deliberately different continuation bytes resemble arbitrary
                # ports/sequence data, but are only opaque fragment payload.
                for index in range(COUNT):
                    struct.pack_into("!IQQ", payload, index * PIECE + 64 - l4_size,
                                     index, 0xffffffffffffffff - index, 0x0123456789abcdef + index)
                original = transport(protocol, payload)
                self.assertEqual(len(original), COUNT * PIECE)
                self.assertEqual(checksum(pseudo(protocol, len(original)) + original), 0)
                packets = [ip_packet(protocol, original[index * PIECE:(index + 1) * PIECE],
                                     fragmented=True, offset=index * PIECE, more=index < COUNT - 1)
                           for index in range(COUNT)]
                reassembled = []
                for index, packet in enumerate(packets):
                    if IP == 4:
                        self.assertEqual(checksum(packet[:20]), 0)
                        self.assertEqual(struct.unpack_from("!H", packet, 2)[0], len(packet))
                        flags = struct.unpack_from("!H", packet, 6)[0]
                        self.assertEqual((flags & 0x1fff) * 8, index * PIECE)
                        self.assertEqual(bool(flags & 0x2000), index < COUNT - 1)
                        self.assertFalse(flags & 0xc000)
                        reassembled.append(packet[20:])
                    else:
                        self.assertEqual((packet[6], struct.unpack_from("!H", packet, 4)[0]), (44, len(packet) - 40))
                        next_header, reserved, position, ident = struct.unpack_from("!BBHI", packet, 40)
                        self.assertEqual((next_header, reserved, position & 0xfff8, position & 7, ident),
                                         (protocol, 0, index * PIECE, int(index < COUNT - 1), 0x89abcdef + protocol))
                        reassembled.append(packet[48:])
                self.assertEqual(b"".join(reassembled), original)
                self.classify32(packets, protocol, "fragment-set")

    def test_32_ordinary_options_ports_and_nonaddress_fields_do_not_reclassify(self):
        for protocol in (6, 17):
            with self.subTest(ip=IP, protocol=protocol):
                packets = []
                for index in range(COUNT):
                    # The 64-bit values are opaque application markers, not
                    # packet send time, TCP Timestamp options, or a session epoch.
                    payload = struct.pack("!IQQ", index, index, 0xffffffffffffffff - index) + bytes([index]) * 43
                    segment = transport(protocol, payload, index)
                    self.assertEqual(checksum(pseudo(protocol, len(segment)) + segment), 0)
                    packet = ip_packet(protocol, segment, index)
                    if IP == 4:
                        self.assertEqual(checksum(packet[:(packet[0] & 15) * 4]), 0)
                    packets.append(packet)
                self.classify32(packets, protocol, "ordinary-options")


if __name__ == "__main__":
    print(f"FAMILY: IPv{IP}", flush=True)
    unittest.main(verbosity=2)
