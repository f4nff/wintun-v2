"""Packet-level oracle tests against the same C engine linked into the prototype SYS."""
import ctypes as C
import ipaddress
import random
import struct
import sys
import unittest


class Plan(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("source", "payload", "count", "mss")] + [
        (name, C.c_uint16) for name in ("header", "l4", "l4_header", "pseudo_src", "pseudo_dst")
    ] + [(name, C.c_uint8) for name in ("ip", "protocol", "kind", "udp_zero")]


DLL = C.CDLL(sys.argv.pop(1))
DLL.PbPlanSegments.argtypes = [C.c_void_p, C.c_uint32, C.c_int, C.c_uint32, C.POINTER(Plan)]
DLL.PbWriteSegmentChecked.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(Plan), C.c_uint32, C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32)]
DLL.PbFinalizeChecksums.argtypes = [C.c_void_p, C.c_uint32, C.c_int, C.c_int, C.c_int]
class Output(C.Structure):
    _fields_ = [('data', C.c_void_p), ('capacity', C.c_uint32), ('length', C.c_uint32)]
DLL.PbWriteSegmentBatchChecked.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(Plan), C.c_uint32, C.POINTER(Output), C.c_uint32, C.POINTER(C.c_uint32)]


def checksum(data):
    data += b"\0" * (len(data) % 2)
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def packet(ip, protocol, payload, *, flags=0x99, seq=0xfffffff0, ip_options=b"", tcp_options=b"", extension=b"", zero_udp=False, gso=True):
    if protocol == 6:
        tcp = struct.pack("!HHIIHHHH", 10001, 10002, seq, 42, ((5 + len(tcp_options) // 4) << 12) | flags, 32768, 0xabcd, 0) + tcp_options
        transport = tcp + payload
    else:
        transport = struct.pack("!HHHH", 10001, 10002, 8 + len(payload), 0 if zero_udp else 0xabcd) + payload
    if ip == 4:
        header = struct.pack("!BBHHHBBH4s4s", 0x45 + len(ip_options) // 4, 0, 0 if gso else 20 + len(ip_options) + len(transport), 0x7ffe, 0x4000, 64, protocol, 0, b"\x0a\0\0\1", b"\x0a\0\0\2") + ip_options
    else:
        header = struct.pack("!IHBB16s16s", 0x60000000, 0 if gso else len(extension) + len(transport), 0 if extension else protocol, 64, ipaddress.IPv6Address("2001:db8::1").packed, ipaddress.IPv6Address("2001:db8::2").packed) + extension
    return header + transport


class SegmentationTests(unittest.TestCase):
    def test_real_batch_boundaries_and_partial_output(self):
        for ip in (4, 6):
            for protocol, kind in ((6, 1), (17, 2)):
                original = packet(ip, protocol, bytes(range(256)) * 120)
                source, plan, status = self.plan(original, kind, 600)
                self.assertEqual(status, 0)
                _, expected = self.segments(original, kind, 600)
                buffers = [C.create_string_buffer(1500) for _ in range(32)]
                outputs = (Output * 32)(*(Output(C.addressof(b), len(b), 0) for b in buffers))
                count = C.c_uint32()
                actual = []
                for first in (0, 32):
                    self.assertEqual(DLL.PbWriteSegmentBatchChecked(source, len(original), C.byref(plan), first, outputs, 32, C.byref(count)), 0)
                    self.assertEqual(count.value, min(32, plan.count - first))
                    actual += [buffers[i].raw[:outputs[i].length] for i in range(count.value)]
                self.assertEqual(actual, expected)
                outputs[2].capacity = 1
                self.assertEqual(DLL.PbWriteSegmentBatchChecked(source, len(original), C.byref(plan), 0, outputs, 32, C.byref(count)), 3)
                self.assertEqual(count.value, 2)
                count.value = 0xdeadbeef
                self.assertEqual(DLL.PbWriteSegmentBatchChecked(source, len(original), C.byref(plan), 0, outputs, 33, C.byref(count)), 1)
                self.assertEqual(count.value, 0xdeadbeef)

    def plan(self, source, kind, mss):
        source = C.create_string_buffer(bytes(source))
        plan = Plan()
        status = DLL.PbPlanSegments(source, len(source) - 1, kind, mss, C.byref(plan))
        return source, plan, status

    def segments(self, original, kind, mss):
        source, plan, status = self.plan(original, kind, mss)
        self.assertEqual(status, 0)
        result = []
        for index in range(plan.count):
            output = C.create_string_buffer(65536)
            written = C.c_uint32()
            self.assertEqual(DLL.PbWriteSegmentChecked(source, len(original), C.byref(plan), index, output, len(output), C.byref(written)), 0)
            result.append(output.raw[:written.value])
        return plan, result

    def validate(self, data, plan):
        offset, protocol = plan.l4, plan.protocol
        if plan.ip == 4:
            self.assertEqual(struct.unpack_from("!H", data, 2)[0], len(data))
            self.assertEqual(checksum(data[:offset]), 0)
            pseudo = data[12:20] + struct.pack("!BBH", 0, protocol, len(data) - offset)
        else:
            self.assertEqual(struct.unpack_from("!H", data, 4)[0], len(data) - 40)
            pseudo = data[8:40] + struct.pack("!I3xB", len(data) - offset, protocol)
        if protocol == 17:
            self.assertEqual(struct.unpack_from("!H", data, offset + 4)[0], len(data) - offset)
            if plan.udp_zero:
                self.assertEqual(data[offset + 6:offset + 8], b"\0\0")
                return
        self.assertEqual(checksum(pseudo + data[offset:]), 0)

    def test_tcp_and_udp_segmentation_oracle(self):
        rng = random.Random(0x101)
        for ip in (4, 6):
            for protocol, kind in ((6, 1), (17, 2)):
                for size in (1, 64, 1199, 1200, 1201, 32768, 65400):
                    for mss in (1, 63, 1200, 1440):
                        if mss == 1 and size > 1201:
                            continue
                        with self.subTest(ip=ip, protocol=protocol, size=size, mss=mss):
                            payload = rng.randbytes(size)
                            original = packet(ip, protocol, payload)
                            plan, output = self.segments(original, kind, mss)
                            self.assertEqual(plan.count, (size + mss - 1) // mss)
                            self.assertEqual(b"".join(p[plan.header:] for p in output), payload)
                            for index, item in enumerate(output):
                                self.validate(item, plan)
                                self.assertEqual(len(item) - plan.header, min(mss, size - index * mss))
                                if protocol == 6:
                                    self.assertEqual(struct.unpack_from("!I", item, plan.l4 + 4)[0], (0xfffffff0 + index * mss) & 0xffffffff)
                                    flags = 0x10 | (0x09 if index + 1 == len(output) else 0) | (0x80 if index == 0 else 0)
                                    self.assertEqual(item[plan.l4 + 13], flags)
                                if ip == 4:
                                    self.assertEqual(struct.unpack_from("!H", item, 4)[0], (0x7ffe + index) & (0x7fff if protocol == 6 else 0xffff))

    def test_ipv4_and_tcp_options_are_replicated(self):
        original = packet(4, 6, b"a" * 2500, ip_options=b"\x01" * 40, tcp_options=b"\x01" * 40)
        plan, output = self.segments(original, 1, 1200)
        self.assertEqual(plan.header, 120)
        for item in output:
            self.assertEqual(item[20:60], b"\x01" * 40)
            self.assertEqual(item[80:120], b"\x01" * 40)
            self.validate(item, plan)

    def test_ipv6_extension_header_checksum(self):
        original = packet(6, 6, b"b" * 2501, extension=b"\x06\x00" + b"\0" * 6)
        plan, output = self.segments(original, 1, 1200)
        self.assertEqual(plan.l4, 48)
        for item in output:
            self.assertEqual(item[40:48], original[40:48])
            self.validate(item, plan)

    def test_udp_zero_checksum_contract(self):
        plan, output = self.segments(packet(4, 17, b"z" * 2001, zero_udp=True), 2, 1200)
        for item in output:
            self.validate(item, plan)

    def test_ipv4_source_route_uses_final_destination(self):
        final = ipaddress.IPv4Address("203.0.113.5").packed
        raw = packet(4, 6, b"route" * 501, ip_options=b"\x83\x07\x04" + final + b"\x00")
        plan, output = self.segments(raw, 1, 1200)
        self.assertEqual(plan.pseudo_dst, 23)
        for item in output:
            self.assertEqual(checksum(item[:28]), 0)
            pseudo = item[12:16] + final + struct.pack("!BBH", 0, 6, len(item) - 28)
            self.assertEqual(checksum(pseudo + item[28:]), 0)

    def test_ipv6_routing_header_uses_final_destination(self):
        final = ipaddress.IPv6Address("2001:db8:ff::9").packed
        extension = b"\x06\x02\x02\x01" + b"\0" * 4 + final
        raw = bytearray(packet(6, 6, b"routing6" * 401, extension=extension))
        raw[6] = 43
        plan, output = self.segments(raw, 1, 1200)
        self.assertEqual(plan.pseudo_dst, 48)
        self.assertEqual(plan.l4, 64)
        for item in output:
            pseudo = item[8:24] + final + struct.pack("!I3xB", len(item) - 64, 6)
            self.assertEqual(checksum(pseudo + item[64:]), 0)

    def test_ipv6_home_address_uses_pseudo_source(self):
        home = ipaddress.IPv6Address("2001:db8:aa::7").packed
        extension = b"\x06\x02\xc9\x10" + home + b"\0" * 4
        raw = bytearray(packet(6, 6, b"home6" * 501, extension=extension))
        raw[6] = 60
        plan, output = self.segments(raw, 1, 1200)
        self.assertEqual(plan.pseudo_src, 44)
        for item in output:
            pseudo = home + item[24:40] + struct.pack("!I3xB", len(item) - 64, 6)
            self.assertEqual(checksum(pseudo + item[64:]), 0)

    def test_ipv4_header_only_checksum_for_icmp_fragment(self):
        raw = bytearray(packet(4, 17, b"fragment", gso=False))
        raw[9] = 1
        raw[6:8] = b"\x20\x00"
        data = C.create_string_buffer(bytes(raw))
        self.assertEqual(DLL.PbFinalizeChecksums(data, len(raw), 1, 0, 0), 0)
        self.assertEqual(checksum(data.raw[:20]), 0)

    def test_regular_checksum_offload(self):
        for ip in (4, 6):
            for protocol in (6, 17):
                raw = packet(ip, protocol, b"regular-checksum" * 13, flags=0x10, gso=False)
                source, plan, status = self.plan(raw, 0, 0)
                self.assertEqual(status, 0)
                self.assertEqual(DLL.PbFinalizeChecksums(source, len(raw), int(ip == 4), int(protocol == 6), int(protocol == 17)), 0)
                plan.udp_zero = 0
                self.validate(source.raw[:-1], plan)

    def test_invalid_and_truncated_headers(self):
        valid = packet(4, 6, b"test")
        for length in range(40):
            self.assertNotEqual(self.plan(valid[:length], 1, 1200)[2], 0)
        for mss in (0, 65536, 0xffffffff):
            self.assertNotEqual(self.plan(valid, 1, mss)[2], 0)
        fragmented = bytearray(valid)
        fragmented[6:8] = b"\x20\x00"
        self.assertNotEqual(self.plan(fragmented, 1, 1200)[2], 0)
        self.assertNotEqual(self.plan(packet(4, 6, b"test", flags=0x02), 1, 1200)[2], 0)
        self.assertNotEqual(self.plan(packet(6, 17, b"test", extension=b"\x11\0" + b"\0" * 6), 2, 1200)[2], 0)
        self.assertNotEqual(self.plan(b"x" * 65537, 1, 1200)[2], 0)
        malformed_option = packet(4, 6, b"test", ip_options=b"\x83\x00\x00\x00")
        self.assertNotEqual(self.plan(malformed_option, 1, 1200)[2], 0)

    def test_capacity_and_index_do_not_write_output(self):
        source, plan, status = self.plan(packet(4, 6, b"test" * 500), 1, 1200)
        self.assertEqual(status, 0)
        output = C.create_string_buffer(b"\xa5" * 64)
        written = C.c_uint32(0xdeadbeef)
        self.assertEqual(DLL.PbWriteSegmentChecked(source, len(source) - 1, C.byref(plan), 0, output, 64, C.byref(written)), 3)
        self.assertEqual(output.raw[:64], b"\xa5" * 64)
        self.assertEqual(written.value, 0)
        written.value = 0xdeadbeef
        self.assertNotEqual(DLL.PbWriteSegmentChecked(source, len(source) - 1, C.byref(plan), plan.count, output, 64, C.byref(written)), 0)
        self.assertEqual(output.raw[:64], b"\xa5" * 64)
        self.assertEqual(written.value, 0)

    def assert_plan_rejected(self, source, length, plan):
        buffers = [C.create_string_buffer(b"\xa5" * 128) for _ in range(2)]
        written = C.c_uint32(0xdeadbeef)
        self.assertEqual(DLL.PbWriteSegmentChecked(source, length, C.byref(plan), 0, buffers[0], 128, C.byref(written)), 1)
        self.assertEqual(written.value, 0)
        outputs = (Output * 2)(*(Output(C.addressof(b), 128, 0xdeadbeef) for b in buffers))
        written.value = 0xdeadbeef
        self.assertEqual(DLL.PbWriteSegmentBatchChecked(source, length, C.byref(plan), 0, outputs, 2, C.byref(written)), 1)
        self.assertEqual(written.value, 0)
        for b, item in zip(buffers, outputs):
            self.assertEqual(b.raw[:128], b"\xa5" * 128)
            self.assertEqual(item.length, 0xdeadbeef)

    def test_plan_must_match_source_headers_and_span(self):
        original = packet(4, 6, b"a" * 24)
        source, plan, status = self.plan(original, 1, 12)
        self.assertEqual(status, 0)
        changes = [dict(source=76, payload=36, count=3), dict(pseudo_src=0), dict(pseudo_dst=12),
                   dict(l4=24, header=44, payload=20, mss=10),
                   dict(ip=6, l4=40, header=60, payload=4, mss=2, pseudo_src=8, pseudo_dst=24),
                   dict(protocol=17, kind=2, l4_header=8, header=28, payload=36, count=3)]
        for change in changes:
            with self.subTest(change=change):
                bad = Plan.from_buffer_copy(plan)
                for name, value in change.items():
                    setattr(bad, name, value)
                self.assert_plan_rejected(source, len(original), bad)

    def test_stale_plans_do_not_override_packet_parser(self):
        original = packet(4, 6, b"a" * 24)
        _, plan, status = self.plan(original, 1, 12)
        self.assertEqual(status, 0)
        for offset, value in ((0, 0x46), (9, 17), (32, 0x60), (33, 2), (6, 0x20)):
            with self.subTest(offset=offset):
                changed = bytearray(original)
                changed[offset] = value
                self.assert_plan_rejected(C.create_string_buffer(bytes(changed)), len(changed), plan)
        for ip in (4, 6):
            original = packet(ip, 17, b"a" * 24, zero_udp=True)
            source, plan, status = self.plan(original, 2, 12)
            self.assertEqual(status, 0)
            plan.udp_zero = 0
            self.assert_plan_rejected(source, len(original), plan)

    def test_bounded_parser_random_inputs(self):
        rng = random.Random(101)
        for _ in range(5000):
            raw = rng.randbytes(rng.randrange(0, 512))
            status = self.plan(raw, rng.randrange(3), rng.randrange(0, 1501))[2]
            self.assertIn(status, (0, 1, 2))


if __name__ == "__main__":
    unittest.main(verbosity=2)
