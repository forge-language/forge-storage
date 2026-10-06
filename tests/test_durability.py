#!/usr/bin/env python3
"""Restart a specifically named isolated Docker service and verify durable bytes."""
import argparse
import hashlib
import http.client
import os
import subprocess
import time
from urllib.parse import urlsplit

parser = argparse.ArgumentParser()
parser.add_argument('--base', default='http://127.0.0.1:18105')
parser.add_argument('--container', required=True)
args = parser.parse_args()
if not args.container.startswith(('forge-storage-test', 'forge-storage-check')):
    parser.error('Container name must begin forge-storage-test or forge-storage-check to avoid production restarts')
base = urlsplit(args.base)
if base.hostname not in ('127.0.0.1', 'localhost'):
    parser.error('Durability test must use a local isolated service')
token = os.environ['STORAGE_TOKEN']
data = os.urandom(32769)
path = '/v1/objects/' + hashlib.sha256(data).hexdigest()

def request(method, path, body=None):
    connection = http.client.HTTPConnection(base.hostname, base.port, timeout=5)
    try:
        connection.request(method, path, body, {'Authorization': 'Bearer ' + token})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()

assert request('PUT', path, data)[0] == 201
subprocess.run(['docker', 'restart', args.container], check=True, capture_output=True)
for attempt in range(30):
    try:
        if request('GET', '/health')[0] == 200:
            break
    except OSError:
        pass
    time.sleep(0.2)
else:
    raise RuntimeError('Isolated service did not recover after restart')
assert request('GET', path) == (200, data)
print('Object checksum and bytes survived service restart')
