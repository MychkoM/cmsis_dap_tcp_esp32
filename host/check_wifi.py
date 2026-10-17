#!/usr/bin/env python3
"""Check CMSIS-DAP framing, UART TCP transport and optional INA219 over Wi-Fi.

No target MCU or UART loopback is required. UART TX is verified through the
bridge counters; physical UART RX and target SWD transfers need external hardware.
"""
import argparse
import json
import re
import socket
import struct
import time


def recv_exact(sock, count):
    result = bytearray()
    while len(result) < count:
        chunk = sock.recv(count - len(result))
        if not chunk:
            raise ConnectionError('Unexpected EOF')
        result.extend(chunk)
    return bytes(result)


def dap_request(sock, payload):
    sock.sendall(struct.pack('<IHBB', 0x00504144, len(payload), 1, 0) + payload)
    signature, length, kind, reserved = struct.unpack('<IHBB', recv_exact(sock, 8))
    if signature != 0x00504144 or kind != 2 or not 0 < length <= 4096:
        raise ValueError('Invalid CMSIS-DAP TCP response header')
    response = recv_exact(sock, length)
    if response[0] != payload[0]:
        raise ValueError('CMSIS-DAP response command mismatch')
    return response


class Console:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.3)
        self.read_until_prompt()  # also consumes the terminal detection probe

    def read_until_prompt(self):
        data = bytearray()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError('Console closed')
            data.extend(chunk)
            if data.endswith(b'esp32> '):
                return data.decode(errors='replace')
        raise TimeoutError('Console prompt timeout: ' + repr(bytes(data)))

    def command(self, line):
        self.sock.sendall(line.encode() + b'\n')
        return self.read_until_prompt()

    def close(self):
        self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--dap-port', type=int, default=4441)
    parser.add_argument('--uart-port', type=int, default=4442)
    parser.add_argument('--console-port', type=int, default=4440)
    parser.add_argument('--ina219', type=int, default=0, metavar='SAMPLES')
    args = parser.parse_args()
    with socket.create_connection((args.host, args.dap_port), timeout=5) as sock:
        for info, label in ((1, 'vendor'), (2, 'product'), (3, 'serial'), (4, 'firmware')):
            response = dap_request(sock, bytes([0, info]))
            if len(response) < 2 or len(response) != response[1] + 2:
                raise ValueError('Invalid DAP_Info response')
            print(label + ':', response[2:].rstrip(b'\0').decode())
        caps = dap_request(sock, bytes([0, 0xf0]))
        if len(caps) < 3 or not caps[2] & 1:
            raise ValueError('Adapter does not advertise SWD')
        if dap_request(sock, b'\x02\x01') != b'\x02\x01':
            raise ValueError('DAP_Connect(SWD) failed')
        if dap_request(sock, b'\x03') != b'\x03\x00':
            raise ValueError('DAP_Disconnect failed')
        print('CMSIS-DAP: info, capabilities, SWD connect/disconnect OK')
    console = Console(args.host, args.console_port)
    try:
        # Verify Wi-Fi -> socket -> UART TX through firmware byte counters.
        with socket.create_connection((args.host, args.uart_port), timeout=5) as uart:
            pattern = r'UART bridge 1: connected.*Bytes: TX=(\d+) RX=(\d+)'
            first = None
            for _ in range(10):
                before = console.command('status')
                first = re.search(pattern, before)
                if first:
                    break
                time.sleep(0.1)
            if not first:
                raise ValueError('UART connection missing from status: ' + before)
            payload = b'CMSIS-DAP WiFi UART check\r\n'
            uart.sendall(payload)
            time.sleep(0.5)
            after = console.command('status')
            last = re.search(pattern, after)
            if not last or int(last[1]) - int(first[1]) != len(payload):
                raise ValueError('UART TX counter did not advance: ' + after)
            print('UART: TCP connection and TX of', len(payload), 'bytes OK')
            print(after)
        for _ in range(args.ina219):
            result = console.command('ina219')
            match = re.search(r'\{"sensor":"INA219"[^\r\n]*\}', result)
            if not match:
                raise ValueError('INA219 measurement failed: ' + result)
            sample = json.loads(match[0])
            for key in ('bus_voltage_v', 'shunt_voltage_mv', 'current_ma', 'power_mw'):
                if not isinstance(sample[key], (float, int)):
                    raise ValueError('Non-numeric INA219 measurement')
            print(json.dumps(sample))
            time.sleep(0.2)
    finally:
        console.close()


if __name__ == '__main__':
    main()
