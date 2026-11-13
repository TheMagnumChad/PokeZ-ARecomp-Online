"""Local-only extraction of a game's Pokémon model resource pack.

Requested on demand by the launcher model service. This produces a
hash-indexed cache from the player's own game. Never distribute this cache.
"""
import ctypes
import json
import mmap
import posixpath
from pathlib import Path
import re
import struct

from game_ui_font import FlatBuffer, archive_from_image

MAX_MEMBER = 64 * 1024 * 1024
MAX_PACK = 256 * 1024 * 1024


def fnv64(path):
    h = 0xcbf29ce484222645
    for b in path.encode('ascii'):
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def model_names(model, form):
    if not (0 <= model <= 9999 and 0 <= form <= 99):
        raise ValueError('Invalid game model or form')
    base = f'pm{model:04d}_{form:02d}_00'
    folder = f'ik_pokemon/data/pm{model:04d}/{base}'
    prefix = f'arc/ik_pokemondatapm{model:04d}{base}{base}'
    return base, folder, prefix


def index_from_image(image):
    header = struct.unpack_from('<10Q', image, 0)
    dirs, entries, data = image[header[3]:header[3]+header[4]], image[header[7]:header[7]+header[8]], header[9]
    try:
        pos = 0
        while pos + 32 <= len(entries):
            parent, _, off, size, _, n = struct.unpack_from('<IIQQII', entries, pos)
            name = bytes(entries[pos+32:pos+32+n])
            pos += 32 + ((n+3) & ~3)
            if name != b'data.trpfd':
                continue
            length = struct.unpack_from('<I', dirs, parent+20)[0]
            if bytes(dirs[parent+24:parent+24+length]) == b'arc':
                return bytes(image[data+off:data+off+size])
    finally:
        dirs.release()
        entries.release()
    raise ValueError('Game resource index not found')


class MissingModelPack(ValueError):
    pass


def find_model_pack(index, prefix):
    fb = FlatBuffer(index)
    table = fb.table(fb.u32(0))
    count, entries = fb.vector(table[1])
    matches = []
    for i in range(count):
        p = entries + 4*i
        p += fb.u32(p)
        name = index[p+4:p+4+fb.u32(p)].decode('utf-8')
        if name.startswith(prefix) and name.endswith('.trpak'):
            matches.append(name)
    if not matches:
        raise MissingModelPack('Requested Pokemon model pack is not in this game')
    if len(matches) != 1:
        raise ValueError(f'Expected one Pokémon model pack; found {len(matches)}')
    return matches[0]


def read_members(pack, wanted, ooz):
    fb = FlatBuffer(pack)
    if bytes(pack[:8]) != b'ONEPACK\0':
        raise ValueError('Not a game ONEPACK archive')
    root = fb.u64(8)
    table = fb.table(root + fb.u32(root))
    count, hashes = fb.vector(table[0])
    n, offsets = fb.vector(table[1])
    if n != count:
        raise ValueError('Invalid game archive index')
    for i in range(n):
        if fb.u64(hashes + 8*i) != fnv64(wanted):
            continue
        off = fb.u64(offsets + 8*i)
        tab = fb.table(off + fb.u32(off))
        n_hashes, member_hashes = fb.vector(tab[0])
        n_files, members = fb.vector(tab[1])
        if n_files != n_hashes or n_files > 4096:
            raise ValueError('Invalid Pokémon resource index')
        total = 0
        for j in range(n_files):
            p = members + 4*j
            ent = fb.table(p + fb.u32(p))
            comp = pack[ent[1]] if len(ent) > 1 and ent[1] else 0xff
            size = fb.u64(ent[3]) if len(ent) > 3 and ent[3] else 0
            length, start = fb.vector(ent[4])
            if not 0 < size <= MAX_MEMBER or not 0 < length <= MAX_MEMBER:
                raise ValueError('Invalid Pokémon resource size')
            total += size
            if total > MAX_PACK:
                raise ValueError('Pokémon resource pack is too large')
            raw = bytes(pack[start:start+length])
            if len(raw) != length:
                raise ValueError('Truncated Pokémon resource')
            if comp == 0xff and length != size:
                raise ValueError('Invalid uncompressed Pokémon resource')
            if comp == 0xff or length == size:
                data = raw
            else:
                out = ctypes.create_string_buffer(size + 64)
                if ooz.ooz_decompress(raw, length, out, size) != size:
                    raise ValueError('Could not decode a Pokémon resource')
                data = out.raw[:size]
            yield fb.u64(member_hashes + 8*j), data
        return
    raise ValueError('Pokémon model pack not found in this game')


