#!/bin/sh
# Build using the already installed ESP-IDF 6.0.2 image; never pull or push.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec podman run --rm --pull=never \
    -v "$project_dir:/project" -w /project \
    docker.io/espressif/esp-matter:latest_idf_v6.0.2 \
    bash -c 'cd /project && IDF_PRESET=board_esp32c3_oled_042 idf.py build'
