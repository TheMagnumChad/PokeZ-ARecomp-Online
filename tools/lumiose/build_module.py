"""Compile the local experiment; generated files and outputs stay in this workspace."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import subprocess
import sys
from platform_paths import WINDOWS, ZIG, LIBRARY_SUFFIX, POSIX_COMPILE, POSIX_LINK

root = Path(__file__).resolve().parents[2]
probe = root / 'tools' / 'lumiose'
source = root
zigroot = Path(os.environ['LUMIOSE_ZIG_DIR'])
zig = str(zigroot / ZIG)
env = os.environ.copy()
env['ZIG_LIB_DIR'] = str(zigroot / 'lib')
cache_dir = Path(os.environ['LUMIOSE_SCRATCH']) / 'zig-cache'
env['ZIG_GLOBAL_CACHE_DIR'] = str(cache_dir)
env['ZIG_LOCAL_CACHE_DIR'] = str(cache_dir)
objdir = Path(os.environ.get('LUMIOSE_MODULE_OBJECT_DIR', probe / 'objects'))
objdir.mkdir(exist_ok=True)
generated = Path(os.environ.get('LUMIOSE_GENERATED_DIR', probe / 'generated-branch'))
files = sorted(generated.glob('*.c'))
files.append(source / 'tools' / 'lumiose_test_entry.c')

def compile_one(src):
    out = objdir / (src.stem + '.o')
    args = [zig, 'cc', '-O0', '-g0', '-fno-sanitize=all', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-DLUMIOSE_TEST_CODE',
            '-I', str(source / 'runtime'), '-c', str(src), '-o', str(out)]
    if not WINDOWS: args[2:2] = POSIX_COMPILE
    result = subprocess.run(args, env=env, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{src.name}: {result.stderr}')
    return out

with ThreadPoolExecutor(max_workers=6) as pool:
    objects = list(pool.map(compile_one, files))
print(f'Compiled {len(objects)} objects', flush=True)
output = Path(os.environ.get('LUMIOSE_LIBRARY_OUTPUT', probe / ('lumiose-test' + LIBRARY_SUFFIX)))
if WINDOWS:
    cmd = [zig, 'lld-link', '@' + str(zigroot / 'rt' / 'link.rsp'),
           '-LIBPATH:' + str(zigroot / 'rt'), '-OUT:' + str(output),
           *map(str, objects), str(source / 'build' / 'lasrecomp.dll.a')]
else:
    cmd = [zig, 'cc', *POSIX_LINK, '-o', str(output), *map(str, objects)]
result = subprocess.run(cmd, env=env, capture_output=True, text=True)
print(result.stderr)
sys.exit(result.returncode)
