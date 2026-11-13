"""Asset-free Linux UI package regression checks."""
import tempfile
import unittest
import zipfile
from pathlib import Path
import package_linux_ui_overlay as overlay

class LinuxOverlayTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root, self.build = self.base / 'source', self.base / 'build'
        self.output = self.base / 'update.zip'
        def put(path, data):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.put = put
        for name in overlay.HELPERS:
            put(self.root / 'tools/lumiose' / name, b'# fixture\n')
        put(self.root / 'LICENSE', b'fixture license')
        put(self.build / 'za/lasrecomp', b'\x7fELF fixture')
        for part in ('helper/zaprompts', 'ui-python-deps', 'ui-runtime-libs', 'ui-python'):
            put(self.build / part / 'fixture.txt', b'fixture')

    def test_known_pip_launchers_omitted_and_metadata_retained(self):
        directory = self.build / 'ui-python/lib/python3.12/site-packages/pip/_vendor/distlib'
        for name in overlay.WINDOWS_PIP_LAUNCHERS:
            self.put(directory / name, b'MZ synthetic launcher')
        self.put(directory / '__init__.py', b'# needed pip module')
        overlay.package(self.root, self.build, self.output)
        with zipfile.ZipFile(self.output) as archive:
            names = archive.namelist()
            self.assertTrue(any(name.endswith('/distlib/__init__.py') for name in names))
            self.assertFalse(any(name.endswith('.exe') for name in names))
            self.assertIn('Lumiose Online Linux/Lumiose Online', names)
            self.assertIn('Lumiose Online Linux/usr/bin/lasrecomp', names)

    def test_unknown_pe_still_rejected(self):
        self.put(self.build / 'ui-python/unexpected.exe', b'MZ synthetic executable')
        with self.assertRaisesRegex(RuntimeError, 'Not Linux code/dependency'):
            overlay.package(self.root, self.build, self.output)

    def test_same_filename_outside_pip_still_rejected(self):
        self.put(self.build / 'ui-python/t32.exe', b'MZ synthetic executable')
        with self.assertRaisesRegex(RuntimeError, 'Not Linux code/dependency'):
            overlay.package(self.root, self.build, self.output)

    def test_prohibited_module_still_rejected(self):
        self.put(self.build / 'ui-python/subsdk9', b'synthetic sentinel')
        with self.assertRaisesRegex(RuntimeError, 'Prohibited input'):
            overlay.package(self.root, self.build, self.output)

if __name__ == '__main__':
    unittest.main()
