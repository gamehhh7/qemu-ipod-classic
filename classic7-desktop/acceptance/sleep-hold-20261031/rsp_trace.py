import socket,struct,time,json,sys
from pathlib import Path
s=socket.create_connection(('127.0.0.1',15870),3);s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);s.settimeout(45)
def recv():
 while True:
  ch=s.recv(1)
  if not ch:raise EOFError('debugger target closed')
  if ch==b'$':break
 data=b''
 while True:
  b=s.recv(1)
  if not b:raise EOFError('debugger target closed')
  if b==b'#':break
  data+=b
 s.recv(2);s.sendall(b'+');return data.decode()
def send(x):
 b=x.encode();s.sendall(b'$'+b+b'#'+f'{sum(b)&255:02x}'.encode());return recv()
def reg():
 b=bytes.fromhex(send('g'));return [int.from_bytes(b[i*4:i*4+4],'little') for i in range(16)]
points=[int(a,16) for a in sys.argv[2:]]
try:
 print('STATUS',send('?'),flush=True)
 for a in points: print('BREAK',hex(a),send(f'Z1,{a:x},4'),flush=True)
 if points:
  for n in range(20):
   try:reply=send('c')
   except socket.timeout:
    s.sendall(b'\x03');reply=recv(); print('TIMEOUT',flush=True);break
   r=reg();print(json.dumps({'stop':reply,'regs':[f'{x:08x}' for x in r]}),flush=True)
   a=r[15]
   if a in points:
    send(f'z1,{a:x},4');send('s');send(f'Z1,{a:x},4')
 else:print('REGS',reg(),flush=True)
finally:
 for a in points:
  try:send(f'z1,{a:x},4')
  except Exception:pass
 try:print('DETACH',send('D'),flush=True)
 except Exception:pass
 s.close()

