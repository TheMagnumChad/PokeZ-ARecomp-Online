"""Build the Windows release folder from an explicit, game-free allowlist."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DLLS = ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'avcodec-63.dll',
        'avformat-63.dll', 'avutil-61.dll', 'libgcc_s_seh-1.dll', 'libiconv-2.dll',
        'libshaderc_shared.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'SDL3.dll', 'swscale-10.dll')


def copy_file(src, dest):
    if not src.is_file(): raise RuntimeError(f'Required support file missing: {src}')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--support', type=Path, required=True, help='Clean ZA Recomp release support tools')
    p.add_argument('--python', type=Path, required=True, help='Portable CPython 3.12 folder')
    p.add_argument('--deps', type=Path, required=True, help='Python dependencies (unicorn, capstone, lz4)')
    p.add_argument('--lasgen', type=Path, help='Freshly built generator folder (otherwise use the support folder)')
    p.add_argument('--build-dir', type=Path, default=ROOT / 'build/win-lumiose-release')
    p.add_argument('--zaprompts', type=Path)
    p.add_argument('--out', type=Path, required=True, help='Empty destination folder')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()): raise RuntimeError('Destination must be empty')
    build = a.build_dir
    prompt_dir = a.zaprompts if a.zaprompts else a.support / 'zaprompts'
    check = subprocess.run([str(prompt_dir / 'zaprompts.exe'), '--prompt-schema'], capture_output=True, text=True, timeout=30)
    if check.returncode or check.stdout.strip() != 'ZAPromptSchema 17':
        raise RuntimeError('Rebuild the current prompt helper and pass --zaprompts')
    for n in ('lasrecomp.exe', 'Lumiose Online.exe', 'lasrecomp.dll.a'):
        copy_file(build / n, out / n)
    copy_file(build / 'lasrecomp.dll.a', out / 'build/lasrecomp.dll.a')
    for n in DLLS: copy_file(a.support / n, out / n)
    for n in ('LICENSE', 'README.md', 'Baloo2-SemiBold.ttf', 'Baloo2-OFL.txt', 'AMD-FSR-LICENSE.md'):
        if (a.support / n).is_file(): copy_file(a.support / n, out / n)
    copy_file(ROOT / 'README.md', out / 'ZA-Recomp-README.md')
    copy_file(ROOT / 'LICENSE', out / 'ZA-Recomp-LICENSE.txt')
    copy_file(ROOT / 'docs/PC-CONTROLS.md', out / 'docs/PC-CONTROLS.md')
    for n in ('DejaVuSans.ttf', 'DejaVuSans-Bold.ttf'):
        copy_file(ROOT / 'third_party/fonts/DejaVu' / n, out / n)
    copy_file(ROOT / 'third_party/fonts/DejaVu/LICENSE', out / 'DejaVu-LICENSE.txt')
    copy_file(ROOT / 'third_party/rnnoise/COPYING', out / 'RNNOISE-LICENSE.txt')
    copy_file(ROOT / 'assets/lumiose_icon.png', out / 'icon.png')
    for n in ('zig', 'lasgen', 'zaimport', 'zaprompts', 'dx12', 'D3D12'):
        src = a.lasgen if n == 'lasgen' and a.lasgen else prompt_dir if n == 'zaprompts' else a.support / n
        if not src.is_dir(): raise RuntimeError(f'Required support folder missing: {src}')
        shutil.copytree(src, out / n)
    for n in ('dx12/dzn_icd.x86_64.json', 'dx12/vulkan_dzn.dll', 'dx12/Mesa-LICENSE.rst',
              'dx12/D3D12-Agility-LICENSE.txt', 'D3D12/D3D12Core.dll'):
        if not (out / n).is_file(): raise RuntimeError(f'Required DirectX support file missing: {n}')
    for n in ('cpu.h', 'simd.h', 'simd_fp.h', 'gamelib.h'):
        copy_file(ROOT / 'runtime' / n, out / 'gamelib-src' / n)
    copy_file(ROOT / 'tools/gamelib_entry.c', out / 'gamelib-src/gamelib_entry.c')
    copy_file(ROOT / 'runtime/hle/hle_symbols.txt', out / 'gamelib-src/hle_symbols.txt')
    shutil.copytree(ROOT / 'runtime', out / 'runtime', ignore=shutil.ignore_patterns('__pycache__'))
    for src in (ROOT / 'tools').glob('*.py'): copy_file(src, out / 'tools' / src.name)
    for n in ('gamelib_entry.c', 'lumiose_test_entry.c'):
        copy_file(ROOT / 'tools' / n, out / 'tools' / n)
    copy_file(ROOT / 'tools/rando/patch_variants.json', out / 'tools/rando/patch_variants.json')
    shutil.copytree(ROOT / 'tools/lumiose', out / 'tools/lumiose', ignore=shutil.ignore_patterns('__pycache__', '*.subsdk9', 'prepackaged.subsdk9'))
    # Exact mod/reference shipped in v0.1.27; never accept arbitrary NSOs.
    approved_mods = {'lumiose/subsdk9': 'e304bf4fa710be688f5a50dc9b6210b8dbdb4671e7b9ea5313c63ed3e17f69e2',
                     'tools/lumiose/prepackaged.subsdk9': 'e7aaea520e8074763c047e9bab7a0de2ae79ed4953fcc7a91c0f36469411e5f9'}
    for name, digest in approved_mods.items():
        src = ROOT / name
        if hashlib.sha256(src.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'Unexpected bundled mod bytes: {name}')
        copy_file(src, out / name)
    for n in ('lumiose/model-renderer', 'THIRD-PARTY-NOTICES'):
        src = a.support / n
        if not src.is_dir(): raise RuntimeError(f'Required renderer/notices missing: {src}')
        shutil.copytree(src, out / n)
    for n in ('python.exe', 'python312.dll', 'python3.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt'):
        copy_file(a.python / n, out / 'python' / n)
    shutil.copytree(a.python / 'DLLs', out / 'python/DLLs')
    shutil.copytree(a.python / 'Lib', out / 'python/Lib', ignore=shutil.ignore_patterns(
        'site-packages', '__pycache__', 'test', 'tests', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'venv'))
    shutil.copytree(a.deps, out / 'python-deps', ignore=shutil.ignore_patterns('__pycache__'))
    copy_file(ROOT / 'LUMIOSE-RELEASE.md', out / 'READ ME - Lumiose Online.md')
    forbidden = {'.nsp', '.xci', '.nca', '.nso'}
    for f in out.rglob('*'):
        if not f.is_file(): continue
        if f.suffix.lower() in forbidden or f.name.lower() in {'main', 'main.npdm', 'sdk', 'rtld', 'romfs.bin', 'prod.keys', 'lasgame.dll'}:
            raise RuntimeError(f'Game file unexpectedly present: {f}')
        with f.open('rb') as stream: magic = stream.read(4)
        if magic == b'NSO0' and f.relative_to(out).as_posix() not in approved_mods:
            raise RuntimeError(f'Unexpected NSO module: {f}')
    print(f'Packaged {sum(1 for f in out.rglob("*") if f.is_file())} files; only exact v0.1.27 mod/reference NSOs are included; no game modules.')


if __name__ == '__main__':
    try: main()
    except Exception as e:
        print(f'Packaging failed: {e}', file=sys.stderr)
        sys.exit(1)
