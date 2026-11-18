"""Exercise the actual Linux package's parent imports with clean bundled Python."""

from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(root / 'tools/lumiose'))
import entry
import lz4.block
import capstone
import unicorn
import native_compat
import update_compat

assert entry.LAUNCHER_VERSION == 'v0.1.38'
for module in (lz4.block, capstone, unicorn):
    assert Path(module.__file__).resolve().is_relative_to(root / 'python-deps')
candidate = root / 'tools/lumiose/prepackaged.subsdk9'
module = update_compat._module(candidate)
profile = native_compat.discover_candidate(module, candidate, reference=root / 'lumiose/subsdk9')
assert profile['build_id'] == '0124d3f6a48e67fefaa3ab4145bf24c26c42c3ff'
assert profile['module_kind'] == 'legacy' and len(profile['camera_query_returns']) == 2
assert native_compat.runtime_profile_bytes(profile).startswith(b'LCP2')
print('Bundled Python parent imports and raw build-8 compatibility discovery passed on Linux.')
