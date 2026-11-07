"""Build a private test game library, reusing verified unchanged local objects."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib
import os
import subprocess
import sys
from platform_paths import WINDOWS, MACOS, ZIG, GAME_LIBRARY, POSIX_COMPILE, POSIX_LINK

root = Path(__file__).resolve().parents[2]
scratch = Path(os.environ['LUMIOSE_SCRATCH'])
source = root
cache = Path(os.environ['LUMIOSE_GAME_CACHE'])
zigroot = Path(os.environ['LUMIOSE_ZIG_DIR'])
zig = str(zigroot / ZIG)
env = os.environ.copy()
env['ZIG_LIB_DIR'] = str(zigroot / 'lib')
env['ZIG_GLOBAL_CACHE_DIR'] = str(scratch / 'zig-cache')
env['ZIG_LOCAL_CACHE_DIR'] = str(scratch / 'zig-cache')
objdir = Path(os.environ.get('LUMIOSE_OBJECT_DIR', scratch / 'objects'))
objdir.mkdir(exist_ok=True)
generated = Path(os.environ.get('LUMIOSE_GAME_GEN', scratch / 'generated'))
sources = sorted((generated / 'main').glob('*.c')) + sorted((generated / 'sdk').glob('*.c'))
target_flags = [] if MACOS else ['-mcpu=x86_64_v3']
if not WINDOWS:
    target_flags = POSIX_COMPILE + target_flags

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def prepare(src):
    existing = cache / (src.stem + '.o')
    receipt = cache / (src.stem + '.receipt')
    # The macOS vanilla build uses different guest-memory flags from the Lumiose build.
    if not MACOS and existing.is_file() and receipt.is_file():
        lines = receipt.read_text().splitlines()
        if len(lines) >= 3 and lines[0] == 'LASOBJ1' and sha(src) == lines[1] and sha(existing) == lines[2]:
            return existing, True
    out = objdir / (src.stem + '.o')
    args = [zig, 'cc', *target_flags, '-O1', '-g0', '-fno-sanitize=all', '-fno-strict-aliasing', '-Wno-return-type',
            '-Wno-unused-label', '-fvisibility=hidden', '-I', str(source / 'runtime'),
            '-c', str(src), '-o', str(out)]
    result = subprocess.run(args, env=env, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{src.name}: {result.stderr[-4000:]}')
    return out, False

objects, reused, compiled = [], 0, 0
with ThreadPoolExecutor(max_workers=8) as pool:
    pending = [pool.submit(prepare, src) for src in sources]
    for future in as_completed(pending):
        obj, was_reused = future.result()
        objects.append(obj)
        reused += was_reused
        compiled += not was_reused
        if (reused + compiled) % 100 == 0:
            print(f'Prepared {reused + compiled}/{len(sources)}; reused {reused}, rebuilt {compiled}', flush=True)
entry_obj = objdir / 'gamelib_entry.o'
game_id = Path(os.environ['LUMIOSE_GAME_ID_FILE']).read_text().strip()
if not game_id or any(c not in '0123456789abcdefABCDEF-v' for c in game_id):
    raise RuntimeError('Invalid game-code identification')
args = [zig, 'cc', *target_flags, '-O1', '-g0', '-fno-sanitize=all', '-fno-strict-aliasing',
        '-fvisibility=hidden', '-DGAMELIB_ID="' + game_id + '"',
        '-I', str(source / 'runtime'), '-c', str(source / 'tools' / 'gamelib_entry.c'), '-o', str(entry_obj)]
result = subprocess.run(args, env=env, capture_output=True, text=True)
if result.returncode:
    raise RuntimeError(f'gamelib_entry.c: {result.stderr[-4000:]}')
objects.append(entry_obj)
rsp = scratch / 'patched-objects.rsp'
rsp.write_text(''.join('"' + str(p).replace('\\', '/') + '"\n' for p in objects))
output = os.environ.get('LUMIOSE_GAME_OUTPUT', str(scratch / ('patched-' + GAME_LIBRARY)))
if WINDOWS:
    cmd = [zig, 'lld-link', '@' + str(zigroot / 'rt' / 'link.rsp'), '-LIBPATH:' + str(zigroot / 'rt'),
           '-OUT:' + output, '@' + str(rsp), str(source / 'build' / 'lasrecomp.dll.a')]
else:
    cmd = [zig, 'cc', *POSIX_LINK, '-o', output, '@' + str(rsp)]
result = subprocess.run(cmd, env=env, capture_output=True, text=True)
print(f'Link exit {result.returncode}; reused {reused}, rebuilt {compiled}', flush=True)
if result.stderr:
    print(result.stderr[-6000:])
sys.exit(result.returncode)
