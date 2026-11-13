#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export PYTHONDONTWRITEBYTECODE=1
JOBS=${JOBS:-4} tools/za/build-linux.sh
L="$PWD/build/linux/x86_64"
python3.12 -m venv "$L/ui-venv"
"$L/ui-venv/bin/pip" install --disable-pip-version-check --report "$L/ui-helper-install-report.json" -r tools/lumiose/ui-helper-build-requirements.txt
mkdir -p build/ooz
O=third_party/ooz
g++ -O2 -shared -fPIC -Itools/za/ooz_posix -include tools/za/ooz_posix/ooz_posix.h -include sys/stat.h -Dmain=ooz_main "$O/kraken.cpp" "$O/bitknit.cpp" "$O/lzna.cpp" "$O/ooz_lib.cpp" -o build/ooz/libooz.so
"$L/ui-venv/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$L/helper" --workpath "$L/ui-pyinstaller" tools/zaprompts.spec
cp third_party/ooz/COPYING "$L/helper/zaprompts/COPYING-GPLv3.txt"
cp third_party/ooz/README.ZARecomp.md "$L/helper/zaprompts/README-ooz.md"
"$L/helper/zaprompts/zaprompts" --help >/dev/null
python3.12 -m pip install --report "$L/ui-dependency-install-report.json" --target "$L/ui-python-deps" Pillow==13.0.0 fonttools==4.68.0 cffsubr==0.4.0 numpy==2.5.4 texture2ddecoder==1.0.6 capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
PYTHONPATH="$L/ui-python-deps:tools/lumiose" python3.12 -c 'import PIL, fontTools, texture2ddecoder, numpy; import model_preview_launcher as m; e={"LUMIOSE_UI_MODEL_ROOT":"stale"}; assert m.start("/missing","/missing","/missing",e) is None; assert "LUMIOSE_UI_MODEL_ROOT" not in e'
python3.12 - "$L" <<'PY'
import pathlib,shutil,sys
r=pathlib.Path(sys.argv[1]);shutil.copytree('/opt/python3.12',r/'ui-python',symlinks=False)
PY
mkdir -p "$L/ui-runtime-libs"
python3.12 - "$L" <<'PY'
import pathlib,subprocess,shutil,sys
r=pathlib.Path(sys.argv[1]); queue=[r/'za/lasrecomp'];done=set()
while queue:
 for line in subprocess.check_output(['ldd',str(queue.pop())],text=True).splitlines():
  if '=>' not in line:continue
  name=line.split('=>')[0].strip();p=r/'deps/lib'/name
  if p.is_file() and name not in done:
   done.add(name);dest=r/'ui-runtime-libs'/name;shutil.copy2(p.resolve(),dest);subprocess.check_call(['patchelf','--set-rpath','$ORIGIN',str(dest)]);queue.append(dest)
subprocess.check_call(['patchelf','--set-rpath','$ORIGIN/../lib',str(r/'za/lasrecomp')])
PY
python3.12 tools/lumiose/package_linux_ui_overlay.py --root "$PWD" --build "$L" --output build/dist/Lumiose-Online-linux-experimental.zip

PYTHONPATH=tools/lumiose python3.12 - <<'PY'
import pathlib,tempfile,linux_launcher_update as u
archive=pathlib.Path('build/dist/Lumiose-Online-linux-experimental.zip').resolve()
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp)/'existing';root.mkdir()
 for name,data in {'usr/bin/save/main':b'unchanged save','usr/bin/lumiose/subsdk9':b'unchanged local mod','usr/bin/lumiose/config.json':b'unchanged config'}.items():
  p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
 u.install(root,str(archive))
 assert (root/'usr/bin/save/main').read_bytes()==b'unchanged save'
 assert (root/'usr/bin/lumiose/subsdk9').read_bytes()==b'unchanged local mod'
 assert (root/'usr/bin/lumiose/config.json').read_bytes()==b'unchanged config'
 print('PASS existing Linux updater schema and protected-file preservation')
PY
