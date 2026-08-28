import struct
d = open('app.ico', 'rb').read()
n = struct.unpack('<HHH', d[:6])[2]
print('entries:', n)
for i in range(n):
    w, h, _, _, _, bc, sz, off = struct.unpack('<BBBBHHII', d[6+16*i:6+16*(i+1)])
    kind = 'PNG' if d[off:off+4] == b'\x89PNG' else 'BMP'
    print(f'  {w or 256}x{h or 256} {kind} {sz}B @ {off}')
# BMP entry 0: bottom-left pixel = border blue, near-center = flake white
w, h, _, _, _, _, sz, off = struct.unpack('<BBBBHHII', d[6:6+16])
xors = off + 40
def px(x, y):                                    # top-down coords
    o = xors + ((15 - y) * 16 + x) * 4
    return list(d[o:o+4])
print('corner (0,0) BGRA:', px(0, 0), '(expect transparent 0,0,0,0)')
print('edge (0,8)  BGRA:', px(0, 8), '(expect opaque blue border)')
print('center (8,8) BGRA:', px(8, 8), '(expect white flake)')
