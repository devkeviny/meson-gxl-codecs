#!/usr/bin/env python3
"""
Fetch the H.264 encoder firmware the Amlogic S905W (GXL) amvenc_avc driver
needs, from the public repository that hosts it, check it against the exact
file the driver was tested with, and install it under /lib/firmware.

Nothing is redistributed by this project: the firmware comes from CoreELEC's
media_modules-aml at a pinned commit. Terms: Amlogic ucode, see
LICENSE.amlogic_vdec in linux-firmware (use with Amlogic chips, not as part
of the kernel).

The driver requests "meson/venc/gxl_h264_enc.bin" via request_firmware().
The file is extracted from the h264_enc.bin PACK (which also carries the
G12A and TXL ucodes) and verified by md5.

    sudo python3 get-firmware.py              # install into /lib/firmware
    python3 get-firmware.py --dest DIR        # or anywhere else
"""
import argparse
import hashlib
import os
import struct
import sys
import urllib.request

COREELEC = ('https://raw.githubusercontent.com/CoreELEC/media_modules-aml/'
            '201519678d35a9d7bf4dde1f3a328aa9d533589b/firmware/')

# installed path, md5, source URL, ucode name inside the PACK
FILES = [
    ('meson/venc/gxl_h264_enc.bin', '548df08288d7cf48d4bb1255bca45ca7',
     COREELEC + 'h264_enc.bin', 'gxl_h264_enc.bin'),
]


def pack_entries(buf):
    """name -> ucode of an Amlogic 'PACK' firmware (video_ucode.bin etc.)"""
    if buf[256:260] == b'KCAP':         # 256-byte signature in front
        buf = buf[256:]
    if buf[:4] != b'KCAP':
        raise ValueError('not an Amlogic PACK firmware')
    out, off = {}, 256                  # package header
    while off + 256 <= len(buf):
        info = buf[off:off + 256]       # package_info: name, format, cpu, len
        length = struct.unpack_from('<i', info, 96)[0]
        if not length:
            break
        fw = buf[off + 256:off + 256 + length]  # 512-byte head + data
        size = struct.unpack_from('<i', fw, 200)[0]
        out[info[:32].split(b'\0')[0].decode()] = fw[512:512 + size]
        off += 256 + length
    return out


def md5(data):
    return hashlib.md5(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--dest', default='/lib/firmware')
    dest = ap.parse_args().dest

    cache, failed = {}, 0
    for path, want, url, entry in FILES:
        target = os.path.join(dest, path)
        if os.path.exists(target):
            have = md5(open(target, 'rb').read())
            if have == want:
                print(f'kept       {path}')
                continue
        if url not in cache:
            print(f'download   {url}')
            with urllib.request.urlopen(url, timeout=60) as r:
                cache[url] = r.read()
        data = pack_entries(cache[url])[entry]
        if md5(data) != want:
            print(f'MISMATCH   {path}: got {md5(data)}, want {want}',
                  file=sys.stderr)
            failed += 1
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target + '.tmp', 'wb') as f:
            f.write(data)
        os.chmod(target + '.tmp', 0o644)
        os.replace(target + '.tmp', target)
        print(f'installed  {path} ({len(data)} bytes)')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
