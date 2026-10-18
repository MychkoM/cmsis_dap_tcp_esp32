#!/usr/bin/env python3
"""Serve one app image on the LAN and trigger/verify the device OTA update."""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import socket
import threading
import time
from check_wifi import Console


def status(host):
    console = Console(host, 4440)
    try:
        for line in console.command('ota status').splitlines():
            if line.startswith('{'):
                return json.loads(line)
    finally:
        console.close()
    raise RuntimeError('Device did not return OTA status')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', help='Device IPv4 address')
    parser.add_argument('--firmware', type=Path, default=Path(__file__).resolve().parent.parent /
                        'build/build_esp32c3_oled_042/cmsis_dap_tcp_esp32.bin')
    parser.add_argument('--bind', help='LAN IPv4 address of this computer (auto-detected by default)')
    parser.add_argument('--port', type=int, default=0, help='Local HTTP port (auto-select by default)')
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    data = args.firmware.read_bytes()
    if len(data) < 288 or data[0] != 0xe9:
        parser.error('Expected an ESP-IDF application .bin image, not merged flash data')
    digest = hashlib.sha256(data).hexdigest()
    if not args.bind:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((args.host, 4440))
            args.bind = probe.getsockname()[0]
    before = status(args.host)
    if before['next'] == 'none' or before['validation'] == 'pending_verify':
        parser.error('Device has no OTA slot or is still validating its current boot')
    if len(data) > before['slot_size']:
        parser.error('Image is larger than the device OTA slot')

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != '/firmware.bin':
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    url = f'http://{args.bind}:{server.server_port}/firmware.bin'
    print('Serving only', args.firmware.name, 'at', url, flush=True)
    print('SHA256:', digest, flush=True)
    try:
        console = Console(args.host, 4440)
        try:
            reply = console.command(f'ota {url} {digest}')
            if 'OTA started;' not in reply:
                raise RuntimeError(reply)
        finally:
            console.close()
        deadline = time.monotonic() + args.timeout
        last = None
        while time.monotonic() < deadline:
            try:
                current = status(args.host)
            except (OSError, TimeoutError, ConnectionError):
                time.sleep(1)
                continue
            progress = (current['state'], current['bytes'], current['validation'])
            if progress != last:
                print(current, flush=True)
                last = progress
            if current['state'] == 'failed':
                raise RuntimeError('OTA failed: ' + current['error'])
            if current['running'] == before['next'] and current['validation'] == 'valid':
                print('OTA download, slot switch and boot confirmation: PASS', flush=True)
                return
            time.sleep(1)
        raise TimeoutError('OTA update or boot confirmation timed out')
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == '__main__':
    main()
