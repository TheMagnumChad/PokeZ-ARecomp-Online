"""Synthetic, asset-free tests for the local identity resource selector."""
import struct
import unittest
from pathlib import Path
from unittest.mock import patch
import game_ui_model_catalog as catalog
import model_preview_service as service


def fixture(identities=((101,7,1),)):
    data=bytearray(4)
    def align():
        while len(data)%4:data.append(0)
    def table(offsets,size):
        align();vt=len(data);data.extend(struct.pack('<HH',4+2*len(offsets),size))
        for off in offsets:data.extend(struct.pack('<H',off))
        align();pos=len(data);data.extend(bytes(size));struct.pack_into('<i',data,pos,pos-vt)
        return pos,[pos+off if off else None for off in offsets]
    def point(p,q):struct.pack_into('<I',data,p,q-p)
    def string(value):
        align();p=len(data);raw=value.encode('ascii');data.extend(struct.pack('<I',len(raw))+raw+b'\0');return p
    root,r=table([0,4],8);struct.pack_into('<I',data,0,root)
    align();v=len(data);data.extend(struct.pack('<I',len(identities))+bytes(4*len(identities)));point(r[1],v)
    for i,(species,form,gender) in enumerate(identities):
        entry,e=table([4,8,12,16],20);point(v+4+4*i,entry)
        spec,s=table([4,6,8],12);point(e[0],spec)
        struct.pack_into('<HHB',data,s[0],species,form,gender)
        stem='pm0123/pm0123_09_00/pm0123_09_00'
        for p,ext in zip(e[1:],('.trmdl','.trmmt','.trpokecfg')):point(p,string(stem+ext))
    return bytes(data)


class CatalogTests(unittest.TestCase):
    def test_bounded_synthetic_catalog(self):
        rows=catalog.parse_catalog(fixture())
        self.assertEqual(rows[(101,7,1)]['model_path'],'pm0123/pm0123_09_00/pm0123_09_00.trmdl')
    def test_out_of_protocol_placeholder_does_not_hide_valid_rows(self):
        rows=catalog.parse_catalog(fixture(((5000,0,0),(101,7,1))))
        self.assertEqual(set(rows),{(101,7,1)})
    def test_reject_truncation_duplicate_and_unknown_gender(self):
        for data in (fixture()[:14],fixture(((101,7,1),(101,7,1))),fixture(((101,7,3),))):
            with self.subTest(length=len(data)):
                with self.assertRaises((ValueError,UnicodeError)):catalog.parse_catalog(data)
    def test_exact_female_else_default_same_form_only(self):
        default={'model_path':'default'};female={'model_path':'female'}
        rows={(101,0,0):default,(101,0,1):female,(102,2,0):default}
        self.assertIs(catalog.select_entry(rows,101,0,1),female)
        self.assertIs(catalog.select_entry(rows,102,2,1),default)
        self.assertIs(catalog.select_entry(rows,102,2,2),default)
        with self.assertRaises(ValueError):catalog.select_entry(rows,102,3,1)
    def test_service_uses_catalog_identity_not_host_hint_or_offset(self):
        key=101|(7<<11)|(1<<17);selection=('v2',key,3,999,7,1,0)
        entry={'model_path':'pm0123/pm0123_09_00/pm0123_09_00.trmdl'}
        mesh=Path('cache/pm0123_09_00.trmdl');idle=Path('cache/idle.tranm')
        with patch.object(service,'resolve_model',return_value=entry) as resolve,patch.object(service,'prepare_model_form',return_value=(mesh,idle)) as prepare:
            self.assertEqual(service.prepare_selection('game','ooz','cache',selection),(mesh,idle))
            resolve.assert_called_once_with('game','ooz',101,7,1)
            prepare.assert_called_once_with('game','ooz','cache',123,9)


if __name__=='__main__':unittest.main()
