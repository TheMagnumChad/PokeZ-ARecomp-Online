import hashlib
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch
import auto_prepare as a

class NativeProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'module'
        self.path.write_bytes(b'synthetic-module-only')
        self.m = types.SimpleNamespace(build_id='test', image=bytearray(128), rodata=(64,32),
            syms=[('',0,0,0,0),('vmp_recomp_camera_find',0x22,0,0,0),('vmp_recomp_name_ready',0x22,0,0,0)],
            relocs=[(64,1025,1,0),(72,1025,2,0)])
        self.m.image[8:12] = b'test'
        self.profile = {'sha256':hashlib.sha256(self.path.read_bytes()).hexdigest(),
            'witnesses':[(8,b'test'.hex())], 'name_preservation_verified':True}
        self.guard = patch.dict(a.NATIVE_PROFILES, {'test':self.profile}, clear=True)
        self.guard.start(); self.addCleanup(self.guard.stop)
    def test_valid(self): self.assertIs(a.native_profile(self.m,self.path),self.profile)
    def test_unknown(self):
        self.m.build_id='unknown'; self.assertIsNone(a.native_profile(self.m,self.path))
    def test_wrong_hash(self):
        self.path.write_bytes(b'changed')
        with self.assertRaises(RuntimeError): a.native_profile(self.m,self.path)
    def test_missing_import(self):
        self.m.syms.pop()
        with self.assertRaises(RuntimeError): a.native_profile(self.m,self.path)
    def test_ambiguous_import(self):
        self.m.syms.append(self.m.syms[1])
        with self.assertRaises(RuntimeError): a.native_profile(self.m,self.path)
    def test_bad_relocation(self):
        self.m.relocs[0]=(128,1025,1,0)
        with self.assertRaises(RuntimeError): a.native_profile(self.m,self.path)
    def test_changed_instruction(self):
        self.m.image[8]=0
        with self.assertRaises(RuntimeError): a.native_profile(self.m,self.path)
    def test_name_unverified(self):
        self.profile['name_preservation_verified']=False
        with self.assertRaises(RuntimeError): a.native_profile(self.m,self.path)
if __name__=='__main__': unittest.main()
