#!/usr/bin/env python3
"""Hardware OTA negative tests; overwrite the inactive slot, never select it."""
import argparse
import hashlib
import http.server
from pathlib import Path
import socket
import threading
import time
from check_wifi import Console
from ota_update import status


def command(host, text):
    c = Console(host, 4440)
    try:
        return c.command(text)
    finally:
        c.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--firmware', type=Path, default=Path(__file__).resolve().parent.parent /
                        'build/build_esp32c3_oled_042/cmsis_dap_tcp_esp32.bin')
    args = parser.parse_args()
    data = args.firmware.read_bytes()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((args.host, 4440))
        address = probe.getsockname()[0]
    before = status(args.host)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == '/missing':
                self.send_error(404)
                return
            if self.path == '/slow':
                time.sleep(3)
            body = b'x' * 512 if self.path in ('/invalid', '/slow') else data
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body[:512] if self.path == '/truncated' else body)

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer((address, 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base = f'http://{address}:{server.server_port}'
    digest = hashlib.sha256(data).hexdigest()
    try:
        assert 'ESP_ERR_INVALID_ARG' in command(args.host, 'ota ftp://invalid ' + digest)
        assert 'ESP_ERR_INVALID_ARG' in command(args.host, 'ota ' + base + '/image xyz')
        for path, sha, expected in (
                ('/missing', digest, 'ESP_ERR_INVALID_SIZE'),
                ('/invalid', digest, 'ESP_ERR_INVALID_RESPONSE'),
                ('/truncated', digest, 'ESP_FAIL'),
                ('/image', '0' * 64, 'ESP_ERR_INVALID_CRC'),
                ('/slow', digest, 'ESP_ERR_INVALID_RESPONSE')):
            assert 'OTA started;' in command(args.host, f'ota {base}{path} {sha}')
            if path == '/slow':
                assert 'ESP_ERR_INVALID_STATE' in command(args.host, f'ota {base}/image {digest}')
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                result = status(args.host)
                if result['state'] == 'failed':
                    break
                time.sleep(0.2)
            else:
                raise TimeoutError('OTA did not finish failure test ' + path)
            assert result['error'] == expected, result
            assert result['running'] == before['running'] and result['boot'] == before['boot']
            print(path, expected, 'boot selection unchanged: PASS', flush=True)
        assert '"sensor":"INA219"' in command(args.host, 'power')
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == '__main__':
    main()
