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


# A native-ABI profile is admitted only after binary, import relocations, call
# sites, camera map queries and name-preservation semantics have been reviewed.
# Unknown modules retain the strict legacy compatibility checks below.
NATIVE_PROFILES = {'84684491199a6b9180725802558b75d9c0a0d0c4': {'camera_query_returns': [86704, 86816, 87040],
                                              'name_preservation_verified': True,
                                              'sha256': '90498556f83eb0f467bad29f1acc805ece178820842e7c63128ea8ecbae34781',
                                              'witnesses': [(86688,
                                                             'e20316aae10313aae00314aa5532059420020035e0bf40f9e00100b4e18741b9'),
                                                            (86784,
                                                             'e20315aae1030391e02303911c0080d21b0080d299b9e7f21600805239320594200700351f2003d5fa6b40f9da0600b4e0e340b9e26740f9000400125703028b'),
                                                            (87020, 'f50317aae1030391e20315aae02303910132059440f9ff34'),
                                                            (87816,
                                                             'df3e0071e8010054403c009100ec7c92e10317aa75a30594e30300aa200100b43afdff97e103002a60400091e1000036'),
                                                            (1448960, 'e10f1ff8c10000d4e20741f8410000b9c0035fd6'),
                                                            (1565936, '301700f011be44f910e2259120021fd6'),
                                                            (1566928, '301700f011b645f910a22d9120021fd6'),
                                                            (256640,
                                                             '602100b401004039020080d201210034e10000341f2003d5420400915f5c00f1800800540168623881ffff3502200034e303022aff4309d1e10300aa402600f000a02491fd7b00a9fd030091e18b01a9e31700f9ebbc0494e31740f9e18b41a9e01f0035402600f0044013918400038b84605139e41d003404401391853442f9'),
                                                            (256800,
                                                             '422600f08000038b23008052436012391f601139202100f0000c47f9400000b464ff0494402600f0'),
                                                            (257712,
                                                             '412600f022008052202100f0000c47f92260123900e7ffb4fd7b40a9ff43099180fe0414'),
                                                            (258460,
                                                             'a15a42f982625139012001ca811200f9620c0034402600d001a0249162421391e5909252e554bc72'),
                                                            (259056,
                                                             '25908d52230a00d063e02e91e4909252e454bc7225000079412600d0430000f9040000b920a064398000003420a02491400000f9c0035fd6200a00d000c02d91400000f9c0035fd6'),
                                                            (1596272, '5265636f6d7000')]},
 'd2e94b5e81e6da075256b515952e75ed2103e32a': {'camera_query_returns': [86704, 86816, 87040],
                                              'name_preservation_verified': True,
                                              'sha256': '2d041c2a270bc5cf47947eecf1c4138907e6992c6b1847310586e2e7943ac2cd',
                                              'witnesses': [(86688,
                                                             'e20316aae10313aae00314aa3d32059420020035e0bf40f9e00100b4e18741b9'),
                                                            (86784,
                                                             'e20315aae1030391e02303911c0080d21b0080d299b9e7f21600805221320594200700351f2003d5fa6b40f9da0600b4e0e340b9e26740f9000400125703028b'),
                                                            (87020, 'f50317aae1030391e20315aae0230391e931059440f9ff34'),
                                                            (87816,
                                                             'df3e0071e8010054403c009100ec7c92e10317aa55a30594e30300aa200100b43afdff97e103002a60400091e1000036'),
                                                            (1448864, 'e10f1ff8c10000d4e20741f8410000b9c0035fd6'),
                                                            (1565808, '301700f011be44f910e2259120021fd6'),
                                                            (1566800, '301700f011b645f910a22d9120021fd6'),
                                                            (256640,
                                                             '602100b401004039020080d201210034e10000341f2003d5420400915f5c00f1800800540168623881ffff3502200034e303022aff4309d1e10300aa402600f000a02491fd7b00a9fd030091e18b01a9e31700f9cbbc0494e31740f9e18b41a9e01f0035402600f0044013918400038b84605139e41d003404401391853442f9'),
                                                            (256800,
                                                             '422600f08000038b23008052436012391f601139202100f0000c47f9400000b444ff0494402600f0'),
                                                            (257712,
                                                             '412600f022008052202100f0000c47f92260123900e7ffb4fd7b40a9ff43099160fe0414'),
                                                            (258460,
                                                             'a15a42f982625139012001ca811200f9620c0034402600d001a0249162421391e5909252e554bc72'),
                                                            (259056,
                                                             '25908d52230a00d063e02e91e4909252e454bc7225000079412600d0430000f9040000b920a064398000003420a02491400000f9c0035fd6200a00d000c02d91400000f9c0035fd6'),
                                                            (1596272, '5265636f6d7000')]},
 'fed1e64fbb8479baa78e4f57afda7f468c7d4ad2': {'camera_query_returns': [86704, 86816, 87040],
                                              'name_preservation_verified': True,
                                              'sha256': '6094f9629785dfed5ffca5782565ddfdaf424c642804793cf1dac62af95c7a6a',
                                              'witnesses': [(86688,
                                                             'e20316aae10313aae00314aa5532059420020035e0bf40f9e00100b4e18741b9'),
                                                            (86784,
                                                             'e20315aae1030391e02303911c0080d21b0080d299b9e7f21600805239320594200700351f2003d5fa6b40f9da0600b4e0e340b9e26740f9000400125703028b'),
                                                            (87020, 'f50317aae1030391e20315aae02303910132059440f9ff34'),
                                                            (87816,
                                                             'df3e0071e8010054403c009100ec7c92e10317aa75a30594e30300aa200100b43afdff97e103002a60400091e1000036'),
                                                            (1448960, 'e10f1ff8c10000d4e20741f8410000b9c0035fd6'),
                                                            (1565936, '301700f011be44f910e2259120021fd6'),
                                                            (1566928, '301700f011b645f910a22d9120021fd6'),
                                                            (256640,
                                                             '602100b401004039020080d201210034e10000341f2003d5420400915f5c00f1800800540168623881ffff3502200034e303022aff4309d1e10300aa402600f000a02491fd7b00a9fd030091e18b01a9e31700f9ebbc0494e31740f9e18b41a9e01f0035402600f0044013918400038b84605139e41d003404401391853442f9'),
                                                            (256800,
                                                             '422600f08000038b23008052436012391f601139202100f0000c47f9400000b464ff0494402600f0'),
                                                            (257712,
                                                             '412600f022008052202100f0000c47f92260123900e7ffb4fd7b40a9ff43099180fe0414'),
                                                            (258460,
                                                             'a15a42f982625139012001ca811200f9620c0034402600d001a0249162421391e5909252e554bc72'),
                                                            (259056,
                                                             '25908d52230a00d063e02e91e4909252e454bc7225000079412600d0430000f9040000b920a064398000003420a02491400000f9c0035fd6200a00d000c02d91400000f9c0035fd6'),
                                                            (1596272, '5265636f6d7000')]}}


