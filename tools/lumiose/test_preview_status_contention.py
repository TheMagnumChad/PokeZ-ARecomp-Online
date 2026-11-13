import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import model_preview_service as service

class StatusContentionTests(unittest.TestCase):
 def test_locked_replace_preserves_old_status_then_recovers(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);old=('v2',568,1,568,0,0,0);new=('v2',568,2,568,0,0,0)
   self.assertTrue(service.write_status(root,old,'ready'))
   target=root/'v2-568/status.txt'
   with patch.object(service.os,'replace',side_effect=PermissionError('reader lock')):
    self.assertFalse(service.write_status(root,new,'loading'))
   self.assertEqual(target.read_text().strip(),'568 1 ready')
   self.assertTrue(service.write_status(root,new,'loading'))
   self.assertEqual(target.read_text().strip(),'568 2 loading')
 def test_unrelated_io_error_not_hidden(self):
  with tempfile.TemporaryDirectory() as d:
   with patch.object(service.os,'replace',side_effect=OSError('disk error')):
    with self.assertRaises(OSError): service.write_status(Path(d),('v2',568,1,568,0,0,0),'ready')

if __name__=='__main__': unittest.main()
