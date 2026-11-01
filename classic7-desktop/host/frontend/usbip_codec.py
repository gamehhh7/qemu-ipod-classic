"""USB/IP wire framing only. No listener, driver attach or fabricated USB data.

Based on https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html .
Device identity is accepted only from real guest descriptor bytes; a live
native transport and packet completion ledger are still required separately.
"""
from dataclasses import dataclass
import struct

VERSION=0x0111
MAX_TRANSFER=1024*1024
COMMON=struct.Struct('>HHI')
BASIC=struct.Struct('>IIIII')
SUBMIT=struct.Struct('>Ii iii 8s'.replace(' ',''))

class ProtocolError(ValueError):pass

def c_string(value,size):
    encoded=value.encode('ascii')
    if not encoded or b'\0' in encoded or len(encoded)>=size:raise ProtocolError('wire string')
    return encoded+bytes(size-len(encoded))

def decode_string(raw):
    at=raw.find(b'\0')
    if at<=0 or any(raw[at:]):raise ProtocolError('non-terminated/padded string')
    try:return raw[:at].decode('ascii')
    except UnicodeError as e:raise ProtocolError('non-ASCII busid') from e

def operation(raw):
    if len(raw)!=8:raise ProtocolError('operation header length')
    version,op,status=COMMON.unpack(raw)
    if version!=VERSION or status or op not in (0x8003,0x8005):raise ProtocolError('operation header')
    return op

@dataclass(frozen=True)
class GuestExport:
    path:str
    busid:str
    busnum:int
    devnum:int
    speed:int
    device:bytes
    configuration:bytes
    configuration_value:int|None=None

    def encode(self,interfaces):
        d=self.device;c=self.configuration
        if len(d)!=18 or d[:2]!=b'\x12\x01' or d[7] not in (8,16,32,64):raise ProtocolError('actual device descriptor invalid')
        if len(c)<9 or c[:2]!=b'\x09\x02' or int.from_bytes(c[2:4],'little')!=len(c) or not c[5]:raise ProtocolError('actual configuration invalid')
        if not 1<=self.busnum<=65535 or not 1<=self.devnum<=65535 or self.speed not in (1,2,3):raise ProtocolError('controller bus/speed')
        found=[];offset=0;seen=set()
        while offset<len(c):
            n=c[offset]
            if n<2 or offset+n>len(c):raise ProtocolError('configuration descriptor walk')
            row=c[offset:offset+n]
            if row[1]==4:
                if n<9:raise ProtocolError('short interface')
                if row[3]==0:
                    if row[2] in seen:raise ProtocolError('duplicate interface')
                    seen.add(row[2]);found.append(bytes(row[5:8])+b'\0')
            offset+=n
        if len(found)!=c[4] or not found:raise ProtocolError('actual interface count')
        selected=c[5] if self.configuration_value is None else self.configuration_value
        if type(selected) is not int or selected not in (0,c[5]):raise ProtocolError('actual current configuration not represented by descriptor')
        active=found if selected else []
        # Numbers and USB class bytes are read from the actual guest payload.
        wire=c_string(self.path,256)+c_string(self.busid,32)
        wire+=struct.pack('>IIIHHH6B',self.busnum,self.devnum,self.speed,
            int.from_bytes(d[8:10],'little'),int.from_bytes(d[10:12],'little'),int.from_bytes(d[12:14],'little'),
            d[4],d[5],d[6],selected,d[17],len(active))
        assert len(wire)==312
        return wire+b''.join(active) if interfaces else wire

def devlist(export):
    if export is None:return COMMON.pack(VERSION,5,0)+struct.pack('>I',0)
    return COMMON.pack(VERSION,5,0)+struct.pack('>I',1)+export.encode(True)

def imported(export):
    return COMMON.pack(VERSION,3,int(export is None))+(export.encode(False) if export else b'')

@dataclass(frozen=True)
class Urb:
    seq:int
    devid:int
    direction:int
    ep:int
    flags:int
    length:int
    setup:bytes

def submit(raw,expected_devid):
    if len(raw)!=48:raise ProtocolError('submit header length')
    cmd,seq,devid,direction,ep=BASIC.unpack_from(raw)
    flags,length,start,packets,interval,setup=SUBMIT.unpack_from(raw,20)
    if cmd!=1 or not seq or devid!=expected_devid or direction not in (0,1) or ep>15:raise ProtocolError('submit identity')
    if not 0<=length<=MAX_TRANSFER:raise ProtocolError('transfer allocation bound')
    # Documentation uses-1; actual Linux usbip_pack_cmd_submit forwards the
    # non-ISO URB's zero count too. Neither form has ISO descriptors appended.
    if start or packets not in (-1,0):raise ProtocolError('isochronous transfer unsupported')
    if interval<0:raise ProtocolError('negative interval')
    if ep==0:
        requested=int.from_bytes(setup[6:8],'little')
        if length!=requested or (length and direction!=(setup[0]>>7)):raise ProtocolError('control direction/length')
    elif setup!=bytes(8):raise ProtocolError('unexpected bulk setup')
    return Urb(seq,devid,direction,ep,flags,length,setup)

def unlink(raw,expected_devid):
    if len(raw)!=48:raise ProtocolError('unlink header length')
    cmd,seq,devid,direction,ep=BASIC.unpack_from(raw)
    target=struct.unpack_from('>I',raw,20)[0]
    # The Windows UDE client leaves former SUBMIT union fields in the unused
    # 24-byte tail. They are not part of UNLINK semantics. Ignore that tail;
    # retain all actual command, device, endpoint and sequence checks.
    if cmd!=2 or not seq or not target or seq==target or devid!=expected_devid or direction not in (0,1) or ep:raise ProtocolError('unlink identity')
    return seq,target

def completed(urb,status,actual,payload=b''):
    """Serialize a native backend's result; never infer successful completion.
    Payload can only contain bytes returned by the actual guest transport.
    """
    if type(status) is not int or not -4095<=status<=0:raise ProtocolError('Linux completion status')
    if not 0<=actual<=urb.length:raise ProtocolError('actual length outside URB')
    if len(payload)!=(actual if urb.direction else 0):raise ProtocolError('completion payload length')
    return BASIC.pack(3,urb.seq,0,0,0)+struct.pack('>iiiii8s',status,actual,0,-1,0,bytes(8))+payload

def unlinked(seq,cancelled):
    return BASIC.pack(4,seq,0,0,0)+struct.pack('>i',-104 if cancelled else 0)+bytes(24)
