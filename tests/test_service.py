#!/usr/bin/env python3
"""Black-box object API checks. Use an isolated service; writes immutable test objects."""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import time
import unittest
from urllib.parse import urlsplit

parser = argparse.ArgumentParser()
parser.add_argument('--base', default='http://127.0.0.1:18105')
parser.add_argument('--root', help='Isolated service filesystem root, enables upload cleanup checks')
config, rest = parser.parse_known_args()
BASE = urlsplit(config.base)
TOKEN = os.environ.get('STORAGE_TOKEN', '')
if len(TOKEN) < 32:
    parser.error('Set STORAGE_TOKEN to the isolated service writer token (at least 32 characters)')


def request(method, path, body=None, headers=None):
    cls = http.client.HTTPSConnection if BASE.scheme == 'https' else http.client.HTTPConnection
    connection = cls(BASE.hostname, BASE.port, timeout=15)
    try:
        connection.request(method, BASE.path.rstrip('/') + path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def object_path(data):
    return '/v1/objects/' + hashlib.sha256(data).hexdigest()


class StorageTests(unittest.TestCase):
    def upload(self, data, path=None):
        return request('PUT', path or object_path(data), data, {'Authorization': 'Bearer ' + TOKEN})

    def test_health_and_invalid_routes(self):
        status, _, body = request('GET', '/health')
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)['language'], 'Forge')
        self.assertEqual(request('GET', '/missing')[0], 404)
        self.assertEqual(request('GET', '/v1/objects/not-a-hash')[0], 400)
        self.assertEqual(request('GET', '/v1/objects/' + 'A' * 64)[0], 400)
        self.assertEqual(request('DELETE', '/v1/objects/' + 'a' * 64)[0], 405)
        self.assertEqual(request('GET', '/v1/objects/' + 'a' * 64 + '/extra')[0], 400)

    def test_authentication_precedes_upload(self):
        data = b'unauthorized-' + os.urandom(16)
        self.assertEqual(request('PUT', object_path(data), data)[0], 401)
        self.assertEqual(request('PUT', object_path(data), data, {'Authorization': 'Bearer incorrect'})[0], 401)
        self.assertEqual(request('GET', object_path(data))[0], 404)

    def test_binary_roundtrip_metadata_head_cache(self):
        data = bytes(range(256)) * 1024 + os.urandom(123)
        path = object_path(data)
        status, _, body = self.upload(data)
        self.assertEqual(status, 201)
        metadata = json.loads(body)
        self.assertEqual(metadata, {'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)})
        self.assertEqual(json.loads(request('GET', path + '/meta')[2]), metadata)
        status, headers, body = request('GET', path)
        self.assertEqual((status, body), (200, data))
        self.assertEqual(headers['Content-Type'], 'application/octet-stream')
        self.assertIn('immutable', headers['Cache-Control'])
        self.assertEqual(headers['X-Content-Type-Options'], 'nosniff')
        self.assertEqual(headers['Content-Disposition'], 'attachment')
        self.assertEqual(headers['ETag'], '"' + metadata['sha256'] + '"')
        head = request('HEAD', path)
        self.assertEqual((head[0], head[2]), (200, b''))
        self.assertEqual(int(head[1]['Content-Length']), len(data))
        cached = request('GET', path, headers={'If-None-Match': headers['ETag']})
        self.assertEqual((cached[0], cached[2]), (304, b''))

    def test_empty_object(self):
        self.assertIn(self.upload(b'')[0], (200, 201))
        self.assertEqual(request('GET', object_path(b''))[2], b'')
        self.assertEqual(request('GET', object_path(b''), headers={'Range': 'bytes=0-0'})[0], 416)

    def test_range_and_if_range(self):
        data = os.urandom(1000)
        path = object_path(data)
        self.assertEqual(self.upload(data)[0], 201)
        for range_value, expected, content_range in [('bytes=10-19', data[10:20], 'bytes 10-19/1000'), ('bytes=990-', data[990:], 'bytes 990-999/1000'), ('bytes=-7', data[-7:], 'bytes 993-999/1000'), ('bytes=999-99999', data[-1:], 'bytes 999-999/1000')]:
            with self.subTest(range=range_value):
                status, headers, body = request('GET', path, headers={'Range': range_value})
                self.assertEqual((status, body), (206, expected))
                self.assertEqual(headers['Content-Range'], content_range)
        for value in ['bytes=1000-', 'bytes=20-10', 'bytes=0-1,3-4', 'garbage']:
            self.assertEqual(request('GET', path, headers={'Range': value})[0], 416)
        status, _, body = request('GET', path, headers={'Range': 'bytes=0-5', 'If-Range': '"different"'})
        self.assertEqual((status, body), (200, data))

    def test_checksum_mismatch_does_not_publish(self):
        desired = b'mismatch-target-' + os.urandom(16)
        self.assertEqual(self.upload(b'wrong body', object_path(desired))[0], 422)
        self.assertEqual(request('GET', object_path(desired))[0], 404)
        self.assertEqual(request('GET', object_path(desired) + '/meta')[0], 404)

    def test_concurrent_immutable_duplicate(self):
        data = os.urandom(50000)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(lambda _: self.upload(data)[0], range(8)))
        self.assertEqual(results.count(201), 1)
        self.assertEqual(results.count(200), 7)
        self.assertEqual(request('GET', object_path(data))[2], data)
        self.assertEqual(self.upload(b'cannot overwrite', object_path(data))[0], 422)
        self.assertEqual(request('GET', object_path(data))[2], data)

    def test_streaming_chunked_upload(self):
        data = os.urandom(196613)
        parts = (data[index:index + 4096] for index in range(0, len(data), 4096))
        self.assertEqual(request('PUT', object_path(data), parts, {'Authorization': 'Bearer ' + TOKEN})[0], 201)
        self.assertEqual(request('GET', object_path(data))[2], data)

    def test_oversized_content_length(self):
        # Header-only request: no 256 MiB allocation or upload is needed to check rejection.
        connection = http.client.HTTPConnection(BASE.hostname, BASE.port, timeout=10)
        try:
            connection.putrequest('PUT', '/v1/objects/' + 'a' * 64)
            connection.putheader('Authorization', 'Bearer ' + TOKEN)
            connection.putheader('Content-Length', str(256 * 1024 * 1024 + 1))
            connection.endheaders()
            self.assertEqual(connection.getresponse().status, 413)
        finally:
            connection.close()

    @unittest.skipUnless(config.root, '--root required for filesystem cleanup checks')
    def test_upload_temporary_files_are_cleaned(self):
        desired = b'cleanup-target-' + os.urandom(16)
        self.assertEqual(self.upload(b'wrong body', object_path(desired))[0], 422)
        time.sleep(0.05)
        self.assertEqual(list(Path(config.root).glob('.upload-*')), [])


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])
