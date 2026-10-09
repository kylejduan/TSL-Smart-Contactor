"""Synthetic privacy checks, including removed files retained in Git history."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_public_tree as privacy


class PublicTreeTests(unittest.TestCase):
    def test_private_records_and_keys_fail_but_synthetic_json_is_allowed(self):
        for path in ('provisioning/setup.json', '.tools/record.json', 'trace.jsonl', 'device.pem', 'env.txt'):
            with self.subTest(path=path):
                self.assertIn('private_file', privacy.violations(path, b'{}'))
        self.assertEqual(privacy.violations('tests/fixtures/home.synthetic.json', b'{}'), [])

    def test_private_key_detection_does_not_require_a_key_filename(self):
        data = b'-----BEGIN ' + b'EC PRIVATE KEY-----\nsynthetic\n'
        self.assertIn('private_key', privacy.violations('docs/sample.md', data))

    def test_personal_paths_are_rejected_but_generic_placeholders_are_allowed(self):
        self.assertIn('personal_machine_path', privacy.violations('README.md', b'/home/' + b'example/project'))
        self.assertEqual(privacy.violations('README.md', b'/home/<user>/project'), [])

    def test_personal_document_timing_and_mac_are_not_confused_with_test_fixtures(self):
        data = b'12:34:56 02:00:00:00:00:01'
        self.assertIn('personal_runtime_timing', privacy.violations('docs/verification.md', data))
        self.assertIn('hardware_identifier', privacy.violations('docs/verification.md', data))
        self.assertEqual(privacy.violations('tests/sample.cpp', data), [])
        self.assertEqual(privacy.violations('docs/verified_interfaces.md', b'Reviewed 2026-01-01; SDK v5.5.2'), [])

    def test_deleted_record_is_still_detected_in_history(self):
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as temporary:
            try:
                os.chdir(temporary)
                def git(*args):
                    subprocess.run(['git', *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                git('init', '-q')
                git('config', 'user.name', 'synthetic')
                git('config', 'user.email', 'test@example.com')
                Path('trace.jsonl').write_text('{}\n')
                git('add', 'trace.jsonl')
                git('commit', '-qm', 'Synthetic record')
                git('rm', '-q', 'trace.jsonl')
                git('commit', '-qm', 'Remove synthetic record')
                self.assertEqual(privacy.check(False), [])
                self.assertIn(('history:trace.jsonl', 'private_file'), privacy.check(True))
            finally:
                os.chdir(previous)

    def test_diagnostic_output_never_echoes_matching_data(self):
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as temporary:
            try:
                os.chdir(temporary)
                subprocess.run(['git', 'init', '-q'], check=True)
                Path('trace.jsonl').write_text('synthetic-sensitive-value\n')
                subprocess.run(['git', 'add', 'trace.jsonl'], check=True)
                output = io.StringIO()
                from unittest.mock import patch
                with patch.object(sys, 'argv', ['check_public_tree.py']), contextlib.redirect_stdout(output):
                    self.assertEqual(privacy.main(), 1)
                self.assertNotIn('synthetic-sensitive-value', output.getvalue())
            finally:
                os.chdir(previous)


if __name__ == '__main__':
    unittest.main()
