#!/usr/bin/env python3
"""Version ordering and release-to-source integrity for automated packages."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('ci_version', ROOT / 'ci/version.py')
version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(version)
SHA = '0123456789abcdef0123456789abcdef01234567'


class VersionTests(unittest.TestCase):
    def test_source_versions_agree(self):
        self.assertRegex(version.base_version(ROOT), r'^\d+\.\d+\.\d+$')

    def test_version_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'debian').mkdir()
            (root / 'CMakeLists.txt').write_text('project(dcc-proxy VERSION 1.2.3 LANGUAGES CXX)')
            (root / 'debian/changelog').write_text('dcc-proxy (1.2.4) UNRELEASED; urgency=medium\n')
            with self.assertRaises(ValueError):
                version.base_version(root)

    def test_preview_ordering(self):
        first = version.release_metadata('1.2.3', 'refs/heads/main', '9', SHA)
        second = version.release_metadata('1.2.3', 'refs/heads/main', '10', SHA)
        self.assertEqual(first['prerelease'], 'true')
        self.assertEqual(first['release_tag'], 'ci-9-0123456')
        subprocess.check_call(['dpkg', '--compare-versions', '1.2.2', 'lt', first['version']])
        subprocess.check_call(['dpkg', '--compare-versions', first['version'], 'lt', second['version']])
        subprocess.check_call(['dpkg', '--compare-versions', second['version'], 'lt', '1.2.3'])

    def test_stable_tag_must_match(self):
        stable = version.release_metadata('1.2.3', 'refs/tags/v1.2.3', '42', SHA)
        self.assertEqual(stable, {'version': '1.2.3', 'release_tag': 'v1.2.3', 'prerelease': 'false'})
        with self.assertRaises(ValueError):
            version.release_metadata('1.2.3', 'refs/tags/v1.2.4', '42', SHA)

    def test_reject_invalid_inputs(self):
        for run in ('', '0', '-1', '1\nrelease_tag=injected', 'abc'):
            with self.assertRaises(ValueError):
                version.release_metadata('1.2.3', 'refs/heads/main', run, SHA)
        with self.assertRaises(ValueError):
            version.release_metadata('1.2.3', 'refs/heads/main', '1', 'not-a-sha')


if __name__ == '__main__':
    unittest.main()