def native_profile(module, path):
    profile = NATIVE_PROFILES.get(module.build_id)
    if profile is None:
        return None
    if hashlib.sha256(Path(path).read_bytes()).hexdigest() != profile['sha256']:
        raise RuntimeError('Native compatibility module hash mismatch')
    for name in ('vmp_recomp_camera_find', 'vmp_recomp_name_ready'):
        indices = [i for i, sym in enumerate(module.syms) if sym[0] == name and sym[2] == 0]
        if len(indices) != 1:
            raise RuntimeError('Native compatibility import missing or ambiguous: ' + name)
        relocs = [r for r in module.relocs if r[2] == indices[0] and r[1] in (257, 1025, 1026)]
        if not relocs or any(r[3] != 0 or r[0] < module.rodata[0] or r[0]+8 > len(module.image) for r in relocs):
            raise RuntimeError('Native compatibility import relocation invalid: ' + name)
    for offset, expected in profile['witnesses']:
        signature = bytes.fromhex(expected)
        if bytes(module.image[offset:offset+len(signature)]) != signature:
            raise RuntimeError('Native compatibility call or name semantics changed')
    if not profile.get('name_preservation_verified'):
        raise RuntimeError('Native compatibility trainer-name preservation not verified')
    return profile


def tables_for(reference, candidate):
    old, new = Nso(str(reference)), Nso(str(candidate))
    if not old.build_id.startswith('96f2b142853fa151'):
        raise RuntimeError('The 1.3.9 compatibility reference is missing or incorrect')
    data = bytes(new.image)
    native = native_profile(new, candidate)
    for off in (() if native else (0x15248, 0x152f0)):
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


def smoke_version(contents, expected_platform=2):
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
                            version = smoke_version(contents, expected_platform=3 if build_id in NATIVE_PROFILES else 2)
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
    if build_id in NATIVE_PROFILES:
        source = Path(work) / 'input' / 'exefs' / 'subsdk9'
        module = Nso(str(source))
        if module.build_id != build_id or native_profile(module, source) is None:
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
        exec(compile((ROOT / 'tools' / 'lumiose' / 'optimize_camera_139.py').read_text(),
                     'optimize_camera_139.py', 'exec'), {})
    finally:
        if previous is None: os.environ.pop('LUMIOSE_SCRATCH', None)
        else: os.environ['LUMIOSE_SCRATCH'] = previous


def main():
    from platform_paths import WINDOWS, GAME_LIBRARY, MODULE_LIBRARY
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
    (a.work / 'prepared.json').write_text(json.dumps({'build_id': candidate.build_id, 'version': version}) + '\n')
    print('Update prepared and startup verified.', flush=True)


if __name__ == '__main__':
    try: main()
    except Exception as e:
        print('Update preparation failed:', e, file=sys.stderr)
        sys.exit(1)
