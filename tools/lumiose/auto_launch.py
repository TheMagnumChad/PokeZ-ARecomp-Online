"""Launch Lumiose, preparing a newly downloaded update before starting it."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from platform_paths import WINDOWS, EXECUTABLE, GAME_LIBRARY, MODULE_LIBRARY, lock_file

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    if not path.is_file(): return None
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
    return h.hexdigest()


def write_state(path, state):
    tmp = path.with_suffix('.tmp')
    with tmp.open('w', encoding='utf-8') as f:
        json.dump(state, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def launch(config_path, prepare_only=False):
    cfg = json.loads(config_path.read_text(encoding='utf-8'))
    install = Path(cfg['install']).resolve()
    game = install / 'extracted'
    mod_dir = install / 'lumiose'
    state_path = mod_dir / 'update-state.json'
    candidate = install / 'save' / 'sdcard' / 'atmosphere' / 'contents' / '0100F43008C44000' / 'exefs' / 'subsdk9'
    if not state_path.is_file():
        raise RuntimeError('Update setup is incomplete; no mod was started')
    state = json.loads(state_path.read_text(encoding='utf-8'))
    mod_id = mod_dir / (GAME_LIBRARY + '.id')
    id_file = game / (GAME_LIBRARY + '.id')
    if not mod_id.is_file() or not id_file.is_file():
        raise RuntimeError('The game-code identification file is missing')
    baseline = state.get('download_hash')
    current_hash = digest(candidate)
    active_name = state.get('version_dir')
    if active_name and Path(active_name).name != active_name:
        raise RuntimeError('Invalid active update path')
    active_dir = mod_dir / 'versions' / active_name if active_name else mod_dir
    active_module = active_dir / 'subsdk9'
    active_id = active_dir / (GAME_LIBRARY + '.id')
    if not active_id.is_file():  # older prepared versions kept this file only in lumiose/
        active_id = mod_id
    game_rebuild = digest(active_id) != digest(id_file)
    if (current_hash and current_hash != baseline and not state.get('force_rebuild_same_module') and
            current_hash == state.get('active_hashes', {}).get('subsdk9')):
        if digest(active_module) != current_hash:
            raise RuntimeError('The active mod failed verification')
        state['download_hash'] = current_hash
        write_state(state_path, state)
        baseline = current_hash
    # Rebuild previously prepared Linux modules once with the same volatile
    # guest-memory accesses used by the standard game build. Windows keeps its
    # existing one-time 1.3.11 switch-table refresh.
    downloaded = bool(current_hash and current_hash != baseline)
    linux_refresh = not WINDOWS and state.get('compat_revision', 0) < 2
    switch_refresh = (state.get('build_id') == 'c754ba10deedfed696a55010c8dd9131c508a647'
                      and state.get('compat_revision', 0) < 1)
    refresh = not downloaded and (linux_refresh or switch_refresh or game_rebuild)
    if refresh:
        if digest(active_module) != state.get('active_hashes', {}).get('subsdk9'):
            raise RuntimeError('The active mod failed verification')
        candidate = active_module
        current_hash = digest(candidate)
    if downloaded or refresh:
        print('Refreshing Lumiose compatibility...' if refresh else
              'A Lumiose update was downloaded. Preparing it for ZA Recomp...', flush=True)
        # Keep all generated files outside the active installation until verified.
        work_root = mod_dir / 'update-work'
        work_root.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix='prepare-', dir=work_root))
        cmd = [sys.executable, str(ROOT / 'tools' / 'lumiose' / 'auto_prepare.py'),
               '--candidate', str(candidate), '--reference', cfg['reference'],
               '--game', str(game), '--vanilla', cfg['vanilla'], '--exe', str(install / EXECUTABLE),
               '--zig', cfg['zig'], '--cache', cfg['cache'], '--work', str(work)]
        if subprocess.run(cmd, cwd=ROOT).returncode:
            raise RuntimeError(f'The update could not be prepared. Details are in {work}')
        if digest(candidate) != current_hash:
            raise RuntimeError('The download changed while being prepared; please start the launcher again')
        if digest(work / 'input' / 'exefs' / 'subsdk9') != current_hash:
            raise RuntimeError('The downloaded file changed while it was being copied')
        result = json.loads((work / 'prepared.json').read_text())
        build_id = result['build_id']
        if len(build_id) != 40 or any(c not in '0123456789abcdef' for c in build_id):
            raise RuntimeError('The prepared update has an invalid build ID')
        version = result.get('version')
        if (not isinstance(version, list) or len(version) != 4 or
                any(not isinstance(x, int) or x < 0 for x in version)):
            raise RuntimeError('The prepared update has an invalid version')
        if tuple(version) <= tuple(state.get('version', [])):
            if not (refresh and tuple(version) == tuple(state.get('version', [])) and
                    build_id == state.get('build_id') and current_hash == state.get('active_hashes', {}).get('subsdk9')):
                raise RuntimeError('The downloaded mod is not newer than the active version')
        versions = mod_dir / 'versions'
        versions.mkdir(exist_ok=True)
        suffix = ('-volatile2' if linux_refresh else '-switch1') if refresh and not game_rebuild else ''
        if game_rebuild:
            suffix += '-game-' + digest(id_file)[:12]
        name = build_id + '-' + current_hash[:16] + suffix
        target = versions / name
        files = ((work / 'input' / 'exefs' / 'subsdk9', 'subsdk9'),
                 (work / MODULE_LIBRARY, MODULE_LIBRARY), (work / GAME_LIBRARY, GAME_LIBRARY),
                 (id_file, GAME_LIBRARY + '.id'))
        if target.exists():
            if any(digest(src) != digest(target / dst) for src, dst in files):
                raise RuntimeError('A different prepared version already exists; update was not activated')
        else:
            stage = Path(tempfile.mkdtemp(prefix=name + '-', dir=versions))
            for src, dst in files:
                shutil.copy2(src, stage / dst)
                if digest(src) != digest(stage / dst): raise RuntimeError('An update file failed verification')
            os.replace(stage, target)
        state = {'download_hash': baseline if refresh else current_hash, 'build_id': build_id, 'version': version, 'version_dir': name,
                 'compat_revision': 1 if WINDOWS else 2,
                 'active_hashes': {dst: digest(target / dst) for _, dst in files}}
        # The pointer is the only active-state change, after all three files exist.
        write_state(state_path, state)
        # This directory was created solely for this preparation attempt.
        try: shutil.rmtree(work)
        except OSError: pass  # A leftover build cache is safe to remove later.
        print('Lumiose update prepared and applied.', flush=True)
    version = state.get('version_dir')
    if version:
        if Path(version).name != version: raise RuntimeError('Invalid active update path')
        active = mod_dir / 'versions' / version
    else:
        active = mod_dir
    if active != mod_dir and not (active / (GAME_LIBRARY + '.id')).is_file():
        # Repair versions produced by the earlier updater, which omitted this
        # launcher-side identification file even though the library was valid.
        shutil.copy2(mod_id, active / (GAME_LIBRARY + '.id'))
    if digest(active / (GAME_LIBRARY + '.id')) != digest(id_file):
        raise RuntimeError('The active game-code identification does not match')
    for name in ('subsdk9', MODULE_LIBRARY, GAME_LIBRARY):
        if not (active / name).is_file(): raise RuntimeError(f'The active mod is missing {name}')
        if state.get('active_hashes', {}).get(name) != digest(active / name):
            raise RuntimeError(f'The active mod file {name} failed verification')
    if digest(mod_id) != digest(id_file):
        # The active set is already verified. If a previous run stopped after switching it, finish the sidecar update.
        temp_id = mod_id.with_suffix('.id.tmp')
        shutil.copy2(id_file, temp_id)
        os.replace(temp_id, mod_id)
    env = os.environ.copy()
    env.update(LARECOMP_DATA_ROOT=str(install), LARECOMP_GAME_LIB=str(active / GAME_LIBRARY),
               LUMIOSE_TEST_MODULE=str(active / 'subsdk9'), LUMIOSE_TEST_LIBRARY=str(active / MODULE_LIBRARY),
               LUMIOSE_TEST_NETWORK='1', LUMIOSE_TEST_SERVER=cfg['server'],
               LARECOMP_DISABLE_OCCLUSION_CULLING='1',
               LUMIOSE_PREPARE_BUILD_ID=state['build_id'])
    env.pop('LUMIOSE_UI_FONT', None)
    env.pop('LUMIOSE_UI_ICONS', None)
    env.pop('LUMIOSE_UI_MODEL_ROOT', None)
    env.pop('LUMIOSE_UI_TEST_MODEL_ROOT', None)
    env.pop('LUMIOSE_UI_TEST_MODEL_FRAME', None)
    if not prepare_only:
        try:
            from game_ui_font import prepare as prepare_game_ui_font
            font = prepare_game_ui_font(install, game, mod_dir / 'ui-assets')
            if font is not None:
                env['LUMIOSE_UI_FONT'] = str(font)
        except Exception as exc:
            # A changed archive or missing optional extractor must not prevent play.
            print(f'Game UI font unavailable; using the system font: {exc}', flush=True)
        try:
            from game_ui_icons import prepare as prepare_game_ui_icons
            icons = prepare_game_ui_icons(install, game, mod_dir / 'ui-assets')
            if icons is not None:
                env['LUMIOSE_UI_ICONS'] = str(icons)
                try:
                    from game_ui_items import prepare as prepare_game_ui_items
                    prepare_game_ui_items(install, game, icons)
                except Exception as exc:
                    print(f'Game item pictures unavailable: {exc}', flush=True)
        except Exception as exc:
            print(f'Game UI icons unavailable: {exc}', flush=True)
    for key in ('LARECOMP_NO_LAUNCHER', 'LARECOMP_NO_UPDATE_CHECK', 'LARECOMP_HIDDEN', 'LUMIOSE_TEST_OFFLINE_CAPTURE',
                'LUMIOSE_TEST_DUMP', 'LUMIOSE_TEST_DROP_FILE'):
        env.pop(key, None)
    if not prepare_only:
        preview = None
        try:
            from model_preview_launcher import start as start_model_preview
            preview = start_model_preview(install, game, mod_dir, env)
        except Exception as exc:
            print(f'Animated Pokémon preview unavailable: {exc}', flush=True)
        try:
            process = subprocess.Popen([str(install / EXECUTABLE), str(game)], cwd=str(install), env=env,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except Exception:
            if preview is not None:
                preview.stop()
            raise
        if preview is not None:
            try:
                preview.attach(process.pid)
            except OSError as exc:
                preview.stop()
                print(f'Animated Pokémon preview unavailable: {exc}', flush=True)
        return process


if __name__ == '__main__':
    try:
        if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != '--prepare-only'):
            raise RuntimeError('Missing update configuration')
        config_path = Path(sys.argv[1]).resolve()
        cfg = json.loads(config_path.read_text(encoding='utf-8'))
        lock_path = Path(cfg['install']) / 'lumiose' / 'update.lock'
        with lock_path.open('a+b') as lock:
            try: lock_file(lock)
            except OSError: raise RuntimeError('Another Lumiose launcher is already preparing an update')
            launch(config_path, prepare_only=len(sys.argv) == 3)
    except Exception as e:
        print('\nLumiose could not start:', e, file=sys.stderr)
        print('Your save was not changed by the update preparation.', file=sys.stderr)
        sys.exit(1)
