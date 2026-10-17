#!/usr/bin/env python3
"""Flash generated ESP-IDF files from macOS/Linux using host esptool."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--build-dir', type=Path, default=Path(__file__).resolve().parent.parent / 'build/build_esp32c3_oled_042')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    config = json.loads((build / 'flasher_args.json').read_text())
    command = [sys.executable, '-m', 'esptool', '--chip', config['extra_esptool_args']['chip'],
               '--port', args.port, '--baud', '460800', '--before', 'default_reset',
               '--after', 'hard_reset', 'write_flash']
    for key in ('flash_mode', 'flash_size', 'flash_freq'):
        command.extend(['--' + key, config['flash_settings'][key]])
    for offset, filename in config['flash_files'].items():
        path = build / filename
        if not path.is_file():
            parser.error('Missing firmware file: ' + str(path))
        command.extend([offset, str(path)])
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
