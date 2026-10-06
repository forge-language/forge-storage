#!/usr/bin/env python3
"""Serve wrong download bytes to test the real native HTTP/hash implementation."""
import http.server
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


class WrongBytes(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        data = b'downloaded bytes deliberately differ from the expected source\n'
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


with tempfile.TemporaryDirectory(prefix='forge-storage-digest-check-') as directory:
    source = Path(directory) / 'source'
    source.write_bytes(b'original expected bytes\n')
    destination = Path(directory) / 'destination'
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), WrongBytes)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([sys.argv[1], 'http://127.0.0.1:' + str(server.server_port) + '/file', str(source), str(destination)], check=True)
        assert not destination.exists()
        assert not list(Path(directory).glob('*.part-*'))
    finally:
        server.shutdown()
        server.server_close()
