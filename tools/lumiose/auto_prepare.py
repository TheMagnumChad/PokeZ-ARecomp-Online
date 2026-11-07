"""Prepare a downloaded Lumiose NSO and its matching native game code.

This command operates only in --work; it never installs or changes a save.
The reference is the verified 1.3.9 recomp module used to locate signatures.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from nso import Nso

TABLES_139 = [(0x57fdc8, 91, 'h', 0x136c44), (0x194b40, 123, 'h', 0x364cc),
              (0x212290, 14, 'b', 0xc1d74)]


def unique(data, signature, description):
    pos = data.find(signature)
    if pos < 0 or data.find(signature, pos + 1) >= 0:
        raise RuntimeError(f'{description} changed or is ambiguous; this update needs compatibility work')
    return pos


def tables_for(reference, candidate):
    old, new = Nso(str(reference)), Nso(str(candidate))
    if not old.build_id.startswith('96f2b142853fa151'):
        raise RuntimeError('The 1.3.9 compatibility reference is missing or incorrect')
    data = bytes(new.image)
    for off in (0x15248, 0x152f0):
        # The BL destination can move; the return address and surrounding
        # camera-query instructions must retain the same meaning.
        if (bytes(old.image[off - 12:off - 4]) != bytes(new.image[off - 12:off - 4]) or
                bytes(old.image[off:off + 12]) != bytes(new.image[off:off + 12])):
            raise RuntimeError('Camera integration changed; this update needs compatibility work')
        def branch_target(module):
            word = struct.unpack_from('<I', module.image, off - 4)[0]
            if word >> 26 != 0b100101: raise RuntimeError('Camera query call changed')
            value = word & 0x3ffffff
            return off - 4 + (value - (1 << 26) if value & (1 << 25) else value) * 4
        a, b = branch_target(old), branch_target(new)
        if min(a, b) < 0 or a + 16 > old.text[1] or b + 16 > new.text[1] or \
                bytes(old.image[a:a + 16]) != bytes(new.image[b:b + 16]):
            raise RuntimeError('Camera query implementation changed')
    found = []
    for off, count, fmt, base in TABLES_139:
        table = bytes(old.image[off:off + count * struct.calcsize(fmt)])
        target = bytes(old.image[base:base + 32])
        noff = unique(data, table, 'Jump table')
        nbase = unique(data[:new.text[1]], target, 'Jump table destination')
        found.append((noff, count, fmt, nbase))
    return new, found


def run(command, env, log):
    step = Path(command[-1]).name if len(command) == 2 else Path(command[1]).name
    print('Preparing:', step, flush=True)
    with Path(log).open('w', encoding='utf-8') as f:
        p = subprocess.Popen(command, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
        last = ''
        while True:
            try:
                p.wait(timeout=5)
                break
            except subprocess.TimeoutExpired:
                pass
            progress_file = Path(log).parent / 'generate-main.log' if step == 'generate_patched.py' else Path(log)
            try:
                lines = progress_file.read_text(encoding='utf-8', errors='replace').splitlines()
                recent = next((line for line in reversed(lines) if 'progress ' in line or
                               line.startswith('Prepared ') or line.startswith('Compiled ')), '')
            except OSError:
                recent = ''
            if recent and recent != last:
                print('  ' + recent, flush=True)
                last = recent
    if p.returncode:
        raise RuntimeError(f'Preparation failed (see {log})')


def smoke_version(contents):
    """Validate a live title or HUD without requiring the player to enter the overworld."""
    if '[crash]' in contents or '*** exception' in contents or 'no function at ' in contents:
        raise RuntimeError('The isolated startup reported a crash')
    if '[violetmp][gate] hooks installed: 0 address misses' not in contents:
        return None
    # A fresh isolated save can remain at the title screen: there is no HUD to size there. The mod's renderer and
    # three present callbacks demonstrate that its UI initialized and is receiving frames, even when it stays hidden.
    title_ready = ('[ui] renderer ready' in contents and
                   re.search(r'\[ui\] present (?:[3-9]|[1-9]\d+):', contents))
    if '[ui] HUD size:' not in contents and not title_ready:
        return None
    version = re.search(r'\[update\] this build is (\d+)\.(\d+)\.(\d+) build (\d+) \(platform (\d+), channel (\d+)\)', contents)
    if not version:
        raise RuntimeError('The updated mod did not identify its version')
    if int(version.group(5)) != 2:
        raise RuntimeError('The download is not the recomp version of Lumiose')
    return [int(x) for x in version.groups()[:4]]


def capture(exe, game, work, module, library, gamelib, build_id, *, snapshot, secs=60):
    env = os.environ.copy()
    env.update(LARECOMP_DATA_ROOT=str(work), LARECOMP_SAVE_DIR=str(work / 'isolated-save'),
               LARECOMP_NO_LAUNCHER='1', LARECOMP_HIDDEN='1', LARECOMP_NO_UPDATE_CHECK='1',
               LUMIOSE_TEST_MODULE=str(module), LUMIOSE_TEST_LIBRARY=str(library),
               LARECOMP_GAME_LIB=str(gamelib), LUMIOSE_TEST_OFFLINE_CAPTURE='1',
               LUMIOSE_PREPARE_BUILD_ID=build_id)
    for key in ('LARECOMP_INPUT', 'LUMIOSE_TEST_NETWORK', 'LUMIOSE_TEST_DROP_FILE', 'LARECOMP_REBOOT_AT',
                'LARECOMP_LAUNCHER_SETUP', 'LARECOMP_LAUNCHER_EXTRACT', 'LARECOMP_LAUNCHER_SHOT', 'LUMIOSE_FIRST_SETUP'):
        env.pop(key, None)
    if snapshot:
        env['LUMIOSE_TEST_DUMP'] = str(work)
    else:
        env.pop('LUMIOSE_TEST_DUMP', None)
    env['PATH'] = str(exe.parent) + os.pathsep + env.get('PATH', '')
    log = work / ('capture.log' if snapshot else 'smoke.log')
    for attempt in range(2):
        retry = False
        with log.open('w', encoding='utf-8') as f:
            p = subprocess.Popen([str(exe), str(game), '--play'], cwd=str(exe.parent), env=env,
                                 stdout=f, stderr=subprocess.STDOUT,
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            try:
                start = time.monotonic()
                while time.monotonic() - start < secs:
                    contents = log.read_text(errors='replace')
                    if '[crash]' in contents or '*** exception' in contents or 'no function at ' in contents:
                        raise RuntimeError(f'Isolated startup reported a crash (see {log})')
                    if p.poll() is not None:
                        if attempt == 0 and not contents:
                            retry = True
                            break
                        raise RuntimeError(f'Isolated startup exited unexpectedly (see {log})')
                    if not contents and attempt == 0 and time.monotonic() - start >= min(20, secs / 2):
                        retry = True
                        break
                    if '[violetmp][gate] hooks installed: 0 address misses' in contents:
                        if snapshot and '[lumiose test] wrote local module snapshots' in contents:
                            return
                        if not snapshot:
                            version = smoke_version(contents)
                            if version is not None:
                                return version
                    time.sleep(1)
                if not retry:
                    raise RuntimeError(f'Isolated startup did not finish (see {log})')
            finally:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        p.kill(); p.wait()
        print('The isolated startup produced no output; trying once more...', flush=True)


def patch_generated(work, build_id):
    root = work / 'generated' / 'lumiose'
    # Locate the name code from its structure, then verify every edit. Mod
    # updates routinely move functions and globals without changing semantics.
    files = [(p, p.read_text()) for p in root.glob('lumiose_*.c')]
    functions = []
    for p, text in files:
        starts = list(re.finditer(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{', text, re.M))
        for i, match in enumerate(starts):
            end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
            functions.append((p, match.group(1), text[match.start():end],
                              starts[i + 1].group(1) if i + 1 < len(starts) else None))
    def one(items, reason):
        if len(items) != 1: raise RuntimeError('Trainer-name compatibility ' + reason + ' changed or is ambiguous')
        return items[0]
    init = one([f for f in functions if '0x54500000ull' in f[2] and
                'c->x[2] = (uint64_t)(0x0ull);\n    (void)(logic64(c, c->x[3] & 0xfull));' in f[2]], 'initializer')
    dest = one(re.findall(r'c->x\[0\] = \(uint64_t\)\(0x([0-9a-f]+)ull\);\n'
                          r'    c->x\[0\] = \(uint64_t\)\(c->x\[0\] \+ (0x[0-9a-f]+)ull\);\n'
                          r'    xr_19 = \(uint64_t\)\(xr_19 \+ (0x[0-9a-f]+)ull\);\n'
                          r'    c->x\[30\] = 0x([0-9a-f]+)ull; (f_[0-9a-f]+)\(c\);', init[2]), 'copy')
    base, offset, struct_offset, return_pc, target = dest
    name_addr = f'{int(base, 16) + int(offset, 16):x}'
    getter = one([f for f in functions if 'c->x[4] = (uint32_t)(0x6c81ull);' in f[2] and
                  'c->x[1] = (uint32_t)(0x9487ull);' in f[2] and
                  '{ uint64_t ea = c->x[2] + 0x0ull; ST64(ea, c->x[3]); }' in f[2]], 'getter')
    if not getter[3]: raise RuntimeError('Trainer-name compatibility getter boundary changed')
    default = one(re.findall(r'c->x\[3\] = \(uint64_t\)\(0x([0-9a-f]+)ull\);\n'
                             r'    c->x\[3\] = \(uint64_t\)\(c->x\[3\] \+ (0x[0-9a-f]+)ull\);', getter[2]), 'default')
    setter_pattern = (f'c->x[0] = (uint64_t)(0x{base}ull);\n'
                      f'    c->x[0] = (uint64_t)(c->x[0] + {offset}ull);')
    setter = one([f for f in functions if setter_pattern in f[2] and
                  '(void)(subs64(c, c->x[2], 0x17ull));' in f[2] and f[1] != init[1]], 'setter')
    # This call site identifies the game-to-mod trainer update. Keep the
    # exact surrounding instructions checked so unrelated calls cannot match.
    ready = one([(p, m.group(1)) for p, text in files for m in re.finditer(
        r'c->x\[0\] = \(uint64_t\)\(0x[0-9a-f]+ull\);\n'
        r'    c->x\[0\] = \(uint64_t\)\(c->x\[0\] \+ 0x[0-9a-f]+ull\);\n'
        r'    c->x\[30\] = 0x([0-9a-f]+)ull; f_' + setter[1] + r'\(c\);', text)], 'ready hook')
    changes = {'7006237cb0': init[1], '7006237f50': getter[1], '7006237f80': getter[3],
               '7006237d80': return_pc, '70068088d8': name_addr,
               '700634d000': default[0], '0x480ull': struct_offset + 'ull', '0xc18ull': default[1] + 'ull',
               '700622ad4c': ready[1], 'f_7006237500(c)': 'f_' + setter[1] + '(c)',
               'f_700632ce40(c)': target + '(c)'}
    patch = (ROOT / 'tools' / 'lumiose' / 'preserve_name_139.py').read_text()
    for old, new in changes.items(): patch = patch.replace(old, new)
    previous = os.environ.get('LUMIOSE_SCRATCH')
    os.environ['LUMIOSE_SCRATCH'] = str(work)
    try:
        exec(compile(patch, 'preserve_name_139.py', 'exec'), {})
        exec(compile((ROOT / 'tools' / 'lumiose' / 'optimize_camera_139.py').read_text(),
                     'optimize_camera_139.py', 'exec'), {})
    finally:
        if previous is None: os.environ.pop('LUMIOSE_SCRATCH', None)
        else: os.environ['LUMIOSE_SCRATCH'] = previous


def main():
    from platform_paths import GAME_LIBRARY, MODULE_LIBRARY, LIBRARY_SUFFIX
    p = argparse.ArgumentParser()
    for arg in ('candidate', 'reference', 'game', 'vanilla', 'exe', 'zig', 'cache', 'work'):
        p.add_argument('--' + arg, type=Path, required=True)
    a = p.parse_args()
    for name in ('candidate', 'reference', 'game', 'vanilla', 'exe', 'zig', 'cache', 'work'):
        setattr(a, name, getattr(a, name).resolve())
    a.work.mkdir(parents=True, exist_ok=True)
    if any(a.work.iterdir()): raise RuntimeError('Work folder must be empty')
    candidate, tables = tables_for(a.reference, a.candidate)
    if len(candidate.build_id) != 40: raise RuntimeError('Invalid module build ID')
    print('Preparing Lumiose build', candidate.build_id, flush=True)
    staged = a.work / 'input' / 'exefs'
    staged.mkdir(parents=True)
    shutil.copy2(a.candidate, staged / 'subsdk9')
    shutil.copy2(a.game / 'exefs' / 'main', staged / 'main')
    env = os.environ.copy()
    env.update(PYTHONPATH=str(ROOT / 'python-deps') + os.pathsep + os.environ.get('PYTHONPATH', ''), LUMIOSE_SCRATCH=str(a.work),
               LUMIOSE_SOURCE_MODULE=str(staged / 'subsdk9'), LUMIOSE_GAME_ROOT=str(a.game),
               LUMIOSE_ENTRY_FILE=str(a.work / 'entries.json'), LUMIOSE_ZIG_DIR=str(a.zig),
               LUMIOSE_GAME_CACHE=str(a.cache), LUMIOSE_UPDATE_TABLES=json.dumps(tables),
               LUMIOSE_GAME_ID_FILE=str(a.vanilla) + '.id')
    entries = set()
    for off, count, fmt, base in tables:
        entries.update(base + v * 4 for v in struct.unpack_from(f'<{count}{fmt}', candidate.image, off))
    env['LUMIOSE_TEST_ENTRIES_LUMIOSE'] = ','.join(hex(x) for x in entries)
    bootstrap = a.work / 'generated' / 'bootstrap'
    bootstrap.mkdir(parents=True)
    py = sys.executable
    gen = ROOT / 'tools' / 'gen.py'
    run([py, str(gen), 'lumiose', str(staged / 'subsdk9'), '0x7006200000', str(bootstrap), '--simd', 'c'],
        env, a.work / 'bootstrap-generation.log')
    bootstrap_library = a.work / ('bootstrap' + LIBRARY_SUFFIX)
    env.update(LUMIOSE_GENERATED_DIR=str(bootstrap), LUMIOSE_MODULE_OBJECT_DIR=str(a.work / 'bootstrap-objects'),
               LUMIOSE_LIBRARY_OUTPUT=str(bootstrap_library))
    run([py, str(ROOT / 'tools' / 'lumiose' / 'build_module.py')], env, a.work / 'bootstrap-build.log')
    capture(a.exe, a.game, a.work, staged / 'subsdk9', bootstrap_library, a.vanilla,
            candidate.build_id, snapshot=True)
    for script in ('prepare_patched_modules.py', 'find_hook_entries.py'):
        run([py, str(ROOT / 'tools' / 'lumiose' / script)], env, a.work / (script + '.log'))
    from fast_retarget import current_cache, retarget, save_cache
    cache_root = a.game.parent / 'lumiose' / 'generation-cache'
    cached = current_cache(cache_root)
    if cached and retarget(cached, a.work):
        env['LUMIOSE_SKIP_GAME_GENERATION'] = '1'
    else:
        env.pop('LUMIOSE_SKIP_GAME_GENERATION', None)
    run([py, str(ROOT / 'tools' / 'lumiose' / 'generate_patched.py')], env,
        a.work / 'generate_patched.py.log')
    patch_generated(a.work, candidate.build_id)
    env.update(LUMIOSE_GENERATED_DIR=str(a.work / 'generated' / 'lumiose'),
               LUMIOSE_MODULE_OBJECT_DIR=str(a.work / 'module-objects'),
               LUMIOSE_LIBRARY_OUTPUT=str(a.work / MODULE_LIBRARY),
               LUMIOSE_GAME_OUTPUT=str(a.work / GAME_LIBRARY))
    for script in ('build_module.py', 'build_patched_game.py'):
        run([py, str(ROOT / 'tools' / 'lumiose' / script)], env, a.work / (script + '.log'))
    version = capture(a.exe, a.game, a.work, staged / 'subsdk9', a.work / MODULE_LIBRARY, a.work / GAME_LIBRARY,
                      candidate.build_id, snapshot=False)
    try:
        save_cache(cache_root, a.work)
    except Exception as e:
        print('Could not save the optional speed-up cache:', e, flush=True)
    (a.work / 'prepared.json').write_text(json.dumps({'build_id': candidate.build_id, 'version': version}) + '\n')
    print('Update prepared and startup verified.', flush=True)


if __name__ == '__main__':
    try: main()
    except Exception as e:
        print('Update preparation failed:', e, file=sys.stderr)
        sys.exit(1)
