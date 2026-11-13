import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import game_ui_font as font


def encoded(raw):
    n = bytes(a ^ b for a, b in zip(len(raw).to_bytes(4, 'big'), font.KEY))
    return bytes.fromhex('36f81a1e') + n + bytes(b ^ font.KEY[i % 4] for i, b in enumerate(raw))


class GameUiFontTest(unittest.TestCase):
    def test_decode_requires_game_font_identity(self):
        raw = b'OTTO' + font.FONT_NAME + bytes(1200)
        self.assertEqual(font.font_from_bfttf(encoded(raw)), raw)
        self.assertIsNone(font.font_from_bfttf(encoded(b'OTTO' + bytes(1200))))
        self.assertIsNone(font.font_from_bfttf(encoded(raw)[:-1]))

    def test_cache_tracks_the_players_game_data(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            game, cache = root / 'extracted', root / 'cache'
            game.mkdir()
            source = game / 'romfs.bin'
            source.write_bytes(b'first')
            lib = root / 'zaprompts/_internal/ooz.dll'
            lib.parent.mkdir(parents=True)
            lib.touch()
            with patch.object(font, 'extract_font', side_effect=[b'OTTO' + bytes(1100), b'OTTO' + bytes(1200)]) as extract:
                path = font.prepare(root, game, cache)
                self.assertEqual(path.stat().st_size, 1104)
                self.assertEqual(font.prepare(root, game, cache), path)
                self.assertEqual(extract.call_count, 1)
                source.write_bytes(b'second game image')
                os.utime(source, None)
                self.assertEqual(font.prepare(root, game, cache).stat().st_size, 1204)
                self.assertEqual(extract.call_count, 2)

    def test_missing_assets_fall_back(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            self.assertIsNone(font.prepare(root, root / 'game', root / 'cache'))

    def test_corrupt_cache_is_regenerated(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            game, cache = root / 'game', root / 'cache'
            game.mkdir()
            (game / 'romfs.bin').write_bytes(b'game data')
            lib = root / 'zaprompts/_internal/ooz.dll'
            lib.parent.mkdir(parents=True)
            lib.touch()
            with patch.object(font, 'extract_font', return_value=b'OTTO' + bytes(1100)) as extract:
                path = font.prepare(root, game, cache)
                path.write_bytes(b'damaged')
                self.assertEqual(font.prepare(root, game, cache).stat().st_size, 1104)
                self.assertEqual(extract.call_count, 2)


if __name__ == '__main__': unittest.main()
