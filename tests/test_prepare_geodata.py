#!/usr/bin/env python3
"""Regression tests for build-time downloads; no external network required."""
import hashlib
import http.client
import importlib.util
import io
from pathlib import Path
import socket
import tempfile
import unittest
from unittest import mock
import urllib.error


SCRIPT = Path(__file__).resolve().parents[1] / 'tools/prepare-geodata.py'
SPEC = importlib.util.spec_from_file_location('prepare_geodata', str(SCRIPT))
geodata = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(geodata)


class InterruptedResponse(io.BytesIO):
    def read(self, size=-1):
        if self.tell():
            raise socket.timeout('The read operation timed out')
        return super().read(size)


class PrepareGeodataTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.cache = Path(temporary.name)
        self.data = b'official database fixture\n' * 20000
        self.item = dict(file='GeoIP.dat', url='https://example.invalid/geoip.dat',
                         size=len(self.data),
                         sha256=hashlib.sha256(self.data).hexdigest())
        self.path = self.cache / self.item['file']
        self.partial = self.cache / 'GeoIP.dat.part'
        self.urlopen = mock.patch.object(geodata.urllib.request, 'urlopen').start()
        self.sleep = mock.patch.object(geodata.time, 'sleep').start()
        self.addCleanup(mock.patch.stopall)

    def prepare(self, **kwargs):
        geodata.prepare_file(self.item, self.cache, **kwargs)

    def test_verified_cache_never_downloads(self):
        self.path.write_bytes(self.data)
        self.prepare()
        self.prepare(offline=True)
        self.urlopen.assert_not_called()

    def test_offline_rejects_missing_or_corrupt_cache_without_network(self):
        for content in (None, b'corrupt'):
            with self.subTest(content=content):
                if content is not None:
                    self.path.write_bytes(content)
                with self.assertRaisesRegex(RuntimeError, 'Missing or invalid'):
                    self.prepare(offline=True)
        self.urlopen.assert_not_called()

    def test_timeout_during_read_retries_and_discards_partial_data(self):
        self.urlopen.side_effect = [InterruptedResponse(self.data),
                                   io.BytesIO(self.data)]
        self.prepare(timeout=12, attempts=2)
        self.assertEqual(self.path.read_bytes(), self.data)
        self.assertFalse(self.partial.exists())
        self.assertEqual(self.urlopen.call_args_list, [
            mock.call(self.item['url'], timeout=12),
            mock.call(self.item['url'], timeout=12)])
        self.sleep.assert_called_once_with(2)

    def test_connection_and_http_failures_retry(self):
        for error in (urllib.error.URLError('connection failed'),
                      urllib.error.HTTPError(self.item['url'], 503,
                                             'Unavailable', {}, None),
                      http.client.IncompleteRead(b'partial', 100)):
            with self.subTest(error=error):
                self.urlopen.reset_mock()
                self.urlopen.side_effect = [error, io.BytesIO(self.data)]
                self.prepare(attempts=2)
                self.assertEqual(self.path.read_bytes(), self.data)
                self.path.unlink()

    def test_truncated_and_wrong_hash_downloads_retry(self):
        for invalid in (self.data[:-1], b'x' * len(self.data)):
            with self.subTest(length=len(invalid)):
                self.urlopen.side_effect = [io.BytesIO(invalid),
                                           io.BytesIO(self.data)]
                self.prepare(attempts=2)
                self.assertEqual(self.path.read_bytes(), self.data)
                self.assertFalse(self.partial.exists())
                self.path.unlink()

    def test_bad_cache_is_replaced_only_after_verification(self):
        self.path.write_bytes(b'bad old cache')
        self.urlopen.return_value = io.BytesIO(self.data)
        self.prepare()
        self.assertEqual(self.path.read_bytes(), self.data)

    def test_failed_download_preserves_existing_cache_and_cleans_partial(self):
        self.path.write_bytes(b'old cache')
        self.urlopen.return_value = InterruptedResponse(self.data)
        with self.assertRaisesRegex(RuntimeError, 'after 1 attempts'):
            self.prepare(attempts=1)
        self.assertEqual(self.path.read_bytes(), b'old cache')
        self.assertFalse(self.partial.exists())
        self.sleep.assert_not_called()

    def test_retry_limit_gives_actionable_error(self):
        self.urlopen.side_effect = socket.timeout('timed out')
        with self.assertRaisesRegex(RuntimeError, 'HTTPS_PROXY'):
            self.prepare(attempts=3)
        self.assertEqual(self.urlopen.call_count, 3)
        self.assertEqual(self.sleep.call_args_list, [mock.call(2), mock.call(4)])
        self.assertFalse(self.path.exists())
        self.assertFalse(self.partial.exists())

    def test_checksum_failure_never_installs_download(self):
        self.urlopen.return_value = io.BytesIO(b'wrong data')
        with self.assertRaises(RuntimeError):
            self.prepare(attempts=1)
        self.assertFalse(self.path.exists())
        self.assertFalse(self.partial.exists())


if __name__ == '__main__':
    unittest.main()
