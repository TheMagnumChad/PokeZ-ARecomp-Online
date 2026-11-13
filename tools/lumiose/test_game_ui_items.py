import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch
import game_ui_items as items

class ItemAssetsTest(unittest.TestCase):
    def test_png_converts_bgra_and_keeps_alpha(self):
        data = items.png_bgra(2, 1, bytes([3, 2, 1, 4, 30, 20, 10, 40]))
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        pos = 8
        chunks = {}
        while pos < len(data):
            size = struct.unpack_from('>I', data, pos)[0]
            kind = data[pos+4:pos+8]; chunks[kind] = data[pos+8:pos+8+size]; pos += 12+size
        self.assertEqual(zlib.decompress(chunks[b'IDAT']), bytes([0,1,2,3,4,10,20,30,40]))

    def test_missing_game_or_decoder_is_optional(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            self.assertFalse(items.prepare(root, root, root/'cache'))

    def test_current_cache_avoids_extraction(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); game = root/'game'; game.mkdir(); source=game/'romfs.bin'; source.write_bytes(b'game')
            dest=root/'cache'; dest.mkdir(); (dest/'item_0004.png').write_bytes(b'local')
            internal=root/'zaprompts'/'_internal'; internal.mkdir(parents=True); (internal/('ooz.dll' if items.os.name=='nt' else 'libooz.so')).touch()
            st=source.stat(); (dest/'.items-made').write_text(f'{st.st_size}:{st.st_mtime_ns}:1')
            with patch.object(items.ctypes, 'CDLL', side_effect=AssertionError('cache should be used')):
                self.assertTrue(items.prepare(root, game, dest))

if __name__ == '__main__': unittest.main()
