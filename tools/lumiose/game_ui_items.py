"""Extract item pictures from the player's game into their local cache only."""
import ctypes
import mmap
import os
from pathlib import Path
import re
import struct
import sys
import zlib
from game_ui_font import FlatBuffer, archive_from_image

PACK = b'arc/uitexicon_itemitem_0000item_0000.bntx.trpak'


def unpack_entries(pack, library):
    fb = FlatBuffer(pack)
    root = fb.u64(8)
    root_table = fb.table(root + fb.u32(root))
    count, hashes = fb.vector(root_table[0])
    n, offsets = fb.vector(root_table[1])
    if count != n: raise ValueError('Inconsistent game archive index')
    # Use the game's ONEPACK hash seed, which differs from standard FNV-1a.
    wanted = 0xcbf29ce484222645
    for b in PACK: wanted = ((wanted ^ b) * 0x100000001b3) & 0xffffffffffffffff
    for i in range(n):
        if fb.u64(hashes + 8*i) != wanted: continue
        off = fb.u64(offsets + 8*i)
        table = fb.table(off + fb.u32(off))
        entries_count, entries = fb.vector(table[1])
        for j in range(entries_count):
            p = entries + 4*j
            entry = fb.table(p + fb.u32(p))
            comp = pack[entry[1]] if len(entry) > 1 and entry[1] else 0xff
            size = fb.u64(entry[3]) if len(entry) > 3 and entry[3] else 0
            length, pos = fb.vector(entry[4])
            if not 0 < size <= 1048576 or length > 1048576: continue
            raw = bytes(pack[pos:pos+length])
            if comp == 0xff or size == length: yield raw; continue
            out = ctypes.create_string_buffer(size + 64)
            if library.ooz_decompress(raw, length, out, size) == size: yield out.raw[:size]
        return
    raise ValueError('Game item textures not found')


def textures(d):
    if len(d) < 0x30 or d[:4] != b'BNTX': return
    count = struct.unpack_from('<I', d, 0x24)[0]
    offsets = struct.unpack_from('<Q', d, 0x28)[0]
    if count > 64: raise ValueError('Invalid texture count')
    for i in range(count):
        t = struct.unpack_from('<Q', d, offsets + 8*i)[0]
        fmt = struct.unpack_from('<I', d, t+0x1c)[0]
        w, h = struct.unpack_from('<II', d, t+0x24)
        bh = struct.unpack_from('<I', d, t+0x34)[0]
        size = struct.unpack_from('<I', d, t+0x50)[0]
        no = struct.unpack_from('<Q', d, t+0x60)[0]
        start = struct.unpack_from('<Q', d, struct.unpack_from('<Q', d, t+0x70)[0])[0]
        name = d[no+2:no+2+struct.unpack_from('<H', d, no)[0]].decode('latin1')
        if re.fullmatch(r'item_\d{4}', name) and (fmt >> 8) == 0x20 and 0 < w <= 512 and 0 < h <= 512 and bh <= 5:
            yield name, w, h, bh, d[start:start+size]


def deswizzle(src, wb, hb, bh):
    gob_h, row = 8 << bh, wb * 16
    result = bytearray(row*hb)
    for y in range(hb):
        by, byo = divmod(y, gob_h); gy, gyo = divmod(byo, 8)
        for x in range(row):
            gx, gxo = divmod(x, 64)
            off = (by*((row+63)//64)+gx)*gob_h*64 + gy*512 + (gxo//32)*256 + (gyo//2)*64 + ((gxo%32)//16)*32 + (gyo%2)*16 + gxo%16
            if off < len(src): result[y*row+x] = src[off]
    return bytes(result)


def png_bgra(w, h, raw):
    # texture2ddecoder returns BGRA; PNG requires RGBA.
    scan = bytearray()
    for y in range(h):
        scan.append(0)
        row = raw[y*w*4:(y+1)*w*4]
        for x in range(0, len(row), 4): scan.extend((row[x+2], row[x+1], row[x], row[x+3]))
    def chunk(kind, data): return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(scan,6))+chunk(b'IEND',b'')


def prepare(install, game, dest):
    folder = game/'romfs'/'arc'/'data.trpfs'
    source = folder if folder.is_file() else game/'romfs.bin'
    libdir = install/'zaprompts'/'_internal'
    library = libdir/('ooz.dll' if os.name == 'nt' else 'libooz.so')
    if not source.is_file() or not library.is_file(): return False
    stamp = source.stat()
    marker = dest/'.items-made'
    fingerprint = f'{stamp.st_size}:{stamp.st_mtime_ns}:1'
    if marker.is_file() and marker.read_text() == fingerprint and (dest/'item_0004.png').is_file(): return True
    sys.path.insert(0, str(libdir))
    try: from texture2ddecoder import _texture2ddecoder as decoder
    finally: sys.path.pop(0)
    ooz = ctypes.CDLL(str(library))
    ooz.ooz_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    with source.open('rb') as f: mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    view = memoryview(mm)
    pack = view if source.name == 'data.trpfs' else archive_from_image(view)
    dest.mkdir(parents=True, exist_ok=True)
    made = 0
    for data in unpack_entries(pack, ooz):
        for name, w, h, bh, tex in textures(data):
            wb, hb = (w+3)//4, (h+3)//4
            if w % 4 or h % 4: continue
            bgra = decoder.decode_bc7(deswizzle(tex, wb, hb, bh), w, h)
            (dest/(name+'.png')).write_bytes(png_bgra(w,h,bgra)); made += 1
    if not made: raise ValueError('No game item pictures found')
    marker.write_text(fingerprint)
    return True
