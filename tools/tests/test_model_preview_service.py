import sys
import unittest
import os
import subprocess
import tempfile
import threading
import time
from unittest.mock import patch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lumiose'))
from model_preview_service import ParentWatch, parse_request, write_control, copy_frame, prepare_model, run


class RequestTests(unittest.TestCase):
    def test_form_eleven_is_used_when_base_form_is_missing(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            root = Path(folder)
            game = root/'game'
            game.write_bytes(b'game')
            def extract(source, library, dest, model, form):
                if form == 0:
                    raise ValueError('pack not found')
                named=dest/'named'
                named.mkdir(parents=True)
                (named/'pm0770_11_00.trmdl').touch()
                (named/'pm0770_11_00_00000_defaultwait01_loop.tranm').touch()
            with patch('model_preview_service.extract', side_effect=extract):
                model, idle = prepare_model(game, root/'ooz', root/'cache', 770, 0)
                self.assertEqual(model.name, 'pm0770_11_00.trmdl')
                self.assertTrue(idle.is_file())

    def test_alternative_idle_number_is_supported(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            root = Path(folder)
            game = root/'game'
            game.write_bytes(b'game')
            def extract(source, library, dest, model, form):
                named=dest/'named'
                named.mkdir(parents=True)
                (named/'pm0709_00_00.trmdl').touch()
                (named/'pm0709_00_00_10000_defaultwait01_loop.tranm').touch()
            with patch('model_preview_service.extract', side_effect=extract) as extract_mock:
                _, idle = prepare_model(game, root/'ooz', root/'cache', 709, 0)
                self.assertIn('_10000_defaultwait01_loop.tranm', idle.name)
                prepare_model(game, root/'ooz', root/'cache', 709, 0)
                self.assertEqual(extract_mock.call_count, 1)

    def test_cached_frame_requires_a_complete_image(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            source, target = Path(folder)/'in', Path(folder)/'out'
            source.write_bytes(b'incomplete')
            self.assertFalse(copy_frame(source, target))
            self.assertFalse(target.exists())
            source.write_bytes(b'\x7f' * (192*192*4))
            self.assertTrue(copy_frame(source, target))
            self.assertEqual(target.read_bytes(), source.read_bytes())

    def test_control_selection_and_pause(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            root = Path(folder)
            control = write_control(root, 'model', 'idle', 'output')
            self.assertEqual(control.read_bytes(), b'model\nidle\noutput\n0\n')
            self.assertEqual(write_control(root, 'model', 'idle', 'output', True).read_bytes(), b'model\nidle\noutput\n1\n')
            self.assertEqual(write_control(root).read_bytes(), b'pause\n')

    def test_worker_is_reused_and_paused(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            root = Path(folder)
            request = root / 'request.txt'
            (root / 'game').write_bytes(b'game')
            request.write_text('0 1 0\n', encoding='ascii')
            process = unittest.mock.Mock()
            process.poll.return_value = None
            def prepare(source, library, cache, model, form):
                return root / f'{model}.trmdl', root / f'{model}.tranm'
            with patch('model_preview_service.prepare_model', side_effect=prepare), \
                 patch('model_preview_service.subprocess.Popen', return_value=process) as spawn:
                thread = threading.Thread(target=run, args=(request, root, root/'game', root/'ooz', root/'cache', root/'worker.exe'))
                thread.start()
                def await_control(value):
                    deadline = time.monotonic() + 4
                    while time.monotonic() < deadline:
                        try:
                            if value in (root/'renderer-control.txt').read_text(encoding='utf-8'):
                                return
                        except OSError:
                            pass
                        time.sleep(.02)
                    self.fail('Renderer did not receive ' + value)
                try:
                    await_control('1.trmdl')
                    request.write_text('5 4 0\n', encoding='ascii')
                    await_control('4.trmdl')
                    request.write_text('', encoding='ascii')
                    await_control('pause')
                    self.assertEqual(spawn.call_count, 1)
                    process.terminate.assert_not_called()
                finally:
                    request.write_text('stop\n', encoding='ascii')
                    thread.join(5)
                    self.assertFalse(thread.is_alive())

    def test_valid_selection(self):
        self.assertEqual(parse_request('0 1 0\n'), (0, 1, 0, 0))
        self.assertEqual(parse_request('24 25 1'), (24, 25, 1, 0))
        self.assertEqual(parse_request('32768 1 0 1'), (32768, 1, 0, 1))

    def test_incomplete_or_invalid_selection(self):
        for text in ('', '0 1', '0 1 0 8', '-1 1 0', '1 -1 0', '1 1 100', 'bad 1 0'):
            with self.subTest(text=text):
                self.assertIsNone(parse_request(text))

    def test_parent_handle_tracks_a_live_process(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            path = Path(folder) / 'parent.txt'
            path.write_text(str(os.getpid()), encoding='ascii')
            watch = ParentWatch(path)
            try:
                self.assertTrue(watch.alive())
                self.assertTrue(watch.alive())
            finally:
                watch.close()

    def test_parent_handle_reports_process_exit(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as folder:
            child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
            try:
                path = Path(folder) / 'parent.txt'
                path.write_text(str(child.pid), encoding='ascii')
                watch = ParentWatch(path)
                try:
                    self.assertTrue(watch.alive())
                    child.terminate()
                    child.wait(timeout=5)
                    self.assertFalse(watch.alive())
                finally:
                    watch.close()
            finally:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
