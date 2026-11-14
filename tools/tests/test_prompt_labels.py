"""Check keyboard prompt labels without requiring game data or the graphics helper."""
import ast
from pathlib import Path
import struct
import unittest

source = (Path(__file__).resolve().parents[1] / 'za' / 'prompt_textures.py').read_text(encoding='utf-8')
tree = ast.parse(source)

class PromptTextureHashTests(unittest.TestCase):
    def test_generated_names_match_renderer_hash(self):
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'fast_hash_full')
        namespace = {'struct': struct, 'M64': (1 << 64) - 1}
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<prompt hash>', 'exec'), namespace)
        data = bytes((i * 37 + 11) & 255 for i in range(257))
        # Reference values from runtime/nvn/tex_hash.h, including both sides of a 64-byte block.
        expected = {0: '371f57b096844ab8', 1: 'f7262b9d2a15e062', 63: '2a18daa892629e3d',
                    64: 'fbb572f371f685e1', 65: 'b677b0876bf90041', 257: '93b81800544e9020'}
        for size, digest in expected.items():
            self.assertEqual(f"{namespace['fast_hash_full'](data[:size]):016x}", digest)

assignments = {'DEFAULTS', 'PAD_DEFAULTS', 'SHORT', 'ARROWS', 'PAD_LABEL', 'DPAD', 'MOUSE_LABEL'}
functions = {'key', 'stick_keys', 'label', 'alias_keyboard_glyphs'}
nodes = [node for node in tree.body
         if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in assignments for t in node.targets)
         or isinstance(node, ast.FunctionDef) and node.name in functions]
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'za'))
from key_glyphs import short_label,LABELS,SCANCODE_NAMES,GLYPH_COUNT
scope = {'short_label':short_label}
exec(compile(ast.Module(body=nodes, type_ignores=[]), '<prompt labels>', 'exec'), scope)


class PromptLabels(unittest.TestCase):
    def setUp(self):
        scope['binds'] = dict(scope['DEFAULTS'])
        scope['pad_binds'] = dict(scope['PAD_DEFAULTS'])
        scope['mouse_camera'] = 'captured'
        scope['mouse_binds'] = {'left': 'zr', 'right': 'aim', 'middle': 'party_toggle', 'x1': 'none', 'x2': 'none'}

    def test_keyboard_labels_match_context_and_rebinding(self):
        label = scope['label']
        self.assertEqual(label('keyboard', 'a', 'field'), 'E')
        self.assertEqual(label('keyboard', 'b', 'field'), 'LShift')
        self.assertEqual(label('keyboard', 'b', 'menu'), 'Esc')
        self.assertEqual(label('keyboard', 'b', 'aim'), 'Esc')
        self.assertEqual(label('keyboard', 'x', 'field'), 'Esc')
        self.assertEqual(label('keyboard', 'x', 'menu'), 'I')
        self.assertEqual(label('keyboard', 'b', 'bag'), 'Esc')
        self.assertEqual(label('keyboard', 'l', 'bag'), 'A')
        self.assertEqual(label('keyboard', 'r', 'bag'), 'D')
        self.assertEqual(label('keyboard', 'b', 'battle'), '2')
        self.assertEqual(label('keyboard', 'a', 'battle'), '1')
        self.assertEqual(label('keyboard', 'zr', 'field'), 'RMB')
        self.assertEqual(label('keyboard', 'zr', 'aim'), 'LMB')
        self.assertEqual(label('keyboard', 'plus', 'battle'), 'R')
        self.assertEqual(label('keyboard', 'up', 'field'), 'MMB')
        self.assertEqual(label('keyboard', 'down', 'field'), 'MMB')
        self.assertEqual(label('keyboard', 'l', 'box'), 'WheelUp')
        self.assertEqual(label('keyboard', 'r', 'box'), 'WheelDown')
        scope['binds']['cancel'] = 'Backspace'
        self.assertEqual(label('keyboard', 'b', 'menu'), 'Bksp')
        self.assertEqual(label('keyboard', 'b', 'aim'), 'Bksp')
        self.assertEqual(label('keyboard', 'x', 'field'), 'Bksp')
        scope['binds']['move_b'] = 'none'
        self.assertEqual(label('keyboard', 'b', 'battle'), 'Unbound')
        scope['mouse_binds']['right'] = 'none'
        self.assertEqual(label('keyboard', 'zr', 'field'), 'Unbound')
        self.assertEqual(label('keyboard', 'rs'), 'Mouse')
        scope['mouse_camera'] = 'hold'
        self.assertEqual(label('keyboard', 'rs'), 'Hold RMB')
        scope['mouse_camera'] = 'off'
        self.assertEqual(label('keyboard', 'rs'), 'Num8/Num4/Num5/Num6')

    def test_all_keys_and_mouse_controls_have_distinct_labels(self):
        self.assertEqual(len(LABELS),GLYPH_COUNT)
        self.assertTrue(all(LABELS[i] for i in range(GLYPH_COUNT)))
        self.assertNotEqual(short_label('Left Shift'),short_label('Right Shift'))
        self.assertNotEqual(short_label('Keypad 1'),short_label('1'))
        scope['binds']['move_a']='F12'
        self.assertEqual(scope['label']('keyboard','a','battle'),'F12')
        scope['binds']['move_a']='Keypad 7'
        self.assertEqual(scope['label']('keyboard','a','battle'),'Num7')
        scope['mouse_binds']['x1']='a'
        self.assertEqual(scope['label']('keyboard','a','menu'),'Mouse4')

    def test_xbox_respects_bindings(self):
        label = scope['label']
        self.assertEqual(label('xbox', 'a'), 'B')
        scope['pad_binds']['a'] = 'south'
        self.assertEqual(label('xbox', 'a'), 'A')

    def test_keyboard_directional_sticks_share_relabelled_glyphs(self):
        class Table:
            cmap = {0xE335: 'left', 0xE336: 'right', 0xE33C: 'old1', 0xE33D: 'old2',
                    0xE33E: 'old3', 0xE33F: 'old4'}
            def isUnicode(self): return True
        table = Table()
        class Font:
            def __getitem__(self, key):
                self.assert_key = key
                return type('Cmap', (), {'tables': [table]})()
            def getBestCmap(self): return table.cmap
        self.assertEqual(scope['alias_keyboard_glyphs'](Font()), {0xE33C, 0xE33D, 0xE33E, 0xE33F})
        self.assertEqual(table.cmap[0xE33C], 'left')
        self.assertEqual(table.cmap[0xE33F], 'right')


if __name__ == '__main__':
    unittest.main()
