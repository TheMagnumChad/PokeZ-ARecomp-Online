import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lumiose'))
import model_preview_launcher as launcher


class FakeProcess:
    def __init__(self, command, **kwargs):
        self.stopped = False
        ready = Path(command[command.index('--ready-file') + 1])
        ready.write_text('ready\n', encoding='ascii')
    def poll(self):
        return 0 if self.stopped else None
    def wait(self, timeout=None):
        self.stopped = True
        return 0
    def terminate(self):
        self.stopped = True


class LauncherTests(unittest.TestCase):
    def test_missing_helper_leaves_launch_environment_unchanged(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            root = Path(folder)
            env = {}
            self.assertIsNone(launcher.start(root, root / 'game', root / 'lumiose', env))
            self.assertNotIn('LUMIOSE_UI_MODEL_ROOT', env)

    def test_ready_helper_attaches_and_stops(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            root = Path(folder)
            game, mod = root / 'game', root / 'lumiose'
            helper = mod / 'model-renderer' / 'UiModelProbe.exe'
            service = root / 'tools' / 'lumiose' / 'model_preview_service.py'
            library = root / 'zaprompts' / '_internal' / ('ooz.dll' if launcher.os.name == 'nt' else 'libooz.so')
            for file in (helper, service, library, game / 'romfs.bin'):
                file.parent.mkdir(parents=True, exist_ok=True)
                file.touch()
            env = {}
            with patch.object(launcher.subprocess, 'Popen', FakeProcess):
                session = launcher.start(root, game, mod, env)
            self.assertIsNotNone(session)
            self.assertEqual(Path(env['LUMIOSE_UI_MODEL_ROOT']), session.request.parent)
            session.attach(12345)
            self.assertEqual(session.parent_file.read_text(encoding='ascii').strip(), '12345')
            session.stop()
            self.assertFalse(session.ready.exists())
            self.assertEqual(session.request.read_text(encoding='ascii').strip(), 'stop')


if __name__ == '__main__':
    unittest.main()
