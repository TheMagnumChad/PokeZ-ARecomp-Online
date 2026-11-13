"""Asset-free service regression tests. Run only after the shared test slot clears."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import model_preview_service as service
from game_ui_models import MissingModelPack


class SelectionSafetyTests(unittest.TestCase):
    def test_v2_identity_and_nonce_bounds(self):
        key = 568 | (3 << 11) | (1 << 17) | (1 << 19)
        self.assertEqual(service.parse_request(f'v2 {key} 7 568 3 1 1'),
                         ('v2', key, 7, 568, 3, 1, 1))
        for text in (f'v2 {key} 0 568 3 1 1', f'v2 {key} 18446744073709551616 568 3 1 1',
                     f'v2 {key} 7 568 2 1 1', f'v2 {key} 7 568 3 0 1',
                     'v2 1048576 1 568 0 0 0', 'v2 0 1 568 0 0 0'):
            self.assertIsNone(service.parse_request(text))
        self.assertEqual(service.parse_request('12 568 0'), (12,568,0,0))
        self.assertEqual(service.parse_request('12 568 0 1'), (12,568,0,1))

    def test_status_namespace_and_nonce(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            selection = ('v2', 568, 99, 568, 0, 0, 0)
            service.write_status(root, selection, 'loading')
            self.assertEqual((root/'v2-568/status.txt').read_bytes(), b'568 99 loading\n')
            service.write_status(root, selection, 'error')
            self.assertEqual((root/'v2-568/status.txt').read_bytes(), b'568 99 error\n')
            self.assertFalse((root/'cell-568').exists())

    def test_renderer_revision_changes_cache_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            renderer = Path(directory)/'UiModelProbe.exe'
            renderer.write_bytes(b'one')
            first = service.renderer_fingerprint(renderer)
            (renderer.parent/'GFTool.Renderer.dll').write_bytes(b'material fix')
            self.assertNotEqual(first, service.renderer_fingerprint(renderer))
            second = service.renderer_fingerprint(renderer)
            (renderer.parent/'Shaders').mkdir()
            (renderer.parent/'Shaders/material.glsl').write_text('material fix')
            self.assertNotEqual(second, service.renderer_fingerprint(renderer))

    def test_dependency_failure_never_changes_form(self):
        with patch.object(service, 'prepare_model_form', side_effect=ValueError('missing sibling')) as prepare:
            with self.assertRaisesRegex(ValueError, 'missing sibling'):
                service.prepare_model(None, None, None, 713, 2)
            self.assertEqual(prepare.call_count, 1)

    def test_only_absent_primary_pack_uses_offset(self):
        result = (Path('pm0713_13_00.trmdl'), Path('idle.tranm'))
        with patch.object(service, 'prepare_model_form', side_effect=[MissingModelPack('absent'), result]) as prepare:
            self.assertEqual(service.prepare_model(None,None,None,713,2), result)
            self.assertEqual([call.args[-1] for call in prepare.call_args_list], [2,13])

    def test_repeated_failed_selection_clears_old_model(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            request = root/'request.txt'
            request.write_text('12 568 0 0')
            old = root/'cell-12/frame.rgba'
            old.parent.mkdir()
            old.write_bytes(b'x' * (192*192*4))
            class Parent:
                calls=0
                def alive(self):
                    self.calls+=1
                    return self.calls<=2
                def close(self): pass
            clock=[0.0]
            def advance(_): clock[0]+=6
            with patch.object(service, 'prepare_model', side_effect=ValueError('missing texture')) as prepare, \
                 patch.object(service.time, 'sleep', side_effect=advance), \
                 patch.object(service.time, 'monotonic', side_effect=lambda:clock[0]), \
                 patch.object(service.subprocess, 'Popen') as spawn:
                service.run(request, root, root/'game', root/'ooz',root/'cache',root/'renderer.exe',parent=Parent())
                self.assertEqual(prepare.call_count,2)
                spawn.assert_not_called()
                self.assertFalse(old.exists())


class PreviewIdleTests(unittest.TestCase):
    def test_default_preferred_and_battle_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);base='pm0714_51_00'
            battle=root/(base+'_20001_battlewait01_loop.tranm');battle.write_bytes(b'test')
            self.assertEqual(service.find_preview_idle(root,base),battle)
            default=root/(base+'_20000_defaultwait01_loop.tranm');default.write_bytes(b'test')
            self.assertEqual(service.find_preview_idle(root,base),default)
    def test_no_arbitrary_action_or_ambiguous_idle(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);base='pm0714_51_00'
            (root/(base+'_20400_attack01.tranm')).write_bytes(b'test')
            self.assertIsNone(service.find_preview_idle(root,base))
            for number in ('20001','20002'):
                (root/(base+'_'+number+'_battlewait01_loop.tranm')).write_bytes(b'test')
            with self.assertRaises(ValueError):service.find_preview_idle(root,base)

if __name__=='__main__': unittest.main()
