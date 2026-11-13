"""Prepare locally extracted game icons for the mod; never ship the artwork."""
import hashlib
import json
import os
from pathlib import Path
import subprocess


def prepare(install, game, cache):
    folder = game / 'romfs' / 'arc' / 'data.trpfs'
    source = folder if folder.is_file() else game / 'romfs.bin'
    if not source.is_file(): return None
    helper = install / 'zaprompts' / ('zaprompts.exe' if os.name == 'nt' else 'zaprompts')
    if not helper.is_file(): return None
    st = source.stat()
    identity = {'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'schema': 2}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
    dest = cache / ('party-icons-' + key)
    marker = dest / '.made'
    if marker.is_file() and (dest / 'pm0001_00_00_00_0.png').is_file(): return dest
    dest.mkdir(parents=True, exist_ok=True)
    romfs = game / 'romfs' if folder.is_file() else source
    log = dest / 'extract.log'
    with log.open('wb') as output:
        result = subprocess.run([str(helper), '--romfs', str(romfs), '--party-icons', str(dest)],
                                cwd=str(install), stdout=output, stderr=subprocess.STDOUT,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=180)
    if result.returncode != 0 or not marker.is_file() or not (dest / 'pm0001_00_00_00_0.png').is_file():
        raise RuntimeError('Local game icon extraction did not finish')
    return dest
