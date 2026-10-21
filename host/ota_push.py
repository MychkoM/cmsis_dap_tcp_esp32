#!/usr/bin/env python3
"""Push a firmware image directly into the debugger's OTA slot over the TCP console.

No HTTP server needed: the device-side 'ota push' command accepts the raw
image on the same console connection (port 4440), verifies SHA-256, and
reboots. Works from any machine that can reach the device.

Usage: python host/ota_push.py <device-ip> [firmware.bin]
"""
import argparse
import hashlib
import socket
import sys
import time
from pathlib import Path

from check_wifi import Console


def read_until(sock, marker, deadline_s, also_fail=None):
    data = bytearray()
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        data.extend(chunk)
        if marker in data:
            return bytes(data)
        for bad in (also_fail or []):
            if bad in data:
                raise RuntimeError('Device refused: ' + repr(bytes(data)))
    raise TimeoutError('Waiting for ' + repr(marker) + ': ' + repr(bytes(data)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', help='Device IPv4 address')
    parser.add_argument('firmware', nargs='?', type=Path, default=Path(__file__).resolve().parent.parent /
                        'build/build_esp32c3_oled_042/cmsis_dap_tcp_esp32.bin')
    parser.add_argument('--port', type=int, default=4440)
    args = parser.parse_args()

    blob = args.firmware.read_bytes()
    sha256 = hashlib.sha256(blob).hexdigest()
    print(f"{args.firmware}: {len(blob)} bytes, sha256={sha256[:16]}...")

    console = Console(args.host, args.port)   # consumes probe, waits for prompt
    sock = console.sock
    sock.settimeout(5)
    sock.sendall(f"ota push {len(blob)} {sha256}\n".encode())
    read_until(sock, b'OTA PUSH READY', 10, also_fail=[b'Usage:', b'failed:', b'FAILED'])
    print('Device ready; streaming image...')

    sock.settimeout(60)
    start = time.monotonic()
    sent = 0
    for off in range(0, len(blob), 16384):
        chunk = blob[off:off + 16384]
        sock.sendall(chunk)
        sent += len(chunk)
        done = time.monotonic() - start
        rate = sent / done / 1024 if done > 0 else 0
        print(f"\r  {sent}/{len(blob)} bytes ({rate:.0f} KB/s)", end='', flush=True, file=sys.stderr)
    print(file=sys.stderr)

    try:
        tail = read_until(sock, b'OTA PUSH ', 90, also_fail=[])
        verdict = tail.decode(errors='replace')
    except (TimeoutError, ConnectionError) as exc:
        # A successful push ends with the device rebooting -- the socket may
        # reset before the OK line reaches us. Verify via a fresh status read.
        print(f'Connection ended during verdict ({exc}); checking status...')
        verdict = ''
    if 'OTA PUSH OK' in verdict:
        print('OTA PUSH OK -- device verified image and is rebooting.')
        return 0
    time.sleep(12)   # reboot + service startup + boot-confirmation window
    for _ in range(5):
        try:
            console2 = Console(args.host, args.port)
            reply = console2.command('ota status')
            console2.close()
            if 'pending_verify' in reply or '"valid"' in reply:
                print('Device is back with a fresh image: push succeeded.')
                return 0
            break
        except (OSError, ConnectionError, TimeoutError):
            time.sleep(5)
    print('Push verdict unknown -- connect and check "ota status".')
    return 2


if __name__ == '__main__':
    sys.exit(main())
