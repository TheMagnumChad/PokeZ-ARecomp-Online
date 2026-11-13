import unittest
from unittest.mock import patch
import game_ui_models as m
class SharedMaterialTests(unittest.TestCase):
 def test_game_path(self):
  self.assertEqual(m.shared_texture_reference('ik_pokemon/data/pm0001/pm0001_00_00','../../../share/specular_prb/poke_metal_default_prb/poke_metal_default_prb.bntx'),'ik_pokemon/share/specular_prb/poke_metal_default_prb/poke_metal_default_prb.bntx')
 def test_reject_paths(self):
  for ref in ['/outside.bntx','C:/outside.bntx','..\\outside.bntx','../../../../outside.bntx','../../../share/../../../outside.bntx','../../../share/a.bntx/../../b.bntx','../../../share/'+('x'*241)+'.bntx']:
   with self.subTest(ref=ref):
    with self.assertRaises(ValueError):m.shared_texture_reference('ik_pokemon/data/pm0001/pm0001_00_00',ref)
 def test_exact_member(self):
  folder='ik_pokemon/data/pm0001/pm0001_00_00';ref='../../../share/test/test.bntx';path=m.shared_texture_reference(folder,ref)
  with patch.object(m,'read_members',return_value=[(m.fnv64(path),b'BNTXtest')]):
   result=m.read_shared_textures(None,folder,[(1,ref.encode())],None)
   self.assertEqual(result,[(path,'test.bntx',b'BNTXtest')])
 def test_wrong_member(self):
  with patch.object(m,'read_members',return_value=[(0,b'BNTXtest')]):
   with self.assertRaises(ValueError):m.read_shared_textures(None,'ik_pokemon/data/pm0001/pm0001_00_00',[(1,b'../../../share/test/test.bntx')],None)
 def test_budget(self):
  refs=b'\0'.join(f'../../../share/a{i}/a{i}.bntx'.encode() for i in range(65))
  with self.assertRaises(ValueError):m.shared_texture_requests('ik_pokemon/data/pm0001/pm0001_00_00',[(1,refs)])
class ModelCacheVersionTests(unittest.TestCase):
 def test_old_model_cache_rebuilt_once(self):
  import tempfile, json
  from pathlib import Path
  import model_preview_service as service
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp); source=root/'romfs.bin'; source.write_bytes(b'synthetic')
   dest=root/'pm0001_00_00'; named=dest/'named';named.mkdir(parents=True)
   (named/'pm0001_00_00.trmdl').write_bytes(b'synthetic')
   (named/'pm0001_00_00_00000_defaultwait01_loop.tranm').write_bytes(b'synthetic')
   (dest/'source.json').write_text(json.dumps({'game':service.game_stamp(source)}))
   with patch.object(service,'extract') as extract:
    service.prepare_model_form(source,root/'ooz.dll',root,1,0)
    self.assertEqual(extract.call_count,1)
    service.prepare_model_form(source,root/'ooz.dll',root,1,0)
    self.assertEqual(extract.call_count,1)
if __name__=='__main__':unittest.main()
