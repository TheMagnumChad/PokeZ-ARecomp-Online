"""Package fresh Linux build outputs as a existing-install UI update compatible with the Linux updater. No mod/game inputs."""
import argparse, hashlib, json, os, stat, zipfile
from pathlib import Path
HELPERS = ('auto_launch.py', 'auto_prepare.py', 'build_module.py', 'build_patched_game.py', 'entry.py', 'fast_retarget.py', 'find_hook_entries.py', 'game_ui_font.py', 'game_ui_icons.py', 'game_ui_items.py', 'game_ui_model_catalog.py', 'game_ui_models.py', 'generate_patched.py', 'linux_launcher_update.py', 'model_preview_launcher.py', 'model_preview_service.py', 'optimize_camera_139.py', 'platform_paths.py', 'prepare_patched_modules.py', 'preserve_name_139.py', 'setup.py', 'switch_tables.py')
FORBIDDEN = {'.nso','.nsp','.xci','.nca','.trmdl','.trmsh','.trmbf','.trskl','.trmtr','.tracm','.tranm','.bntx'}
def package(root, build, output):
    root,build,output=map(Path,(root,build,output)); files={}
    def add(path,name):
        if path.is_symlink():
            path=path.resolve(strict=True)
        resolved = path.resolve(strict=True)
        if not (resolved.is_relative_to(root.resolve()) or resolved.is_relative_to(build.resolve())):
            raise RuntimeError('Input link escapes authorized source/build roots')
        if not path.is_file(): raise RuntimeError('Missing input: '+str(path))
        if Path(name).suffix.lower() in FORBIDDEN or Path(name).name in {'subsdk9','lasgame.so','lumiose.so'}: raise RuntimeError('Prohibited input')
        data=path.read_bytes()
        if data[:4] in (b'NSO0',b'BNTX',b'PFS0',b'HFS0') or data[:2]==b'MZ': raise RuntimeError('Not Linux code/dependency: '+name)
        if name in files: raise RuntimeError('Duplicate input')
        files[name]=(data,path.stat().st_mode)
    runtime=build/'za/lasrecomp'
    if runtime.read_bytes()[:4]!=b'\x7fELF': raise RuntimeError('Expected independently built Linux ELF')
    add(runtime,'usr/bin/lasrecomp')
    for n in HELPERS:add(root/'tools/lumiose'/n,'usr/bin/tools/lumiose/'+n)
    for source,prefix in ((build/'helper/zaprompts','usr/bin/zaprompts'),(build/'ui-python-deps','usr/bin/python-deps'),(build/'ui-runtime-libs','usr/lib'),(build/'ui-python','usr/bin/python')):
        if not source.is_dir():raise RuntimeError('Missing fresh build dependencies')
        for p in sorted(source.rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts and p.suffix!='.pyc':add(p,prefix+'/'+p.relative_to(source).as_posix())
    add(root/'LICENSE','usr/bin/docs/UI-UPDATE-LICENSE.txt')
    starter = "#!/bin/sh\nset -eu\nHERE=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\nexport LD_LIBRARY_PATH=\"$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\nexport PYTHONPATH=\"$HERE/usr/bin/python-deps${PYTHONPATH:+:$PYTHONPATH}\"\nexec \"$HERE/usr/bin/python/bin/python3.12\" \"$HERE/usr/bin/tools/lumiose/entry.py\" \"$@\"\n"
    files['Lumiose Online']=(starter.encode(),0o755)
    text=('LINUX UI COMPATIBILITY UPDATE: EXISTING INSTALLATIONS ONLY\n'
          'Close the game and launcher. Extract this overlay into the existing Lumiose Online Linux folder, merging usr/.\n'
          'Includes fresh portablePython3.12; requires existing compiler/importer, local game and separately obtained compatible mod.\n'
          'Preserve saves, settings and mod files. This archive contains no mod NSO, game code/assets or generated game libraries.\n'
          'Icons, fonts and UI textures are extracted locally. Animated3D models are unavailable on Linux in this update; use Icons.\n'
          'Native build/asset-free tests are not AMD/SteamDeck gameplay validation. Not a fresh installer; compatible with the existing Linux updater.\n')
    files['READ ME.txt']=(text.encode(),0o644)
    manifest={'kind':'linux-existing-install-ui-overlay','animated_models':False,'files':[{'path':n,'sha256':hashlib.sha256(d).hexdigest(),'bytes':len(d)} for n,(d,m) in sorted(files.items())]}
    files['usr/bin/docs/LINUX-UI-UPDATE-MANIFEST.json']=(json.dumps(manifest,indent=2).encode(),0o644)
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as z:
        for n,(data,mode) in sorted(files.items()):
            info=zipfile.ZipInfo('Lumiose Online Linux/'+n);info.create_system=3;info.compress_type=zipfile.ZIP_DEFLATED;info.external_attr=(stat.S_IFREG|(mode&0o777))<<16;z.writestr(info,data)
    with zipfile.ZipFile(output) as z:
        for item in manifest['files']:assert hashlib.sha256(z.read('Lumiose Online Linux/'+item['path'])).hexdigest()==item['sha256']
    print(json.dumps({'archive':str(output),'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'files':len(files)}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--root',required=True);p.add_argument('--build',required=True);p.add_argument('--output',required=True);a=p.parse_args();package(a.root,a.build,a.output)
