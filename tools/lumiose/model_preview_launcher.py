"""Optional launcher lifecycle for the local, game-sourced model renderer."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class PreviewSession:
    def __init__(self, process, request, parent_file, ready):
        self.process = process
        self.request = request
        self.parent_file = parent_file
        self.ready = ready

    def attach(self, game_pid):
        temporary = self.parent_file.with_suffix('.pending')
        temporary.write_text(str(game_pid) + '\n', encoding='ascii')
        os.replace(temporary, self.parent_file)

    def stop(self):
        try:
            self.ready.unlink(missing_ok=True)
        except OSError:
            pass
        try:
            self.request.write_text('stop\n', encoding='ascii')
            self.process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            if self.process.poll() is None:
                self.process.terminate()
                self.process.wait(timeout=5)


def start(install, game, mod_dir, env):
    """Start only when an explicitly packaged helper is present; otherwise return None."""
    # This release supplies the animated renderer for Windows only. Linux keeps Icons.
    if os.name != "nt":
        env.pop("LUMIOSE_UI_MODEL_ROOT", None)
        return None
    install, game, mod_dir = Path(install), Path(game), Path(mod_dir)
    renderer = mod_dir / 'model-renderer' / 'UiModelProbe.exe'
    service = install / 'tools' / 'lumiose' / 'model_preview_service.py'
    library = install / 'zaprompts' / '_internal' / ('ooz.dll' if os.name == 'nt' else 'libooz.so')
    source = game / 'romfs' / 'arc' / 'data.trpfs'
    if not source.is_file():
        source = game / 'romfs.bin'
    if not all(path.is_file() for path in (renderer, service, library, source)):
        return None
    asset_root = mod_dir / 'ui-assets'
    asset_root.mkdir(parents=True, exist_ok=True)
    session = Path(tempfile.mkdtemp(prefix='model-session-', dir=asset_root))
    request, parent_file, ready = session / 'request.txt', session / 'parent.txt', session / 'ready.txt'
    request.write_text('', encoding='ascii')
    command = [sys.executable, str(service), '--request', str(request), '--frames', str(session),
               '--game', str(source), '--ooz', str(library), '--cache', str(asset_root / 'models'),
               '--renderer', str(renderer), '--parent-file', str(parent_file), '--ready-file', str(ready)]
    process = subprocess.Popen(command, cwd=install, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    result = PreviewSession(process, request, parent_file, ready)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and process.poll() is None:
        if ready.is_file():
            env['LUMIOSE_UI_MODEL_ROOT'] = str(session)
            return result
        time.sleep(0.05)
    result.stop()
    return None
