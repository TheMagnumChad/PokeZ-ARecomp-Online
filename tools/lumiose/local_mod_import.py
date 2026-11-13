"""Local-only import of the legacy compatibility reference and current mod."""
from pathlib import Path
import os, shutil, tempfile, sys

def _read_path(value):
    raw = value.strip().strip('"')
    if not raw:
        raise RuntimeError('Mod selection cancelled. No game files or saves were changed.')
    path = Path(raw).expanduser().resolve(strict=True)
    if not path.is_file() or not 64 <= path.stat().st_size <= 64 * 1024 * 1024:
        raise RuntimeError('Choose a valid separately supplied mod NSO file (maximum 64 MiB).')
    with path.open('rb') as f:
        if f.read(4) != b'NSO0':
            raise RuntimeError('The selected file is not an NSO module.')
    return path

def _validate_pair(root, reference, candidate):
    sys.path[:0] = [str(root/'python-deps'), str(root/'tools/lumiose'), str(root/'tools')]
    from auto_prepare import tables_for
    tables_for(reference, candidate)  # Keep all existing compatibility guards.

def _copy_new(source, target):
    if target.exists():
        return
    fd, name = tempfile.mkstemp(prefix='mod-import-', suffix='.tmp', dir=target.parent)
    os.close(fd)
    tmp = Path(name)
    try:
        shutil.copyfile(source, tmp)
        os.link(tmp, target)  # Atomic create, never overwrite an existing file.
    finally:
        tmp.unlink(missing_ok=True)

def ensure_local_mod(root, choose=input):
    root = Path(root)
    mod = root/'lumiose'
    mod.mkdir(parents=True, exist_ok=True)
    reference = mod/'subsdk9'
    candidate = mod/'imported-mod.subsdk9'
    if reference.is_file() and (mod/'update-state.json').is_file():
        return reference  # Configured installations keep their current update flow.
    if reference.is_file() and candidate.is_file():
        _validate_pair(root, reference, candidate)
        return reference
    print('The launcher does not bundle the mod, its legacy reference, or game data.')
    print('First setup requires TWO separately supplied files: the supported 1.3.9 compatibility reference and your current online mod.')
    print('Arbitrary game NSOs are unsupported. Both files stay local; compatibility is checked before compilation.')
    ref_source = reference if reference.is_file() else _read_path(choose('Legacy 1.3.9 reference subsdk9 path (blank cancels): '))
    mod_source = candidate if candidate.is_file() else _read_path(choose('Current online mod subsdk9 path (blank cancels): '))
    _validate_pair(root, ref_source, mod_source)
    _copy_new(ref_source, reference)
    _copy_new(mod_source, candidate)
    return reference
