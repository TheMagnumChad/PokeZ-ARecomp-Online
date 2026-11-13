"""Locate the UI font in the player's own game data; never distribute game assets."""
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import tempfile

KEY = bytes((0x49, 0x62, 0x18, 0x06))
FONT_NAME = b'FOT-UDKakugoC80'
FONT_PACK = b'arc/uifontbinFOT-UDKakugoC80Pro-DB.bfotf.trpak'
MAX_FONT = 0x600000


class FlatBuffer:
    def __init__(self, data): self.data = data
    def u32(self, p): return struct.unpack_from('<I', self.data, p)[0]
    def u64(self, p): return struct.unpack_from('<Q', self.data, p)[0]
    def table(self, p):
        vt = p - struct.unpack_from('<i', self.data, p)[0]
        n = (struct.unpack_from('<H', self.data, vt)[0] - 4) // 2
        return [(p + off) if (off := struct.unpack_from('<H', self.data, vt + 4 + 2*i)[0]) else None for i in range(n)]
    def vector(self, p):
        v = p + self.u32(p)
        return self.u32(v), v + 4


def font_from_bfttf(raw):
    if len(raw) < 8 or raw[:4] != bytes.fromhex('36f81a1e'): return None
    n = int.from_bytes(bytes(a ^ b for a, b in zip(raw[4:8], KEY)), 'big')
    if n < 1040 or n > MAX_FONT or n > len(raw) - 8: return None
    font = bytes(raw[8+i] ^ KEY[i % 4] for i in range(n))
    if font[:4] not in (b'OTTO', b'\0\1\0\0') or FONT_NAME not in font.replace(b'\0', b''):
        return None
    return font


def files(pack):
    if bytes(pack[:8]) != b'ONEPACK\0': raise ValueError('Unknown game archive')
    fb = FlatBuffer(pack)
    root = fb.u64(8)
    root_table = fb.table(root + fb.u32(root))
    hash_count, hashes = fb.vector(root_table[0])
    n, offsets = fb.vector(root_table[1])
    if n != hash_count: raise ValueError('Game archive index is inconsistent')
    wanted = 0xcbf29ce484222645
    for b in FONT_PACK:
        wanted = ((wanted ^ b) * 0x100000001b3) & 0xffffffffffffffff
    for i in range(n):
        if fb.u64(hashes + 8*i) != wanted: continue
        off = fb.u64(offsets + 8*i)
        table = fb.table(off + fb.u32(off))
        count, entries = fb.vector(table[1])
        for j in range(count):
            p = entries + 4*j
            entry = fb.table(p + fb.u32(p))
            comp = pack[entry[1]] if len(entry) > 1 and entry[1] else 0xff
            size = fb.u64(entry[3]) if len(entry) > 3 and entry[3] else 0
            nbytes, data = fb.vector(entry[4])
            if 1040 <= size < MAX_FONT and nbytes <= MAX_FONT:
                yield comp, size, pack[data:data+nbytes]


def archive_from_image(image):
    h = struct.unpack_from('<10Q', image, 0)
    dirs, entries, data = image[h[3]:h[3]+h[4]], image[h[7]:h[7]+h[8]], h[9]
    pos = 0
    while pos + 32 <= len(entries):
        parent, _, off, size, _, n = struct.unpack_from('<IIQQII', entries, pos)
        name = bytes(entries[pos+32:pos+32+n])
        pos += 32 + ((n+3) & ~3)
        if name != b'data.trpfs': continue
        parent_name_len = struct.unpack_from('<I', dirs, parent+20)[0]
        parent_name = bytes(dirs[parent+24:parent+24+parent_name_len])
        if parent_name == b'arc': return image[data+off:data+off+size]
    raise ValueError('Game UI archive not found')


def extract_font(source, library):
    with source.open('rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    view = memoryview(mm)
    pack = view if source.name == 'data.trpfs' else archive_from_image(view)
    ooz = ctypes.CDLL(str(library))
    ooz.ooz_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    for comp, size, raw in files(pack):
        if comp == 0xff or size == len(raw):
            data = bytes(raw)
        else:
            out = ctypes.create_string_buffer(size + 64)
            if ooz.ooz_decompress(bytes(raw), len(raw), out, size) != size: continue
            data = out.raw[:size]
        font = font_from_bfttf(data)
        if font: return font
    raise ValueError('Game UI font not found')


def prepare(install, game, cache):
    folder = game / 'romfs' / 'arc' / 'data.trpfs'
    source = folder if folder.is_file() else game / 'romfs.bin'
    if not source.is_file(): return None
    libs = [install / 'zaprompts' / '_internal' / n for n in ('ooz.dll', 'libooz.so', 'libooz.dylib')]
    libs += [install / 'zaprompts' / n for n in ('ooz.dll', 'libooz.so', 'libooz.dylib')]
    library = next((p for p in libs if p.is_file()), None)
    if library is None: return None
    stat = source.stat()
    fingerprint = {'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns, 'version': 2}
    dest, meta = cache / 'game-ui-font.otf', cache / 'game-ui-font.json'
    try:
        saved = json.loads(meta.read_text())
        if dest.is_file() and saved.get('source') == fingerprint:
            cached = dest.read_bytes()
            if (1040 <= len(cached) <= MAX_FONT and cached[:4] in (b'OTTO', b'\0\1\0\0') and
                    hashlib.sha256(cached).hexdigest() == saved.get('sha256')):
                return dest
    except (OSError, ValueError): pass
    font = extract_font(source, library)
    cache.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix='game-ui-font-', dir=cache)
    try:
        with os.fdopen(fd, 'wb') as out: out.write(font)
        os.replace(temp, dest)
    finally:
        if os.path.exists(temp): os.unlink(temp)
    meta.write_text(json.dumps({'source': fingerprint, 'sha256': hashlib.sha256(font).hexdigest()}), encoding='utf-8')
    return dest
