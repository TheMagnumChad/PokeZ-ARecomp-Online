"""Prepare a downloaded Lumiose NSO and its matching native game code.

This command operates only in --work; it never installs or changes a save.
The reference is the verified 1.3.9 recomp module used to locate signatures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
from native_compat import SCHEMA, ABI_REVISION, canonical_build_id, normalize_candidate, validate_candidate, discover_candidate, runtime_profile_bytes

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'python-deps'), str(ROOT / 'tools')]
from nso import Nso

# Only profiles derived and validated during THIS preparation are cached.
# Build IDs and hashes bind those bytes; they never select compatibility policy.
SUPPLIED_COMPATIBILITY = {}
DISCOVERED_COMPATIBILITY = {}
CURRENT_COMPATIBILITY = {}


def module_profile(module, path, reference=None):
    data = Path(path).read_bytes()
    if len(data) < 0x100 or data[:4] != b'NSO0':
        raise RuntimeError('Invalid compatibility module')
    module.build_id = canonical_build_id(data[0x40:0x60])
    key = (data[0x40:0x60].hex(), hashlib.sha256(data).hexdigest())
    if key not in DISCOVERED_COMPATIBILITY:
        actual = discover_candidate(module, path, reference=reference)
        supplied = SUPPLIED_COMPATIBILITY.get(key[0])
        if supplied is not None:
            accepted = validate_candidate(module, path, supplied)
            actual['additional_function_entries'] = sorted(set(actual['additional_function_entries']) |
                                                          set(accepted['additional_function_entries']))
            for key in ('version', 'executable_hook_pools'):
                if key in accepted:
                    actual[key] = accepted[key]
        DISCOVERED_COMPATIBILITY[key] = actual
    value = DISCOVERED_COMPATIBILITY[key]
    CURRENT_COMPATIBILITY[module.build_id] = value
    return value


def profile_for_build(build_id):
    return CURRENT_COMPATIBILITY.get(build_id)


def startup_has_crash(contents):
    for line in contents.splitlines():
        notice = line.strip()
        if notice == '[crash] no crash report folder on card' or notice == '[guest] [crash] no crash report folder on card':
            continue
        if any(marker in line for marker in ('[crash]', '*** exception', 'no function at ', 'FATAL', '[fatal]')):
            return True
    return False


def tables_for(reference, candidate):
    new = Nso(str(candidate))
    profile = module_profile(new, candidate, reference=reference)
    return new, profile['switch_tables']


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


def smoke_version(contents, expected_platform=2):
    """Validate a live title or HUD without requiring the player to enter the overworld."""
    if startup_has_crash(contents):
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
    if int(version.group(5)) != expected_platform:
        raise RuntimeError('The download is not the recomp version of Lumiose')
    return [int(x) for x in version.groups()[:4]]


def capture(exe, game, work, module, library, gamelib, build_id, *, snapshot, secs=60):
    env = os.environ.copy()
    env.update(LARECOMP_DATA_ROOT=str(work), LARECOMP_SAVE_DIR=str(work / 'isolated-save'),
               LARECOMP_NO_LAUNCHER='1', LARECOMP_HIDDEN='1', LARECOMP_NO_UPDATE_CHECK='1',
               LUMIOSE_TEST_MODULE=str(module), LUMIOSE_TEST_LIBRARY=str(library),
               LARECOMP_GAME_LIB=str(gamelib), LUMIOSE_TEST_OFFLINE_CAPTURE='1',
               LUMIOSE_PREPARE_BUILD_ID=build_id)
    runtime_profile = work / 'native-profile.bin'
    if runtime_profile.is_file():
        env['LUMIOSE_NATIVE_PROFILE_PATH'] = str(runtime_profile)
    else:
        env.pop('LUMIOSE_NATIVE_PROFILE_PATH', None)
    for key in ('LARECOMP_INPUT', 'LUMIOSE_TEST_NETWORK', 'LUMIOSE_TEST_DROP_FILE', 'LARECOMP_REBOOT_AT',
                'LARECOMP_LAUNCHER_SETUP', 'LARECOMP_LAUNCHER_EXTRACT', 'LARECOMP_LAUNCHER_SHOT', 'LUMIOSE_FIRST_SETUP',
                'LASRECOMP_LOG_FILE', 'LASRECOMP_INFO_LOGGED'):
        env.pop(key, None)
    if snapshot:
        env['LUMIOSE_TEST_DUMP'] = str(work)
    else:
        env.pop('LUMIOSE_TEST_DUMP', None)
    env['PATH'] = str(exe.parent) + os.pathsep + env.get('PATH', '')
    log = work / ('capture.log' if snapshot else 'smoke.log')
    runtime_log = work / 'logs' / 'play.log'
    for attempt in range(2):
        retry = False
        # The runtime can redirect stderr after rt_paths_init() to its owned
        # data-root log. Never accept markers left by the snapshot or a retry.
        if runtime_log.exists():
            os.replace(runtime_log, work / (log.stem + f'-attempt-{attempt + 1}-previous-play.log'))
        def contents_now():
            parts = []
            for path in (log, runtime_log):
                try: parts.append(path.read_text(errors='replace'))
                except FileNotFoundError: pass
            return '\n'.join(parts)
        with log.open('w', encoding='utf-8') as f:
            p = subprocess.Popen([str(exe), str(game), '--play'], cwd=str(exe.parent), env=env,
                                 stdout=f, stderr=subprocess.STDOUT,
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            def failure(reason):
                return RuntimeError(f'{reason} (attempt {attempt + 1}/2, exit code {p.poll()}; logs: {log}, {runtime_log})')
            try:
                start = time.monotonic()
                while time.monotonic() - start < secs:
                    contents = contents_now()
                    if startup_has_crash(contents):
                        raise failure('Isolated startup reported a crash')
                    if p.poll() is not None:
                        if attempt == 0 and not contents:
                            retry = True
                            break
                        raise failure('Isolated startup exited unexpectedly')
                    if not contents and attempt == 0 and time.monotonic() - start >= min(20, secs / 2):
                        retry = True
                        break
                    if '[violetmp][gate] hooks installed: 0 address misses' in contents:
                        if snapshot and '[lumiose test] wrote local module snapshots' in contents:
                            return
                        if not snapshot:
                            version = smoke_version(contents, expected_platform=3 if (profile_for_build(build_id) or {}).get('module_kind') == 'native' else 2)
                            if version is not None:
                                return version
                    time.sleep(1)
                if not retry:
                    raise failure('Isolated startup did not finish')
            finally:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        p.kill(); p.wait()
                if startup_has_crash(contents_now()):
                    raise failure('Isolated startup reported a crash')
        print('The isolated startup produced no output; trying once more...', flush=True)


def patch_generated(work, build_id):
    if (profile_for_build(build_id) or {}).get('module_kind') == 'native':
        source = Path(work) / 'input' / 'exefs' / 'subsdk9'
        module = Nso(str(source))
        if module_profile(module, source)['build_id'] != build_id or module_profile(module, source)['module_kind'] != 'native':
            raise RuntimeError('Native compatibility profile does not match generated input')
        # This exact module preserves trainer names itself and calls the bound
        # native scanner/readiness ABI. Applying legacy translated-body edits
        # would corrupt changed register allocations and duplicate hooks.
        print('Verified native camera/name compatibility profile', build_id, flush=True)
        return
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
        # This optional acceleration has an exact old generated-body template.
        # A moved/reallocated scanner retains its original guest instructions;
        # dynamic query-return discovery supplies correctness independently.
        scanner_needle = ('L_7006215368:;\n'
            '    { uint64_t ea = xr_20 + 0x0ull; uint64_t v = LD64(ea); c->x[0] = (uint64_t)(v); }')
        scanner_matches = [body for _, _, body, _ in functions if scanner_needle in body]
        if (len(scanner_matches) == 1 and scanner_matches[0].count(scanner_needle) == 1
                and 'L_70062153f4:;' in scanner_matches[0]):
            exec(compile((ROOT / 'tools' / 'lumiose' / 'optimize_camera_139.py').read_text(),
                         'optimize_camera_139.py', 'exec'), {})
        else:
            print('Legacy camera scanner retained unchanged; optional old fast-path template does not match', flush=True)
    finally:
        if previous is None: os.environ.pop('LUMIOSE_SCRATCH', None)
        else: os.environ['LUMIOSE_SCRATCH'] = previous


def main():
    from platform_paths import WINDOWS, GAME_LIBRARY, MODULE_LIBRARY
    p = argparse.ArgumentParser()
    for arg in ('candidate', 'reference', 'game', 'vanilla', 'exe', 'zig', 'cache', 'work'):
        p.add_argument('--' + arg, type=Path, required=True)
    p.add_argument('--compatibility-profile', type=Path)
    a = p.parse_args()
    for name in ('candidate', 'reference', 'game', 'vanilla', 'exe', 'zig', 'cache', 'work'):
        setattr(a, name, getattr(a, name).resolve())
    a.work.mkdir(parents=True, exist_ok=True)
    if any(a.work.iterdir()): raise RuntimeError('Work folder must be empty')
    staged = a.work / 'input' / 'exefs'
    update = normalize_candidate(a.candidate, staged / 'subsdk9', a.compatibility_profile)
    a.candidate = Path(update['path'])
    if update['selected_profile'] is not None:
        supplied = update['selected_profile']
        SUPPLIED_COMPATIBILITY[supplied['build_id32']] = supplied
    candidate, tables = tables_for(a.reference, a.candidate)
    if len(candidate.build_id) not in (40, 64): raise RuntimeError('Invalid module build ID')
    print('Preparing Lumiose build', candidate.build_id, flush=True)
    shutil.copy2(a.game / 'exefs' / 'main', staged / 'main')
    env = os.environ.copy()
    env.update(PYTHONPATH=str(ROOT / 'python-deps') + os.pathsep + os.environ.get('PYTHONPATH', ''), LUMIOSE_SCRATCH=str(a.work),
               LUMIOSE_SOURCE_MODULE=str(staged / 'subsdk9'), LUMIOSE_GAME_ROOT=str(a.game),
               LUMIOSE_ENTRY_FILE=str(a.work / 'entries.json'), LUMIOSE_ZIG_DIR=str(a.zig),
               LUMIOSE_GAME_CACHE=str(a.cache), LUMIOSE_UPDATE_TABLES=json.dumps(tables),
               LUMIOSE_GAME_ID_FILE=str(a.vanilla) + '.id')
    entries = set()
    profile = module_profile(candidate, a.candidate, reference=a.reference)
    entries.update(profile['additional_function_entries'])
    for off, count, fmt, base in tables:
        entries.update(base + v * 4 for v in struct.unpack_from(f'<{count}{fmt}', candidate.image, off))
    env['LUMIOSE_TEST_ENTRIES_LUMIOSE'] = ','.join(hex(x) for x in sorted(entries))
    profile_outputs = dict(native_sha256=update['base_sha256'], download_sha256=update['download_sha256'])
    if profile is not None:
        accepted = dict(profile)
        accepted.setdefault('switch_tables', tables)
        accepted.setdefault('additional_function_entries', sorted(entries))
        (a.work / 'compatibility.json').write_text(json.dumps(accepted, sort_keys=True) + '\n', encoding='utf-8')
        (a.work / 'native-profile.bin').write_bytes(runtime_profile_bytes(accepted))
        env['LUMIOSE_NATIVE_PROFILE_PATH'] = str(a.work / 'native-profile.bin')
        profile_outputs.update(build_id32=accepted['build_id32'], compatibility_abi_revision=ABI_REVISION,
            compatibility_profile='compatibility.json', runtime_profile='native-profile.bin',
            module_kind=accepted['module_kind'], native_abi=accepted['module_kind'] == 'native',
            camera_query_count=len(accepted['camera_query_returns']), runtime_profile_format=2)
    bootstrap = a.work / 'generated' / 'bootstrap'
    bootstrap.mkdir(parents=True)
    py = sys.executable
    gen = ROOT / 'tools' / 'gen.py'
    run([py, str(gen), 'lumiose', str(staged / 'subsdk9'), '0x7006200000', str(bootstrap), '--simd', 'c'],
        env, a.work / 'bootstrap-generation.log')
    env.update(LUMIOSE_GENERATED_DIR=str(bootstrap), LUMIOSE_MODULE_OBJECT_DIR=str(a.work / 'bootstrap-objects'),
               LUMIOSE_LIBRARY_OUTPUT=str(a.work / ('bootstrap.dll' if WINDOWS else 'bootstrap.so')))
    run([py, str(ROOT / 'tools' / 'lumiose' / 'build_module.py')], env, a.work / 'bootstrap-build.log')
    capture(a.exe, a.game, a.work, staged / 'subsdk9', a.work / ('bootstrap.dll' if WINDOWS else 'bootstrap.so'), a.vanilla,
            candidate.build_id, snapshot=True)
    for script in ('prepare_patched_modules.py', 'find_hook_entries.py'):
        run([py, str(ROOT / 'tools' / 'lumiose' / script)], env, a.work / (script + '.log'))
    from fast_retarget import current_cache, retarget, save_cache
    cache_root = a.game.parent / 'lumiose' / 'generation-cache'
    # Optional existing cache is a read-only input. Retarget validates its
    # fingerprints/hashes; new generated results still save only to cache_root.
    readonly_cache = os.environ.get('LUMIOSE_READONLY_GENERATION_CACHE')
    lookup_root = Path(readonly_cache).resolve() if readonly_cache else cache_root
    cached = current_cache(lookup_root)
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
    (a.work / 'prepared.json').write_text(json.dumps({'build_id': candidate.build_id, 'version': version, **profile_outputs}) + '\n')
    print('Update prepared and startup verified.', flush=True)


if __name__ == '__main__':
    try: main()
    except Exception as e:
        print('Update preparation failed:', e, file=sys.stderr)
        sys.exit(1)
