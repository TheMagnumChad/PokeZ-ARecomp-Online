"""Prepare Lumiose using a player's own already-extracted game (Windows)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from platform_paths import WINDOWS, EXECUTABLE, GAME_LIBRARY, MODULE_LIBRARY, ZIG

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--install', required=True, type=Path, help='ZA Recomp install containing extracted/ and lasrecomp.exe')
    ap.add_argument('--zig', required=True, type=Path, help='ZA Recomp zig directory')
    ap.add_argument('--server', required=True, help='Lumiose relay address and port')
    args = ap.parse_args()
    install, zig = args.install.resolve(), args.zig.resolve()
    game = install / 'extracted'
    exe = install / EXECUTABLE
    vanilla = game / GAME_LIBRARY
    vanilla_id = game / (GAME_LIBRARY + '.id')
    reference = ROOT / 'lumiose' / 'subsdk9'
    bundled = ROOT / 'tools/lumiose/prepackaged.subsdk9'
    candidate = bundled if bundled.is_file() else reference  # older installations
    required = [exe, vanilla, vanilla_id, zig / ZIG, reference, candidate]
    if WINDOWS: required.append(ROOT / 'build' / 'lasrecomp.dll.a')
    for p in required:
        if not p.is_file(): raise RuntimeError(f'Required file is missing: {p}')
    mod = install / 'lumiose'
    mod.mkdir(exist_ok=True)
    state_path = mod / 'update-state.json'
    if state_path.exists(): raise RuntimeError('Lumiose is already configured; no files changed')
    work_root = mod / 'update-work'
    work_root.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='initial-', dir=work_root))
    cmd = [sys.executable, str(ROOT / 'tools/lumiose/auto_prepare.py'),
           '--candidate', str(candidate), '--reference', str(reference), '--game', str(game),
           '--vanilla', str(vanilla), '--exe', str(exe), '--zig', str(zig),
           '--cache', str(game / '.gamebuild'), '--work', str(work)]
    if subprocess.run(cmd, cwd=ROOT).returncode: raise RuntimeError(f'Preparation failed; see {work}')
    result = json.loads((work / 'prepared.json').read_text())
    files = ((work / 'input/exefs/subsdk9', 'subsdk9'), (work / MODULE_LIBRARY, MODULE_LIBRARY),
             (work / GAME_LIBRARY, GAME_LIBRARY), (vanilla_id, GAME_LIBRARY + '.id'))
    version_dir = result['build_id'] + '-' + sha(candidate)[:16]
    versions = mod / 'versions'
    versions.mkdir(exist_ok=True)
    dest = versions / version_dir
    if dest.exists(): raise RuntimeError('A version folder already exists; it was left unchanged')
    staged = Path(tempfile.mkdtemp(prefix='initial-version-', dir=versions))
    for src, name in files:
        shutil.copy2(src, staged / name)
        if sha(src) != sha(staged / name): raise RuntimeError('A prepared file did not verify')
    os.replace(staged, dest)
    shutil.copy2(vanilla_id, mod / (GAME_LIBRARY + '.id'))
    sd = install / 'save/sdcard/atmosphere/contents/0100F43008C44000/exefs/subsdk9'
    # Only mark the SD-card file consumed if it is the exact mod we prepared.
    seen = sha(sd) if sd.is_file() and sha(sd) == sha(candidate) else None
    config = {'install': str(install), 'reference': str(reference), 'vanilla': str(vanilla),
              'zig': str(zig), 'cache': str(game / '.gamebuild'), 'server': args.server}
    state = {'build_id': result['build_id'], 'version': result['version'], 'version_dir': version_dir,
             'compat_revision': 1 if WINDOWS else 2, 'download_hash': seen, 'active_hashes': {name: sha(dest / name) for _, name in files}}
    (mod / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    tmp = state_path.with_suffix('.tmp')
    tmp.write_text(json.dumps(state, indent=2) + '\n')
    os.replace(tmp, state_path)
    shutil.rmtree(work, ignore_errors=True)
    print('Lumiose prepared. Start it with:')
    print(f'  "{sys.executable}" "{ROOT / "tools/lumiose/auto_launch.py"}" "{mod / "config.json"}"')


if __name__ == '__main__':
    try: main()
    except Exception as e:
        print(f'Lumiose setup failed: {e}', file=sys.stderr)
        sys.exit(1)
