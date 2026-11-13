"""Experimental, local-only animated model supervisor. Not enabled by the launcher."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

from game_ui_models import extract, model_names, MissingModelPack
from game_ui_model_catalog import resolve_model


def parse_request(text):
    """Preserve legacy tuples; v2 carries a bounded identity and host nonce."""
    try:
        parts = text.strip().split()
        if parts and parts[0] == 'v2':
            if len(parts) != 7:
                return None
            key, nonce, model, form, gender, shiny = map(int, parts[1:])
            if not (0 <= key <= 0xfffff and key & 0x7ff and 1 <= nonce <= 0xffffffffffffffff):
                return None
            if not (0 <= form <= 63 and 0 <= gender <= 2 and shiny in (0, 1)):
                return None
            if ((key >> 11) & 63, (key >> 17) & 3, (key >> 19) & 1) != (form, gender, shiny):
                return None
            model_names(model, form)
            return ('v2', key, nonce, model, form, gender, shiny)
        if len(parts) not in (3, 4):
            return None
        cell, model, form = map(int, parts[:3])
        shiny = int(parts[3]) if len(parts) == 4 else 0
        if shiny not in (0, 1) or not 0 <= cell <= 65535:
            return None
        model_names(model, form)
        return cell, model, form, shiny
    except (TypeError, ValueError):
        return None


def selection_fields(selection):
    if selection[0] == 'v2':
        _, key, nonce, model, form, gender, shiny = selection
        return key, model, form, gender, shiny
    key, model, form, shiny = selection
    return key, model, form, 0, shiny


def selection_directory(root, selection):
    return root / (f'v2-{selection[1]}' if selection[0] == 'v2' else f'cell-{selection[0]}')


def write_status(root, selection, state):
    if selection is None or selection[0] != 'v2':
        return
    if state not in ('loading', 'ready', 'error'):
        raise ValueError('Invalid preview status')
    directory = selection_directory(root, selection)
    target = directory / 'status.txt'
    temporary = directory / 'status.pending'
    try:
        directory.mkdir(parents=True, exist_ok=True)
        temporary.write_text(f'{selection[1]} {selection[2]} {state}\n', encoding='ascii', newline='\n')
        os.replace(temporary, target)
        return True
    except PermissionError:
        # Windows readers may briefly hold a non-delete-sharing handle.
        # Keep the last atomic status intact; the next service tick retries.
        # Identity+nonce checks prevent an older status authorizing a new frame.
        return False


def renderer_fingerprint(renderer):
    """Content identity, not a claim that a manifest is authentic or licensed."""
    digest = hashlib.sha256()
    files = {renderer, renderer.parent/'UiModelProbe.dll',
             renderer.parent/'GFTool.Core.dll', renderer.parent/'GFTool.Renderer.dll',
             renderer.parent/'BntxLibrary.dll'}
    shaders = renderer.parent/'Shaders'
    if shaders.is_dir():
        files.update(path for path in shaders.rglob('*') if path.is_file())
    for path in sorted(files):
        digest.update(path.relative_to(renderer.parent).as_posix().encode('utf-8'))
        try:
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
        except FileNotFoundError:
            digest.update(b'absent')
    return digest.hexdigest()[:24]


def prepare_selection(source, library, cache, selection):
    key, model, form, gender, _ = selection_fields(selection)
    if selection[0] == 'v2':
        entry = resolve_model(source, library, key & 0x7ff, form, gender)
        expected = Path(entry['model_path']).name
        base = expected[:-6]
        # The local catalog is authoritative; the host model number is only a
        # legacy hint. Never guess a form offset or reinterpret an icon cell.
        resolved_model, resolved_form, variant = int(base[2:6]), int(base[7:9]), int(base[10:12])
        if variant != 0:
            raise ValueError('This catalog resource variant needs extraction support')
        mesh, idle = prepare_model_form(source, library, cache, resolved_model, resolved_form)
        if mesh.name != expected:
            raise ValueError('Prepared model does not match the local catalog')
        return mesh, idle
    return prepare_model(source, library, cache, model, form)

def game_stamp(source):
    st = source.stat()
    return f'{st.st_size}:{st.st_mtime_ns}'


def write_control(frame_root, model=None, idle=None, output=None, shiny=False):
    """Send an atomic selection to the persistent renderer, or pause it."""
    frame_root.mkdir(parents=True, exist_ok=True)
    target = frame_root / 'renderer-control.txt'
    temporary = frame_root / 'renderer-control.pending'
    value = f'{model}\n{idle}\n{output}\n{int(shiny)}\n' if model is not None else 'pause\n'
    temporary.write_text(value, encoding='utf-8', newline='\n')
    os.replace(temporary, target)
    return target


def copy_frame(source, target):
    """Copy only complete RGBA frames, using a separate temporary filename."""
    try:
        data = source.read_bytes()
        if len(data) != 192 * 192 * 4:
            return False
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.cache-pending')
        temporary.write_bytes(data)
        os.replace(temporary, target)
        return True
    except OSError:
        return False


def prepare_model(source, library, cache, model, form):
    try:
        return prepare_model_form(source, library, cache, model, form)
    except MissingModelPack:
        # Some game packs number the first form from 11 rather than 0, as do
        # the existing icon lookups. Preserve nonzero forms with that offset.
        if form + 11 > 99:
            raise
        return prepare_model_form(source, library, cache, model, form + 11)


def find_preview_idle(named, base):
    """Use an explicitly named looping idle, never an arbitrary action clip."""
    preferred = named / (base + '_00000_defaultwait01_loop.tranm')
    if preferred.is_file():
        return preferred
    for label in ('defaultwait01_loop', 'battlewait01_loop'):
        matches = sorted(named.glob(base + '_?????_' + label + '.tranm'))
        if len(matches) > 1:
            raise ValueError('Ambiguous model idle animation')
        if matches:
            return matches[0]
    return None

def prepare_model_form(source, library, cache, model, form):
    base, _, _ = model_names(model, form)
    dest = cache / base
    marker = dest / 'source.json'
    stamp = game_stamp(source)
    expected = dest / 'named' / (base + '.trmdl')
    def idle_animation():
        return find_preview_idle(dest / 'named', base)
    try:
        metadata = json.loads(marker.read_text(encoding='utf-8'))
        current = metadata['game'] if metadata.get('model_format') == 4 else None
    except (OSError, ValueError, KeyError, TypeError):
        current = None
    idle = idle_animation()
    if current != stamp or not expected.is_file() or idle is None:
        extract(source, library, dest, model, form)
        idle = idle_animation()
        if not expected.is_file() or idle is None:
            raise ValueError('The model or its default idle animation is unavailable')
        marker.write_text(json.dumps({'game': stamp, 'model_format': 4}) + '\n', encoding='utf-8')
    return expected, idle


class ParentWatch:
    """Retain a handle to the launched game so the renderer exits with it."""
    def __init__(self, parent_file):
        self.path = parent_file
        self.deadline = time.monotonic() + 30
        self.handle = None
        self.pid = None
        if os.name == 'nt':
            self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            self.kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
            self.kernel.OpenProcess.restype = ctypes.c_void_p
            self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            self.kernel.WaitForSingleObject.restype = ctypes.c_uint32
            self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]

    def alive(self):
        if self.pid is None:
            try:
                self.pid = int(self.path.read_text(encoding='ascii').strip())
                if self.pid <= 0:
                    return False
            except (OSError, ValueError):
                return time.monotonic() < self.deadline
            if os.name == 'nt':
                self.handle = self.kernel.OpenProcess(0x00100000, 0, self.pid)
                if not self.handle:
                    return False
        if os.name == 'nt':
            return self.kernel.WaitForSingleObject(self.handle, 0) == 0x102
        try:
            os.kill(self.pid, 0)
            return True
        except OSError:
            return False

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def run(request_file, frame_root, source, library, cache, renderer, dotnet=None, parent=None):
    active = None
    active_output = None
    generation = 0
    session = uuid.uuid4().hex
    process = None
    retry_at = 0.0
    pending_capture = None
    revision = renderer_fingerprint(renderer)
    try:
        while True:
            if parent is not None and not parent.alive():
                break
            try:
                text = request_file.read_text(encoding='ascii')
                if text.strip() == 'stop':
                    break
                wanted = parse_request(text)
            except OSError:
                wanted = None
            exited = process is not None and process.poll() is not None
            if exited:
                write_status(frame_root, active, 'error')
                if active_output is not None:
                    active_output[1].unlink(missing_ok=True)
                active_output = None
                pending_capture = None
                process = None
                retry_at = time.monotonic() + 5
            should_retry = wanted is not None and (process is None or (active_output is None and retry_at > 0)) and time.monotonic() >= retry_at
            if wanted != active or should_retry:
                active = wanted
                active_output = None
                generation += 1
                retry_at = 0.0
                if wanted is None:
                    pending_capture = None
                    if process is not None:
                        write_control(frame_root)
                else:
                    if process is not None:
                        write_control(frame_root)
                    pending_capture = None
                    cell, model, form, gender, shiny = selection_fields(wanted)
                    try:
                        bridge_frame = selection_directory(frame_root, wanted) / 'frame.rgba'
                        write_status(frame_root, wanted, 'loading')
                        bridge_frame.unlink(missing_ok=True)
                        model_path, idle_path = prepare_selection(source, library, cache, wanted)
                        # Selection may have changed while the local game data was read.
                        try:
                            if parse_request(request_file.read_text(encoding='ascii')) != wanted:
                                active = None
                                continue
                        except OSError:
                            active = None
                            continue
                        output = frame_root / f'generation-{session}-{generation}'
                        output.mkdir(parents=True, exist_ok=True)
                        stamp = hashlib.sha256(game_stamp(source).encode('ascii')).hexdigest()[:16]
                        base = model_path.stem
                        cached_frame = cache / base / (f'preview-v4-{stamp}-{revision}-form{form}-gender{gender}-shiny{shiny}.rgba')
                        frame = output / 'frame.rgba'
                        if cached_frame.is_file():
                            if copy_frame(cached_frame, bridge_frame):
                                write_status(frame_root, wanted, 'ready')
                        if not cached_frame.is_file():
                            frame.unlink(missing_ok=True)
                            pending_capture = (frame, cached_frame)
                        active_output = (frame, bridge_frame, wanted)
                        control = write_control(frame_root, model_path, idle_path, output, shiny)
                        if process is None:
                            command = ([str(dotnet), str(renderer)] if renderer.suffix.lower() == '.dll' and dotnet else [str(renderer)])
                            process = subprocess.Popen(command + [str(model_path), str(idle_path), str(output), '--worker', str(control)],
                                                       cwd=renderer.parent, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                            print('Model preview worker started', flush=True)
                    except (OSError, ValueError) as exc:
                        write_status(frame_root, wanted, 'error')
                        active_output = None
                        pending_capture = None
                        try:
                            bridge_frame.unlink(missing_ok=True)
                        except OSError:
                            pass
                        print(f'Model {model} form {form} unavailable: {exc}', flush=True)
                        retry_at = time.monotonic() + 5
            if active_output is not None:
                rendered, bridge, selection = active_output
                try:
                    still_selected = parse_request(request_file.read_text(encoding='ascii')) == selection
                except OSError:
                    still_selected = False
                if still_selected and active == selection:
                    if copy_frame(rendered, bridge):
                        write_status(frame_root, selection, 'ready')
                else:
                    active_output = None
                    pending_capture = None
            if pending_capture is not None:
                frame, cached_frame = pending_capture
                if cached_frame.is_file() or copy_frame(frame, cached_frame):
                    pending_capture = None
            time.sleep(0.03)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        if parent is not None:
            parent.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('request', 'frames', 'game', 'ooz', 'cache', 'renderer'):
        parser.add_argument('--' + arg, type=Path, required=True)
    parser.add_argument('--dotnet', type=Path)
    parser.add_argument('--parent-file', type=Path)
    parser.add_argument('--ready-file', type=Path)
    args = parser.parse_args()
    if args.renderer.suffix.lower() == '.dll' and args.dotnet is None:
        parser.error('--dotnet is required for a DLL renderer')
    parent = ParentWatch(args.parent_file) if args.parent_file else None
    if args.ready_file:
        args.ready_file.write_text('model-preview-v2\n', encoding='ascii', newline='\n')
    try:
        run(args.request, args.frames, args.game, args.ooz, args.cache, args.renderer, args.dotnet, parent)
    finally:
        if args.ready_file:
            try:
                args.ready_file.unlink(missing_ok=True)
            except OSError:
                pass



