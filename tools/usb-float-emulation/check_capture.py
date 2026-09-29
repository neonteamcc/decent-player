#!/usr/bin/env python3
"""Independent byte oracle for synthetic QEMU capture; requires guest PASS too."""
import collections, pathlib, struct, sys
capture=pathlib.Path(sys.argv[1]).read_bytes()
log=pathlib.Path(sys.argv[2]).read_text()
assert 'USB_FLOAT_GUEST_PASS' in log and 'HARNESS_EXIT=0' in log, 'guest did not pass'
packets=collections.defaultdict(list)
pos=0
while pos<len(capture):
    rate,alt,n=struct.unpack_from('<III',capture,pos);pos+=12
    assert n<=104 and n%8==0 and pos+n<=len(capture)
    packets[alt,rate].append(capture[pos:pos+n]);pos+=n
pattern=[-1.,-.5,-.125,0.,.125,.5,1.,1.25,-1.25,-0.0]
for alt in (1,2):
    for rate in (44100,48000,96000):
        parts=packets.pop((alt,rate));wire=b''.join(parts)
        frames=rate//5+37
        samples=[pattern[i] if i<10 else ((i*37)%65521-32760)/32768 for i in range(frames*2)]
        if alt==2: expected=struct.pack('<%df'%len(samples),*samples)
        else: expected=struct.pack('<%di'%len(samples),*[max(-2147483648,min(2147483647,int(x*2147483648))) for x in samples])
        assert wire[:len(expected)]==expected, f'byte mismatch alt={alt} rate={rate}'
        assert all(x==0 for x in wire[len(expected):]), 'EOS padding not silent'
        assert len(wire)-len(expected)<104, 'excess EOS padding'
        sizes=collections.Counter(len(p)//8 for p in parts)
        assert set(sizes)<=({5,6} if rate==44100 else {rate//8000}), (rate,sizes)
        assert abs(len(wire)//8-len(parts)*rate/8000)<3, 'packet rate drift'
        print(f'PASS alt={alt} rate={rate} frames={frames} bytes={len(expected)} padding={len(wire)-len(expected)} packets={len(parts)} geometry={dict(sizes)}')
assert not packets, 'unexpected stream'
print('CAPTURE_PASS: production bytes reached QEMU via Linux usbfs and xHCI')
