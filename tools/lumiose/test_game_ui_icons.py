from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import game_ui_icons as icons


class GameUiIconsTest(unittest.TestCase):
    def test_extracts_once_per_game_revision(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game, cache = root / 'game', root / 'cache'
            game.mkdir()
            source = game / 'romfs.bin'
            source.write_bytes(b'first game')
            helper = root / 'zaprompts' / ('zaprompts.exe' if icons.os.name == 'nt' else 'zaprompts')
            helper.parent.mkdir()
            helper.touch()
            def extract(args, **kwargs):
                dest = Path(args[-1])
                (dest / '.made').write_text('1')
                (dest / 'pm0001_00_00_00_0.png').write_bytes(b'local icon')
                return type('Result', (), {'returncode': 0})()
            with patch.object(icons.subprocess, 'run', side_effect=extract) as run:
                first = icons.prepare(root, game, cache)
                self.assertEqual(icons.prepare(root, game, cache), first)
                self.assertEqual(run.call_count, 1)
                source.write_bytes(b'a different game revision')
                self.assertNotEqual(icons.prepare(root, game, cache), first)
                self.assertEqual(run.call_count, 2)

    def test_missing_helper_is_optional(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game = root / 'game'
            game.mkdir()
            (game / 'romfs.bin').write_bytes(b'game')
            self.assertIsNone(icons.prepare(root, game, root / 'cache'))


if __name__ == '__main__': unittest.main()
