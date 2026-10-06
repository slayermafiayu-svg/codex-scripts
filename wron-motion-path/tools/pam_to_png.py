#!/usr/bin/env python3
"""Converts a P7 RGB_ALPHA PAM file to PNG using only the standard library."""
import struct
import sys
import zlib


def main(src, dst):
    data = open(src, 'rb').read()
    header_end = data.index(b'ENDHDR\n') + len(b'ENDHDR\n')
    fields = dict(line.split(' ', 1) for line in data[:header_end].decode().splitlines()[1:-1])
    w, h = int(fields['WIDTH']), int(fields['HEIGHT'])
    pixels = data[header_end:]
    raw = b''.join(b'\x00' + pixels[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
