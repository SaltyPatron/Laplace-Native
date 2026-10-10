import itertools
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


class IsaEvidence(unittest.TestCase):
    def check(self, change=None):
        root = ET.Element('testsuite')
        for kernel, isa in itertools.product(('follows', 'core', 'coord', 'shape', 's3', 'kernels'),
                                            ('scalar', 'sse2', 'avx2', 'avxvnni', 'avx512', 'avx512vnni')):
            ET.SubElement(root, 'testcase', name=f'{kernel}_{isa}', status='run')
        if change:
            change(root)
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / 'result.xml'
            ET.ElementTree(root).write(report)
            return subprocess.run([sys.executable, str(Path(__file__).with_name('verify-isa.py')), str(report)],
                                  capture_output=True, text=True).returncode

    def test_complete_evidence_passes(self):
        self.assertEqual(self.check(), 0)

    def test_skipped_isa_is_not_a_pass(self):
        self.assertNotEqual(self.check(lambda root: ET.SubElement(root[-1], 'skipped')), 0)

    def test_missing_isa_is_not_a_pass(self):
        self.assertNotEqual(self.check(lambda root: root.remove(root[-1])), 0)

    def test_failed_isa_is_not_a_pass(self):
        self.assertNotEqual(self.check(lambda root: ET.SubElement(root[-1], 'failure')), 0)


if __name__ == '__main__':
    unittest.main()
