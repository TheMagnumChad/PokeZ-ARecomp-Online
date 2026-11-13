import unittest
from game_ui_models import discover_member_names, fnv64

class CompanionTests(unittest.TestCase):
    def names(self, resources, reference):
        folder='ik_pokemon/data/pm0707/pm0707_00_00'
        members=[(fnv64(folder+'/'+name), b'') for name in resources]
        members.append((123, reference.encode('ascii')))
        return set(discover_member_names('pm0707_00_00',folder,members).values())
    def test_exact_companion(self):
        stem='pm0707_00_00_defaultwait'
        self.assertEqual(self.names([stem+'.tranm',stem+'.tracm'],stem+'.tranm'),{stem+'.tranm',stem+'.tracm'})
    def test_absent_companion(self):
        stem='pm0707_00_00_defaultwait'
        self.assertEqual(self.names([stem+'.tranm'],stem+'.tranm'),{stem+'.tranm'})
    def test_unrelated_clip_not_selected(self):
        self.assertNotIn('pm0707_00_00_attack.tracm',self.names(['pm0707_00_00_idle.tranm','pm0707_00_00_attack.tracm'],'pm0707_00_00_idle.tranm'))
    def test_path_escape_rejected(self):
        self.assertEqual(self.names(['pm0707_00_00/../evil.tracm'],'pm0707_00_00/../evil.tracm'),set())
    def test_explicit_visibility_reference(self):
        name='pm0707_00_00_idle.tracm'
        self.assertEqual(self.names([name],name),{name})

if __name__=='__main__': unittest.main()