def shared_texture_reference(folder, reference):
    """Resolve a material reference in the game namespace, never host paths."""
    if not isinstance(reference, str) or len(reference) > 240 or not re.fullmatch(r'[A-Za-z0-9_./-]+\.bntx', reference):
        raise ValueError('Invalid shared texture reference')
    if reference.startswith('/') or '\\' in reference or ':' in reference:
        raise ValueError('Absolute shared texture reference')
    resolved = posixpath.normpath(posixpath.join(folder, reference))
    if not resolved.startswith('ik_pokemon/share/') or '/..' in resolved:
        raise ValueError('Shared texture reference escapes game namespace')
    return resolved


def shared_texture_requests(folder, members):
    requests = set()
    # FlatBuffer strings are length prefixed. Restrict discovery to bounded ASCII
    # material references; false matches fail closed instead of copying arbitrary paths.
    pattern = re.compile(rb'(?:\.\./)+share/[A-Za-z0-9_./-]{1,180}\.bntx')
    for _, data in members:
        for raw in pattern.findall(data):
            requests.add(shared_texture_reference(folder, raw.decode('ascii')))
            if len(requests) > 64:
                raise ValueError('Too many shared material textures')
    return sorted(requests)


def read_shared_textures(pack, folder, members, ooz):
    result = []
    total = 0
    filenames = set()
    for reference in shared_texture_requests(folder, members):
        filename = posixpath.basename(reference)
        if filename in filenames:
            raise ValueError('Conflicting shared texture filenames')
        filenames.add(filename)
        archive_name = 'arc/' + reference.replace('/', '') + '.trpak'
        matches = [(h, data) for h, data in read_members(pack, archive_name, ooz) if h == fnv64(reference)]
        if len(matches) != 1 or not matches[0][1].startswith(b'BNTX'):
            raise ValueError('Shared texture member did not match its exact game path')
        data = matches[0][1]
        total += len(data)
        if total > MAX_PACK:
            raise ValueError('Shared material textures are too large')
        result.append((reference, filename, data))
    return result


def read_sibling_textures(pack, index, folder, members, model, ooz):
    """Resolve only explicit, same-model sibling texture references from local data.

    Names are flattened into the selected named cache for the renderer's existing
    basename fallback. Exact game-path hashes remain authoritative; no guessed art.
    """
    references = set()
    pattern = re.compile(rb'(?:\.\./)+pm\d{1,4}_\d{2}_\d{2}/[A-Za-z0-9_.-]{1,160}\.bntx')
    scope = f'ik_pokemon/data/pm{model:04d}/'
    for _, data in members:
        for match in pattern.finditer(data):
            raw = match.group()
            # Consume only a complete, length-prefixed FlatBuffer string.
            if match.start() < 4 or struct.unpack_from('<I', data, match.start()-4)[0] != len(raw):
                continue
            resolved = posixpath.normpath(posixpath.join(folder, raw.decode('ascii')))
            if not resolved.startswith(scope):
                raise ValueError('Sibling texture escapes the requested model namespace')
            relative = resolved[len(scope):]
            pieces = relative.split('/')
            if len(pieces) != 2 or not re.fullmatch(r'pm\d{4}_\d{2}_00', pieces[0]):
                raise ValueError('Unsupported sibling texture layout')
            if int(pieces[0][2:6]) != model:
                raise ValueError('Sibling texture model mismatch')
            references.add(resolved)
            if len(references) > 128: raise ValueError('Too many sibling texture references')
    by_pack = {}
    result = []
    total = 0
    filenames = set()
    for reference in sorted(references):
        directory, filename = reference.rsplit('/', 1)
        base = directory.rsplit('/', 1)[1]
        resident = [data for h, data in members if h == fnv64(reference)]
        if resident:
            if len(resident) != 1 or not resident[0].startswith(b'BNTX'):
                raise ValueError('Invalid resident sibling texture')
            if filename in filenames: raise ValueError('Conflicting sibling texture filenames')
            filenames.add(filename)
            result.append((reference, filename, resident[0]))
            continue
        _, _, prefix = model_names(model, int(base[7:9]))
        try:
            name = find_model_pack(index, prefix)
        except MissingModelPack as exc:
            raise ValueError('Referenced sibling texture pack is unavailable') from exc
        if name not in by_pack:
            if len(by_pack) >= 16: raise ValueError('Too many sibling model packs')
            entries = list(read_members(pack, name, ooz))
            total += sum(len(data) for _, data in entries)
            if total > MAX_PACK: raise ValueError('Sibling resources exceed extraction budget')
            by_pack[name] = entries
        found = [data for h, data in by_pack[name] if h == fnv64(reference)]
        if len(found) != 1 or not found[0].startswith(b'BNTX'):
            raise ValueError(f'Sibling texture missing at exact game path: {reference}')
        if filename in filenames: raise ValueError('Conflicting sibling texture filenames')
        filenames.add(filename)
        result.append((reference, filename, found[0]))
    return result


