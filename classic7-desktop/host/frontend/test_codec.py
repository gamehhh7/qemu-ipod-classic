"""Replay/negative wire tests only; never imports a Windows device."""
import unittest,struct
from dataclasses import replace
from usbip_codec import *

DEVICE=bytes.fromhex('1201000200000040ac056112010001020302')
CONFIG=bytes.fromhex('09022000010100c0fa0904000002080650000705830200020007050202000200')
def exported():return GuestExport('/classic7/native/session1','1-1',1,1,3,DEVICE,CONFIG)
def header(seq=7,devid=65537,direction=1,ep=3,length=512,packets=-1,setup=bytes(8),flags=0):
    return BASIC.pack(1,seq,devid,direction,ep)+struct.pack('>Iiiii8s',flags,length,0,packets,0,setup)

class CodecTests(unittest.TestCase):
    def test_actual_identity_offsets(self):
        raw=exported().encode(True)
        self.assertEqual(len(raw),316)
        self.assertEqual(struct.unpack_from('>HH',raw,300),(0x05ac,0x1261))
        self.assertEqual(raw[312:],bytes([8,6,0x50,0]))
    def test_changed_guest_identity_changes_export(self):
        d=bytearray(DEVICE);d[8:12]=bytes.fromhex('34127856')
        self.assertEqual(struct.unpack_from('>HH',replace(exported(),device=bytes(d)).encode(False),300),(0x1234,0x5678))
    def test_no_device_list_and_import(self):
        self.assertEqual(devlist(None),bytes.fromhex('011100050000000000000000'))
        self.assertEqual(imported(None),bytes.fromhex('0111000300000001'))
    def test_list_has_interfaces_import_does_not(self):
        self.assertEqual(len(devlist(exported())),328);self.assertEqual(len(imported(exported())),320)
    def test_unconfigured_export_reports_zero_active_interfaces(self):
        e=replace(exported(),configuration_value=0);raw=e.encode(True)
        self.assertEqual(len(raw),312);self.assertEqual(raw[309],0);self.assertEqual(raw[311],0)
        self.assertEqual(len(devlist(e)),324)
    def test_unsupported_active_configuration_not_invented(self):
        with self.assertRaises(ProtocolError):replace(exported(),configuration_value=2).encode(False)
    def test_bad_descriptor_lengths(self):
        for d in (bytes(18),DEVICE[:17]):
            with self.assertRaises(ProtocolError):replace(exported(),device=d).encode(False)
        for c in (CONFIG[:9],CONFIG+bytes(2)):
            with self.assertRaises(ProtocolError):replace(exported(),configuration=c).encode(False)
    def test_bad_interface_walk(self):
        c=bytearray(CONFIG);c[18]=0
        with self.assertRaises(ProtocolError):replace(exported(),configuration=bytes(c)).encode(False)
    def test_strings_require_terminator_and_zero_padding(self):
        for raw in (b'a'*32,b'x\0garbage'+bytes(23),bytes(32)):
            with self.assertRaises(ProtocolError):decode_string(raw)
        self.assertEqual(decode_string(c_string('1-1',32)),'1-1')
    def test_operation_version_status_commands(self):
        self.assertEqual(operation(bytes.fromhex('0111800500000000')),0x8005)
        for raw in (bytes.fromhex('0110800500000000'),bytes.fromhex('0111800500000001'),bytes.fromhex('0111800200000000'),bytes(7)):
            with self.assertRaises(ProtocolError):operation(raw)
    def test_submit_non_iso_variants(self):
        for n in (-1,0):self.assertEqual(submit(header(packets=n),65537).length,512)
    def test_submit_identity(self):
        for kwargs in ({'seq':0},{'devid':3},{'direction':2},{'ep':16}):
            with self.assertRaises(ProtocolError):submit(header(**kwargs),65537)
    def test_reject_negative_and_large_allocations(self):
        for n in (-1,MAX_TRANSFER+1):
            with self.assertRaises(ProtocolError):submit(header(length=n),65537)
    def test_reject_iso(self):
        with self.assertRaises(ProtocolError):submit(header(packets=1),65537)
    def test_control_setup_direction_length(self):
        setup=bytes.fromhex('8006000100001200')
        self.assertEqual(submit(header(ep=0,length=18,setup=setup),65537).setup,setup)
        for kwargs in ({'length':17},{'direction':0}):
            args=dict(ep=0,length=18,setup=setup);args.update(kwargs)
            with self.assertRaises(ProtocolError):submit(header(**args),65537)
    def test_bulk_setup_rejected(self):
        with self.assertRaises(ProtocolError):submit(header(setup=bytes.fromhex('8006000100001200')),65537)
    def test_return_submit_signed_failure_and_server_zero_identity(self):
        u=submit(header(),65537);raw=completed(u,-32,3,b'abc')
        self.assertEqual(BASIC.unpack_from(raw),(3,7,0,0,0))
        self.assertEqual(struct.unpack_from('>ii',raw,20),(-32,3));self.assertEqual(raw[48:],b'abc')
    def test_return_length_mismatch_rejected(self):
        u=submit(header(),65537)
        for args in ((0,513,bytes(513)),(0,3,b'ab'),(1,0,b'')):
            with self.assertRaises(ProtocolError):completed(u,*args)
    def test_out_completion_has_actual_length_but_no_payload(self):
        u=submit(header(direction=0),65537)
        self.assertEqual(len(completed(u,0,512)),48)
        with self.assertRaises(ProtocolError):completed(u,0,512,bytes(512))
    def test_unlink_layout_and_signed_cancelled_status(self):
        raw=BASIC.pack(2,8,65537,0,0)+struct.pack('>I',7)+bytes(24)
        self.assertEqual(unlink(raw,65537),(8,7))
        self.assertEqual(struct.unpack_from('>i',unlinked(8,True),20)[0],-104)
        self.assertEqual(struct.unpack_from('>i',unlinked(8,False),20)[0],0)
    def test_unlink_padding_and_identity(self):
        raw=BASIC.pack(2,8,65537,0,0)+struct.pack('>I',7)+bytes(23)+b'\1'
        with self.assertRaises(ProtocolError):unlink(raw,65537)

if __name__=='__main__':unittest.main()
