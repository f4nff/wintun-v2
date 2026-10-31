"""Ethernet boundary oracle: tags, ARP, NDP, checksums, and DAD suppression."""
import ctypes as C
import ipaddress
import random
import struct
import sys
import unittest

DLL = C.CDLL(sys.argv.pop(1))
DLL.PbEthernetInspect.argtypes = [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32)]
DLL.PbEthernetControl.argtypes = [C.c_void_p, C.c_uint32, C.c_void_p, C.c_void_p, C.POINTER(C.c_uint32)]
DLL.PbEthernetHeader.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p, C.c_uint32]
LOCAL, PEER = bytes.fromhex('025001020304'), bytes.fromhex('02d001020304')

def checksum(data):
    data += b'\0' * (len(data) % 2)
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16: s = (s & 65535) + (s >> 16)
    return ~s & 65535

def frame(kind, payload, tag=False):
    return b'\xff' * 6 + LOCAL + (struct.pack('!HHH', 0x8100, 123, kind) if tag else struct.pack('!H', kind)) + payload

def ns(source='fe80::1', target='2001:db8::2', options=b''):
    src, dst = ipaddress.IPv6Address(source).packed, ipaddress.IPv6Address('ff02::1:ff00:2').packed
    data = bytearray(b'\x87\0\0\0' + b'\0' * 4 + ipaddress.IPv6Address(target).packed + options)
    struct.pack_into('!H', data, 2, checksum(src + dst + struct.pack('!I3xB', len(data), 58) + data))
    return struct.pack('!IHBB', 0x60000000, len(data), 58, 255) + src + dst + data

class EthernetTests(unittest.TestCase):
    def reply(self, data):
        out, n = C.create_string_buffer(128), C.c_uint32()
        kind = DLL.PbEthernetControl(data, len(data), PEER, out, C.byref(n))
        return kind, out.raw[:n.value]

    def test_ip_and_single_8021q_offsets(self):
        for kind, ip in ((0x0800, b'\x45' + bytes(19)), (0x86dd, b'\x60' + bytes(39))):
            for tagged in (False, True):
                data, offset = frame(kind, ip, tagged), C.c_uint32()
                self.assertEqual(DLL.PbEthernetInspect(data, len(data), C.byref(offset)), 1)
                self.assertEqual(offset.value, 18 if tagged else 14)
        data = frame(0x8100, bytes(40), True)
        self.assertEqual(DLL.PbEthernetInspect(data, len(data), C.byref(C.c_uint32())), 0)

    def test_arp_response_addresses_and_dad(self):
        src, dst = bytes([10, 0, 0, 1]), bytes([10, 0, 0, 2])
        def arp(s, d): return struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + LOCAL + s + bytes(6) + d
        for tagged in (False, True):
            kind, reply = self.reply(frame(0x0806, arp(src, dst), tagged))
            self.assertEqual((kind, len(reply)), (2, 60))
            self.assertEqual(reply[:14], LOCAL + PEER + b'\x08\x06')
            self.assertEqual(reply[20:42], b'\0\x02' + PEER + dst + LOCAL + src)
        for s, d in ((bytes(4), dst), (src, src), (src, bytes([224, 0, 0, 1]))):
            self.assertEqual(self.reply(frame(0x0806, arp(s, d))), (0, b''))

    def test_ndp_reply_oracle_and_dad(self):
        source, target = ipaddress.IPv6Address('fe80::1').packed, ipaddress.IPv6Address('2001:db8::2').packed
        for tagged in (False, True):
            kind, reply = self.reply(frame(0x86dd, ns(options=b'\x01\x01' + LOCAL), tagged))
            self.assertEqual((kind, len(reply)), (2, 86))
            ip, icmp = reply[14:54], reply[54:]
            self.assertEqual(ip[8:40], target + source)
            self.assertEqual(icmp[:2], b'\x88\0'); self.assertEqual(icmp[4], 0x60)
            self.assertEqual(icmp[24:], b'\x02\x01' + PEER)
            self.assertEqual(checksum(ip[8:40] + struct.pack('!I3xB', len(icmp), 58) + icmp), 0)
        self.assertEqual(self.reply(frame(0x86dd, ns('::'))), (0, b''))
        self.assertEqual(self.reply(frame(0x86dd, ns('2001:db8::2'))), (0, b''))

    def test_ndp_validation_and_truncation(self):
        for data in (ns(options=b'\x01\0' + LOCAL), ns(options=b'\x01\x01' + PEER)):
            self.assertEqual(self.reply(frame(0x86dd, data)), (0, b''))
        good = bytearray(ns())
        for index in (7, 40, 42, 50):
            bad = good.copy(); bad[index] ^= 1
            self.assertEqual(self.reply(frame(0x86dd, bytes(bad))), (0, b''))
        data = frame(0x86dd, ns())
        for n in range(len(data)):
            self.assertEqual(self.reply(data[:n]), (0, b''))

    def test_injection_mac_mapping(self):
        for address, mac in (('10.0.0.1', LOCAL), ('224.128.1.2', bytes.fromhex('01005e000102')), ('255.255.255.255', b'\xff'*6)):
            ip = b'\x45' + bytes(15) + ipaddress.IPv4Address(address).packed
            out = C.create_string_buffer(14); DLL.PbEthernetHeader(out, LOCAL, PEER, ip, len(ip))
            self.assertEqual(out.raw, mac + PEER + b'\x08\0')
        ip = b'\x60' + bytes(23) + ipaddress.IPv6Address('ff02::1').packed
        out = C.create_string_buffer(14); DLL.PbEthernetHeader(out, LOCAL, PEER, ip, len(ip))
        self.assertEqual(out.raw, bytes.fromhex('333300000001') + PEER + b'\x86\xdd')

    def test_bounded_random_inputs(self):
        rng = random.Random(0x101)
        for _ in range(5000):
            data = rng.randbytes(rng.randrange(129)); kind, reply = self.reply(data)
            self.assertIn(kind, (0, 2)); self.assertLessEqual(len(reply), 128)

if __name__ == '__main__': unittest.main()