def discover_member_names(base, folder, members):
    """Name only exact archive members, including same-clip visibility data."""
    hashes = {h for h, _ in members}
    known = {f'{base}.trmdl'}
    pattern = re.compile(rb'pm\d{1,4}_[A-Za-z0-9_./-]{1,100}\.(?:trmdl|trmsh|trmbf|trskl|trmtr|trmmt|tranm|tracm|bntx)', re.I)
    for _, data in members:
        known.update(s.decode('ascii') for s in pattern.findall(data))
    safe = {name for name in known if '/' not in name and '\\' not in name}
    for name in tuple(safe):
        if name.lower().endswith('.tranm'):
            companion = name[:-6] + '.tracm'
            if fnv64(folder + '/' + companion) in hashes:
                safe.add(companion)
    return {fnv64(folder + '/' + name): name for name in safe
            if fnv64(folder + '/' + name) in hashes}


def extract(source, library, dest, model, form=0):
    """Extract one model pack to an explicit, local development cache."""
    source, dest = Path(source), Path(dest)
    base, folder, pack_prefix = model_names(model, form)
    ooz = ctypes.CDLL(str(library))
    ooz.ooz_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    with source.open('rb') as f:
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            image = memoryview(mm)
            pack = image if source.name == 'data.trpfs' else archive_from_image(image)
            try:
                index = source.with_name('data.trpfd').read_bytes() if source.name == 'data.trpfs' else index_from_image(image)
                pack_name = find_model_pack(index, pack_prefix)
                members = list(read_members(pack, pack_name, ooz))
                shared = read_shared_textures(pack, folder, members, ooz)
                shared += read_sibling_textures(pack, index, folder, members, model, ooz)
            finally:
                if pack is not image:
                    pack.release()
                image.release()
    if len({h for h, _ in members}) != len(members):
        raise ValueError('Duplicate Pokémon resource hashes')
    names = discover_member_names(base, folder, members)
    dest.mkdir(parents=True, exist_ok=True)
    named_dir = dest / 'named'
    named_dir.mkdir(exist_ok=True)
    if named_dir.resolve().parent != dest.resolve():
        raise ValueError('Model named cache escapes its destination')
    index = []
    for h, data in members:
        filename = f'{h:016x}.bin'
        (dest / filename).write_bytes(data)
        if h in names:
            (named_dir / names[h]).write_bytes(data)
        index.append({'hash': f'{h:016x}', 'file': filename, 'name': names.get(h), 'size': len(data)})
    shared_index = []
    root = named_dir.resolve()
    occupied = set(names.values())
    for reference, filename, data in shared:
        target = (named_dir / filename).resolve()
        if target.parent != root or filename in occupied:
            raise ValueError('Shared texture destination collision or escape')
        target.write_bytes(data)
        shared_index.append({'source': reference, 'name': filename, 'size': len(data)})
    result = {'format': 4, 'shared_textures': shared_index, 'model': model, 'form': form, 'pack': pack_name, 'members': index}
    (dest / 'index.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result



