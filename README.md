# CMSIS-DAP TCP for ESP32

This firmware turns an ESP32 into a network JTAG/SWD adapter for OpenOCD.
The same Wi-Fi connection provides access to the target board's UART,
a management console, INA219 measurements, and firmware updates over OTA.

The main configuration in this version is an **ESP32-C3 Development Board
With 0.42 Inch OLED**, with an external **MCU-219 / INA219** connected to
**GPIO5 for SDA** and **GPIO6 for SCL**. Other boards and optional interfaces
are selected through CMake presets and `menuconfig`.

![CMSIS-DAP TCP overview](img/cmsis_dap_tcp_diagram.svg)

This guide distinguishes three devices:

- **Host computer**: runs the build tools, OpenOCD, terminals, and Python scripts.
- **Adapter**: the ESP32 running this firmware and accepting Wi-Fi connections.
- **Target board**: an external device connected to the adapter's SWD/JTAG or
  UART pins. OTA updates the adapter; OpenOCD programs the target board.

## Contents

1. [Features and limitations](#features)
2. [Boards and presets](#boards)
3. [ESP32-C3 OLED and INA219 wiring](#wiring)
4. [Quick start](#quick-start)
5. [Building and configuration](#build)
6. [USB flashing and backup](#usb-flash)
7. [Wi-Fi, network services, and the console](#network)
8. [Complete command reference](#commands)
9. [Power measurements and the ring buffer](#power)
10. [OTA installation, updates, and rollback](#ota)
11. [Connecting OpenOCD](#openocd)
12. [UART over Wi-Fi](#uart)
13. [Internal ADC streaming](#adc)
14. [Verification and tests](#verification)
15. [Troubleshooting](#troubleshooting)
16. [Project structure and component integration](#internals)
17. [Performance and upstream origins](#upstream)

<a id="features"></a>
## 1. Features and limitations

| Feature | Implementation |
|---|---|
| Network SWD/JTAG | CMSIS-DAP over TCP; up to three independent interfaces |
| Network UART | Up to three UART-to-TCP bridges, subject to available UARTs and GPIOs |
| Management | The same commands through USB and TCP consoles |
| Wi-Fi | Station connection, DHCP, and settings stored in NVS |
| INA219 monitoring | Bus voltage, shunt voltage, signed current, and signed power |
| Averaging | Background sampling every 50 ms with a configurable averaging window |
| History | A 200-entry RAM ring buffer of completed intervals with timestamps |
| OTA | HTTP/HTTPS download, mandatory SHA-256, two app slots, and boot rollback |
| Internal ADC | An optional TCP voltage stream, enabled separately |
| Indicators | Standard or RGB LEDs on supported boards; disabled in the OLED preset |

Current implementation details and limitations:

- SWO is not supported.
- Each CMSIS-DAP interface accepts one active client. Separate interfaces
  operate independently. Each UART bridge also serves one active TCP client;
  the TCP command console supports multiple connections.
- SWD/JTAG availability depends on the preset. The ESP32-C3 OLED preset
  enables SWD and disables JTAG.
- The OLED does not display information: no display driver is implemented.
  Its I2C address may still appear when scanning the shared bus.
- Power history and its averaging interval are RAM-only. Any restart,
  including an OTA reboot, clears history and restores the 1000 ms interval.
- There are no commands for INA219 zero-offset calibration, clearing history,
  or accumulating energy in Wh/mAh. `power history` reports averages, not energy.
- The TCP console and services are unauthenticated and unencrypted. Use a
  trusted LAN. The OTA SHA-256 verifies that the download matches the supplied
  hash; it does not authenticate the person issuing the command.

<a id="boards"></a>
## 2. Boards and presets

All presets are listed in [CMakePresets.json](CMakePresets.json).

| Preset | Board | IDF target |
|---|---|---|
| `board_esp32c3_oled_042` | ESP32-C3 with 0.42-inch OLED and 4 MB flash | `esp32c3` |
| `board_esp32c3_super_mini` | ESP32-C3 SuperMini | `esp32c3` |
| `board_esp32s3_devkit_c1` | Espressif ESP32-S3 DevKitC-1 | `esp32s3` |
| `board_yd_esp32_s3` | VCC-GND Studio YD-ESP32-S3 | `esp32s3` |
| `board_esp32s3_zero` | Waveshare ESP32-S3-Zero | `esp32s3` |
| `board_xiao_esp32c6` | Seeed Studio XIAO ESP32-C6 | `esp32c6` |
| `board_xiao_esp32c6_alt` | XIAO ESP32-C6 with alternate UART GPIOs | `esp32c6` |

Common defaults are in [sdkconfig.defaults](sdkconfig.defaults); board settings
are in `sdkconfig.board_*`. INA219 and OTA are enabled in the OLED preset.
Having a module in the source tree does not mean it is enabled in every preset.

Each configuration has its own build directory, for example:

```text
build/build_esp32c3_oled_042/
build/build_esp32s3_devkit_c1/
```

The original pinout diagrams for other boards are retained:

![ESP32-S3 DevKitC-1](img/esp32s3_devkitc_1.png)

![ESP32-C3 SuperMini](img/esp32c3_super_mini.png)

![XIAO ESP32-C6](img/xiao_esp32c6_pinout.png)

<a id="wiring"></a>
## 3. ESP32-C3 OLED and INA219 wiring

### 3.1. OLED preset pin assignments

The source of these assignments is
[sdkconfig.board_esp32c3_oled_042](sdkconfig.board_esp32c3_oled_042).
Numbers below identify **GPIOs**, not physical header positions.

| Function | ESP32-C3 pin | Connection |
|---|---|---|
| SWCLK | GPIO0 | Target SWCLK |
| SWDIO | GPIO1 | Target SWDIO |
| nRESET | GPIO2 | Optional active-low target reset |
| UART1 TX | GPIO21 | Target RX |
| UART1 RX | GPIO20 | Target TX |
| INA219 SDA | **GPIO5** | INA219 SDA |
| INA219 SCL | **GPIO6** | INA219 SCL |
| INA219 power | 3V3 | Sensor VCC |
| Common ground | GND | Sensor and target GND |
| USB Serial/JTAG | Onboard USB-C | Power, flashing, and USB console |

GPIO8/9 are unused in this preset; the activity LED and JTAG are disabled.
The native USB pins, GPIO18/19, are not reassigned to external peripherals.
Do not assign the same GPIO to SWD, UART, and I2C simultaneously.

Cross-connect UART signals: adapter TX → target RX, adapter RX ← target TX.
This is a UART using board logic levels, not an electrical RS-232 interface.
Devices using different levels require appropriate level conversion.

### 3.2. Sensor and load circuit

```text
ESP32-C3                  INA219 / MCU-219
3V3 --------------------- VCC
GND --------------------- GND
GPIO5 ------------------- SDA
GPIO6 ------------------- SCL

Supply (+) ------- VIN+ [INA219 shunt] VIN- ----- Load (+)
Supply (-) ------------------------------------ Load (-)
     |
     +------------------- common GND
```

VCC powers the sensor itself. VIN+/VIN− belong to the measured circuit:
the load current must flow through the shunt between these terminals.
Load-side bus voltage is measured at VIN− relative to common GND.

Defaults:

| Parameter | Value |
|---|---|
| I2C frequency | 100 kHz |
| INA219 address | `0x40` |
| Shunt resistance | 100 mΩ = 0.1 Ω, marked R100 |
| Background sampling | Every 50 ms, nominally 20 samples/s |
| Averaging window | 1000 ms |
| History capacity | 200 entries |

If the fitted shunt differs, change `CONFIG_ESP_INA219_SHUNT_MILLIOHMS`.
This coefficient determines the calculated current and power. Operating
limits depend on the INA219, shunt, and module hardware; refer to the
[TI datasheet](https://www.ti.com/lit/ds/symlink/ina219.pdf).

The onboard OLED shares SDA=GPIO5 and SCL=GPIO6. On the tested board,
a scan detects `0x3C` and `0x40`; the display remains inactive.
I2C pull-ups must match the 3.3 V logic supply.

With no load connected, the measurement inputs may float. Nonzero readings
in that condition do not establish accuracy or zero calibration. The firmware
does not force those readings to zero.

<a id="quick-start"></a>
## 4. Quick start

Run host shell commands **from the repository root**. Replace the device
address and USB port with your own values. `192.168.1.178` is an example
address observed during local tests; DHCP may assign another address.

### 4.1. Host tools

Building requires ESP-IDF; the tested version is **6.0.2**. Use a local
installation or the previously downloaded Podman image:

```text
docker.io/espressif/esp-matter:latest_idf_v6.0.2
```

A separate Python environment is convenient for USB flashing and terminal use:

```sh
python3 -m venv build/host-venv
. build/host-venv/bin/activate
python -m pip install 'esptool==4.12.0'

export DEVICE_IP=192.168.1.178
export USB_PORT=/dev/cu.usbmodem1101
```

This esptool version was used for hardware verification and installs pyserial
as a dependency. The network Python scripts use the standard library.
ADC plotting additionally requires `matplotlib`; the UART PTY helper requires
`socat`. Shell examples target macOS/Linux.

### 4.2. Build and install over USB

Start the existing Podman VM if it is stopped, then run:

```sh
./host/build_oled_podman.sh
python host/flash_firmware.py --port "$USB_PORT"
python -m serial.tools.miniterm "$USB_PORT" 115200
```

Exit miniterm with `Ctrl+]`. Close the terminal before the next USB flash
operation so that two processes do not use the same port.

For the first migration from a single-app firmware layout, read about the
[OTA partition layout](#ota) and [backups](#usb-flash) first.

### 4.3. Connect to Wi-Fi

Enter the following **in the ESP32 console**, not in the host shell:

```text
wifi "YOUR_SSID" "YOUR_PASSWORD" wpa2
reboot
```

After reboot, find `IP address:` in the USB log. Then, on the host:

```sh
nc "$DEVICE_IP" 4440
```

At the `esp32>` prompt:

```text
status
power scan
power
power interval 1000
power history 10
ota status
```

### 4.4. Check connectivity

Close other CMSIS-DAP/UART clients and run:

```sh
python host/check_wifi.py "$DEVICE_IP" --power 10
```

This checks CMSIS-DAP framing, adapter-level SWD connect/disconnect, UART TCP
connectivity and its TX counter, followed by ten INA219 measurements.
No target board is required. This test does not establish physical UART RX
operation or access to the target's memory.

<a id="build"></a>
## 5. Building and configuration

### 5.1. Building with Podman

[host/build_oled_podman.sh](host/build_oled_podman.sh) builds only the OLED preset:

```sh
podman machine list
podman images
./host/build_oled_podman.sh
```

On macOS, start an existing stopped VM with `podman machine start`.
The build script does not start the VM itself. It uses `--pull=never`, so the
image must already be available locally. The repository is mounted at
`/project`; outputs are written to the local `build/` directory.

The esp-matter image changes its working directory in its entrypoint.
The script therefore explicitly runs `cd /project` before `idf.py` and sets
`IDF_PRESET=board_esp32c3_oled_042`.

To change configuration using the same image:

```sh
podman run --rm -it --pull=never \
  -v "$PWD:/project" -w /project \
  docker.io/espressif/esp-matter:latest_idf_v6.0.2 \
  bash -c 'cd /project && IDF_PRESET=board_esp32c3_oled_042 idf.py menuconfig build'
```

USB flashing runs on the host: the Linux VM on macOS does not automatically
receive access to `/dev/cu.usbmodem1101`.

### 5.2. Local ESP-IDF installation

Install the tools using the
[official ESP-IDF instructions](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/index.html).
After activating the environment, select **one** preset:

```sh
. /path/to/esp-idf/export.sh
cmake --list-presets
export IDF_PRESET=board_esp32c3_oled_042
idf.py menuconfig
idf.py build
idf.py -p "$USB_PORT" flash
idf.py -p "$USB_PORT" monitor
```

For another board, change `IDF_PRESET` to a name from the table. Do not rely
on implicit preset selection: the first preset belongs to an ESP32-S3 board.

### 5.3. Configuration storage

| File | Purpose |
|---|---|
| `sdkconfig.defaults` | Common initial configuration values |
| `sdkconfig.board_*` | Defaults for the selected board |
| `build/build_<board>/sdkconfig` | Effective configuration for that build |
| `main/Kconfig.projbuild` | Project options exposed in `menuconfig` |
| `CMakePresets.json` | Target, build directory, and sdkconfig paths |

**An existing sdkconfig takes precedence over changed defaults.** If a build
was configured before OTA or pin assignments changed, check it in
`menuconfig` or configure a fresh build using the appropriate board defaults.

Important OLED preset options:

| Symbol | Value or purpose |
|---|---|
| `CONFIG_ESP_UART_BRIDGE_1_ENABLED` | UART bridge #1 enabled by common defaults |
| `CONFIG_ESP_UART_BRIDGE_1_TXD_PIN`, `...RXD_PIN` | 21 / 20 |
| `CONFIG_ESP_INA219_ENABLED` | Enable `power` and background history |
| `CONFIG_ESP_INA219_SDA_PIN`, `...SCL_PIN` | 5 / 6 |
| `CONFIG_ESP_INA219_ADDRESS` | `0x40`; configurable from `0x40` to `0x4F` |
| `CONFIG_ESP_INA219_SHUNT_MILLIOHMS` | 100; configurable from 1 to 10000 mΩ |
| `CONFIG_ESP_DAP_OTA_ENABLED` | Enable the `ota` command |
| `CONFIG_PARTITION_TABLE_CUSTOM` | Use a custom partition table |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `partitions.ota_4mb.csv` |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | Enable bootloader rollback |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` | CA bundle for HTTPS |
| `CONFIG_ESP_ADC_STREAM_ENABLED` | Optional ADC stream; disabled by default |
| `CONFIG_ESP_DAP_2_ENABLED`, `...3_ENABLED` | Additional interfaces; disabled by default |

Most project options are under **CMSIS-DAP configuration**. Partition and
bootloader settings are in their respective ESP-IDF menus.

![Example menuconfig screen](img/menuconfig1.png)

### 5.4. Build outputs

For the OLED preset, `build/build_esp32c3_oled_042/` contains:

| File | Use |
|---|---|
| `cmsis_dap_tcp_esp32.bin` | Application image for OTA |
| `cmsis_dap_tcp_esp32.elf` | Adapter application ELF for analysis/debugging |
| `bootloader/bootloader.bin` | Bootloader for initial USB installation |
| `partition_table/partition-table.bin` | Binary partition table |
| `ota_data_initial.bin` | Initial OTA metadata for USB installation |
| `flasher_args.json` | Exact files, offsets, and USB flashing parameters |
| `project_description.json` | Build metadata, including application version |

The application version is derived from the Git state. It is separate from
CMSIS-DAP firmware version `2.1.2`, returned by DAP_Info. `ota status` and the
ESP-IDF startup log show the application version.

<a id="usb-flash"></a>
## 6. USB flashing and backup

Before USB operations, close miniterm, `screen`, monitor, and any other process
using the same serial port. On the tested macOS setup, the board exposes
`/dev/cu.usbmodem1101` and `/dev/tty.usbmodem1101`; the suffix may differ on
another host.

```sh
ls /dev/*usbmodem*
python host/flash_firmware.py --port "$USB_PORT"
```

[flash_firmware.py](host/flash_firmware.py) reads `flasher_args.json`, invokes
esptool for the selected chip target, and writes all listed files.
For a different configuration, specify its build directory:

```sh
python host/flash_firmware.py --port "$USB_PORT" \
  --build-dir build/build_esp32c3_super_mini
```

USB installation of the OTA preset also writes `ota_data_initial.bin`.
This reinitializes boot selection; it is not an ordinary OTA update.
Use OTA for subsequent updates of an already configured board.

### 6.1. Back up before changing the layout

Example for a **4 MB ESP32-C3**:

```sh
python -m esptool --chip esp32c3 --port "$USB_PORT" --baud 460800 \
  read_flash 0x0 0x400000 build/pre-ota-flash.bin
```

The resulting file should be 4194304 bytes. It contains the entire flash,
including NVS Wi-Fi settings, so keep it local. The `build/` directory is
excluded from Git by `.gitignore`.

To restore this exact full backup to the same board:

```sh
python -m esptool --chip esp32c3 --port "$USB_PORT" --baud 460800 \
  write_flash 0x0 build/pre-ota-flash.bin
```

Restoring a full backup replaces all partitions, including current NVS settings.
During hardware OTA verification, the original full backup was saved locally
as `build/verification/pre-ota-flash.bin`; that file is not part of the repository.

<a id="network"></a>
## 7. Wi-Fi, network services, and the console

### 7.1. Services and ports

| Default TCP port | Service | OLED preset state |
|---|---|---|
| 4440 | Command console | Enabled |
| 4441 | CMSIS-DAP #1 | SWD enabled |
| 4442 | UART bridge #1 | UART1, 115200-8-N-1 |
| 4443 / 4445 | CMSIS-DAP #2 / #3 | Disabled |
| 4444 / 4446 | UART bridge #2 / #3 | Disabled |
| 4451 | ADC streaming | Disabled |

Ports are configurable in `menuconfig`. Consider possible conflicts: for
example, port 4444 is often used by the OpenOCD telnet console **on the host**,
whereas this table describes services **on the ESP32**.

With IPv6 enabled, CMSIS-DAP creates a dual-stack listener. The TCP console,
UART bridges, and ADC stream use IPv4. The OTA host scripts expect IPv4;
IPv6 addresses in `status` do not imply IPv6 support in every service.

OTA does not open a separate incoming HTTP port on the ESP32. A console
command starts the update, and the adapter downloads from the specified
HTTP/HTTPS server.

### 7.2. Network configuration

```text
wifi "YOUR_SSID" "YOUR_PASSWORD" wpa2
reboot
```

Credentials in NVS take precedence. If none are stored, the firmware uses
`CONFIG_ESP_WIFI_SSID` and `CONFIG_ESP_WIFI_PASSWORD`. To return to build defaults:

```text
wifi "" ""
reboot
```

For an open network:

```text
wifi "GUEST_SSID" "" open
reboot
```

Supported `auth_mode` values are `open`, `wep`, `wpa`, `wpa2`, and `wpa3`.
If omitted, the default is `wpa2`. The current command handler accepts an
SSID of up to 31 bytes and a password of up to 63 bytes. The command echoes
the supplied password, so do not publish its complete console transcript.

Network services start after DHCP assigns an address. If Wi-Fi is lost after
a network service starts, the firmware reboots the adapter to restore its
sockets. This also clears the RAM power history.

### 7.3. Using the TCP console

```sh
nc "$DEVICE_IP" 4440
```

Wait for `esp32>`, then enter commands one line at a time. The console may echo
input and emit terminal-detection escape sequences. When parsing responses,
select JSON lines instead of treating the entire response as a JSON document.

For automation, use the `Console` class in [host/check_wifi.py](host/check_wifi.py).
It waits for the prompt and keeps the connection open until the command
finishes. This is more reliable than sending a line and immediately closing
stdin or the socket.

<a id="commands"></a>
## 8. Complete command reference

Command availability depends on build configuration. Registration and argument
validation are implemented in [main/commands.c](main/commands.c). Use `help`
to see the commands enabled in the running firmware.

| Command | Result | Persistence and application |
|---|---|---|
| `help` | List enabled commands | No settings changed |
| `status` | Network, addresses, interfaces, connections, and UART counters | Current snapshot |
| `wifi "<ssid>" "<password>" [auth_mode]` | Configure Wi-Fi | NVS; applied after `reboot` |
| `wifi "" ""` | Remove NVS Wi-Fi settings | Build defaults used after reboot |
| `uart <instance> <baud> <bits> <parity> <stops>` | Configure a UART bridge | NVS and an attempt to apply immediately |
| `power` | One fresh INA219 reading | Not added to background history |
| `power scan` | Scan I2C on the configured SDA/SCL pins | No settings changed |
| `power interval` | Interval, sampling period, and buffered count | JSON status |
| `power interval <ms>` | Set a 100–3600000 ms averaging window | RAM; a new window starts immediately |
| `power history` | All retained averages, up to 200 records | Non-destructive read |
| `power history <count>` | Latest 1–200 records | Returned oldest to newest |
| `ota` or `ota status` | Version, slots, update state, and progress | JSON status |
| `ota <URL> <SHA256>` | Download and verify a new application | Slot switch and reboot on success |
| `adc <gpio> <output_sps> <averaging> <format>` | Configure the ADC stream | NVS; applied on a new connection |
| `reboot` | Restart the adapter | RAM history and its interval are lost |

Command names are case-sensitive. The power command is **`power`**;
the old name `ina219` is no longer registered. INA219 identifiers in JSON
still describe the sensor type and remain unchanged.

### What survives a reboot

| Setting or data | Storage | After reboot / OTA |
|---|---|---|
| Wi-Fi SSID, password, auth mode | NVS | Retained |
| UART settings configured with `uart` | NVS | Retained |
| ADC GPIO/rate/averaging/format | NVS, when the module is enabled | Retained |
| `power interval` setting | RAM | Restored to 1000 ms |
| Ring history, sequence, and uptime | RAM | Restart from the beginning |
| OTA progress and last error | RAM | Reset |
| OTA slot selection and boot validation | `otadata` | Retained by the bootloader |
| GPIOs, TCP ports, and INA219 shunt value | Compiled application settings | Changed by rebuilding |

### 8.1. UART arguments

- `instance`: enabled bridge number 1, 2, or 3, not a GPIO number.
- `baud`: a positive UART baud rate; `0` removes stored settings.
- `bits`: 7 or 8.
- `parity`: `n`, `e`, or `o`; case-insensitive for this argument.
- `stops`: 1 or 2.

```text
uart 1 115200 8 n 1
uart 1 9600 8 n 1
uart 1 0 8 n 1
```

After a command with `baud=0`, subsequent connections use build defaults.
It does not promise immediate reconfiguration of an already connected UART
client. An ordinary nonzero baud change is persisted and applied to the
running UART if its driver has already started.

<a id="power"></a>
## 9. Power measurements and the ring buffer

### 9.1. One-shot measurement

```text
power
```

Example response format, with illustrative values:

```json
{"sensor":"INA219","address":64,"bus_voltage_v":3.300,"shunt_voltage_mv":10.000,"current_ma":100.000,"power_mw":330.000,"shunt_milliohms":100}
```

| Field | Unit and meaning |
|---|---|
| `sensor` | Sensor model |
| `address` | Decimal I2C address: 64 = `0x40` |
| `bus_voltage_v` | Voltage at VIN− relative to GND, V |
| `shunt_voltage_mv` | Signed VIN+ − VIN− drop, mV |
| `current_ma` | Signed current through the configured shunt, mA |
| `power_mw` | `bus_voltage_v × current_ma`, mW |
| `shunt_milliohms` | Shunt resistance coefficient from build configuration, mΩ |

The command triggers a conversion and waits for data to become ready.
The INA219 calibration/current/power registers are not used. Current is
calculated from signed shunt voltage and the physical shunt resistance:

```text
current_ma = shunt_voltage_mv × 1000 / shunt_milliohms
power_mw   = bus_voltage_v × current_ma
```

A negative sign corresponds to reverse current according to the VIN+/VIN−
connection, or to a negative measured offset. I2C failures, conversion timeouts,
configuration readback mismatches, and overflow flags produce an explicit
error instead of a fabricated zero reading.

### 9.2. Background averaging

After standalone firmware starts, a separate task samples the sensor every
50 ms, even with no console connected. The default window is one second:

```text
power interval 1000
power interval
```

Example status:

```json
{"type":"ina219_interval","interval_ms":1000,"sample_period_ms":50,"count":12,"capacity":200,"timebase":"uptime_ms"}
```

For a five-second window, use `power interval 5000`. The range is 100 to
3600000 ms: 0.1 seconds to one hour. Supply an integer value. The underlying
sampling period remains 50 ms and is not changed by this command.

Only successful **background** measurements contribute to each average.
With uniform sampling, a 1000 ms window normally contains 20 samples.
Concurrent manual readings, I2C scans, and scheduling delays may reduce that
number.

Mean power is the mean of individual `V × I` measurements, not the product
of mean voltage and mean current. These can differ when the load changes.

### 9.3. Recording and reading history

```text
power history
power history 10
```

The response starts with a header, followed by one JSON line per completed
interval:

```json
{"type":"ina219_history","count":1,"capacity":200,"timebase":"uptime_ms"}
{"type":"ina219_average","sequence":11,"timestamp_ms":11007,"start_ms":10000,"end_ms":11000,"interval_ms":1000,"samples":20,"errors":0,"bus_voltage_v":3.300,"shunt_voltage_mv":10.000,"current_ma":100.000,"power_mw":330.000}
```

| Record field | Meaning |
|---|---|
| `sequence` | Completed interval number since this boot |
| `timestamp_ms` | Actual time the record was inserted into the buffer |
| `start_ms`, `end_ms` | Window boundaries, `[start_ms, end_ms)` |
| `interval_ms` | Averaging interval used for this record |
| `samples` | Number of successful measurements in the average |
| `errors` | Number of failed background measurement attempts |
| Remaining measurement fields | Mean bus voltage, shunt voltage, current, and power |

All times are **milliseconds since ESP32 boot**, not Unix timestamps or UTC.
A conversion belongs to a window according to its completion time. If
scheduling delays insertion, `timestamp_ms` can be later than `end_ms`.

Buffer rules:

1. Only completed windows are stored; the current partial window is not returned.
2. Capacity is 200 records. Each new record overwrites the oldest once full.
3. Requested latest-N records are returned in chronological order.
4. Reads do not consume records. Output uses a snapshot, so a slow TCP client
   does not hold the buffer lock while the response is sent.
5. Changing the interval discards only the incomplete window. Existing records
   remain and may have different `interval_ms` values; numbering continues.
6. Reboot clears history, numbering, and the interval setting.

At a one-second interval, the buffer holds approximately the latest 200
seconds; at five seconds, it holds 1000 seconds. A window without successful
samples has `samples:0` and `null` measurement fields. Do not interpret such
a window as zero power consumption.

### 9.4. Exporting JSONL from the host

This example saves only average records to a local file:

```sh
python - "$DEVICE_IP" > build/power-history.jsonl <<'PY'
import json
import sys
sys.path.insert(0, "host")
from check_wifi import Console

console = Console(sys.argv[1], 4440)
try:
    response = console.command("power history")
    for line in response.splitlines():
        if line.startswith("{"):
            row = json.loads(line)
            if row.get("type") == "ina219_average":
                print(json.dumps(row, ensure_ascii=False))
finally:
    console.close()
PY
```

This exports the current buffer contents; it does not start a continuous
subscription. For long-running logging, poll frequently enough and remove
duplicates using `sequence` within a single device boot. Sequence numbers
restart after reboot.

<a id="ota"></a>
## 10. OTA installation, updates, and rollback

### 10.1. 4 MB flash layout

Source: [partitions.ota_4mb.csv](partitions.ota_4mb.csv).

| Partition | Type / subtype | Offset | Size |
|---|---|---|---|
| `nvs` | data / nvs | `0x9000` | `0x6000`, 24 KiB |
| `phy_init` | data / phy | `0xF000` | `0x1000`, 4 KiB |
| `otadata` | data / ota | `0x10000` | `0x2000`, 8 KiB |
| `ota_0` | app / ota_0 | `0x20000` | `0x1E0000`, 1920 KiB |
| `ota_1` | app / ota_1 | `0x200000` | `0x1E0000`, 1920 KiB |

NVS and PHY retain their offsets from the old single-app layout. The first
OTA-capable installation requires USB because it needs a new bootloader,
partition table, initial metadata, and application image. The OTA command
does not change the partition table, bootloader, or NVS.

When migrating an existing build, check these settings:

```text
CONFIG_ESP_DAP_OTA_ENABLED=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.ota_4mb.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
```

Back up the board, build, and install once over USB. Do not attempt to install
the first OTA-capable image on an old single-app layout by replacing only
the application.

### 10.2. Updating with one command

On an adapter that already has an OTA-compatible layout, build the new app
and run:

```sh
./host/build_oled_podman.sh
python host/ota_update.py "$DEVICE_IP"
```

The [host/ota_update.py](host/ota_update.py) helper:

1. Reads the application `.bin` and calculates its SHA-256.
2. Checks that the device has a next OTA slot and that the image fits.
3. Starts a temporary HTTP server on the host's LAN address. It exposes only
   the selected image at `/firmware.bin`, not the entire project directory.
4. Sends the URL and hash through the console on port 4440.
5. Monitors download progress, the slot switch, and new-boot confirmation.
6. Stops the local server on completion or error.

Script arguments:

| Argument | Default or purpose |
|---|---|
| `host` | Required adapter IPv4 address |
| `--firmware PATH` | `build/build_esp32c3_oled_042/cmsis_dap_tcp_esp32.bin` |
| `--bind IP` | Host LAN address; detected automatically when omitted |
| `--port PORT` | A free port is selected when omitted |
| `--timeout SECONDS` | 120 seconds to wait for update and confirmation |

```sh
python host/ota_update.py "$DEVICE_IP" \
  --firmware build/build_esp32c3_oled_042/cmsis_dap_tcp_esp32.bin \
  --bind 192.168.1.10 --port 8000 --timeout 180
```

The adapter must be able to reach the host's HTTP server. Check routing,
firewall rules, and Wi-Fi client isolation. This helper uses console port
4440 and does not provide an argument for a different console port.

### 10.3. Starting an update manually

On the host, calculate the **application image's** hash:

```sh
shasum -a 256 build/build_esp32c3_oled_042/cmsis_dap_tcp_esp32.bin
```

Start a server in the directory containing that file, or use your own server.
For a manual test, Python's HTTP server supports `--directory`; point it only
at the directory containing the published firmware. In the ESP32 console:

```text
ota status
ota http://192.168.1.10:8000/cmsis_dap_tcp_esp32.bin <SHA256>
```

Replace `<SHA256>` with the complete 64-character hexadecimal hash. URLs may
contain up to 180 characters. The server must return HTTP 200 and a positive
`Content-Length`. Redirects and responses using only chunked transfer encoding
are rejected. Both `http://` and `https://` are supported; HTTPS uses the
ESP-IDF CA bundle. TLS certificate time checks, when enforced by the TLS
configuration, require a valid device clock. A separate SNTP configuration is
not implemented here. Local hardware testing used HTTP.

Updates are asynchronous: the console returns `OTA started`, after which it
can disconnect. Only one OTA task may run at a time. A successful download
and verification trigger a reboot.

### 10.4. Verification and rollback behavior

- The image is written only to the inactive application slot.
- Length, header, and project name are checked, followed by SHA-256 of the
  complete downloaded file and ESP-IDF image validation, including chip,
  revision, and image integrity checks.
- Boot selection changes only after these checks succeed.
- Download failures, truncation, and hash mismatches leave the current boot
  selection unchanged. The inactive slot's contents may already be overwritten.
- On its first boot, a new OTA image enters `pending_verify`.
- After creating startup tasks, the firmware waits ten seconds before
  confirming the boot. A startup-service creation failure requests rollback;
  a reset or crash before confirmation also causes bootloader rollback.
- This checks startup, not complete target-board or INA219 functionality.

Ordinary commands remain available during downloads, but flash writes can
affect timing and sampling coverage. A successful OTA update reboots the
board, disconnects clients, and clears RAM history.

### 10.5. `ota status`

Example after a successful boot:

```json
{"type":"ota_status","state":"idle","error":"ESP_OK","bytes":0,"total":0,"version":"f860c5a","running":"ota_1","boot":"ota_1","next":"ota_0","slot_size":1966080,"validation":"valid"}
```

| Field | Meaning |
|---|---|
| `state` | `idle`, `downloading`, `rebooting`, or `failed` for the current boot |
| `error` | Last OTA error name, or `ESP_OK` when none |
| `bytes`, `total` | Bytes written and expected file size |
| `version` | Running application version |
| `running` | Partition currently executing the application |
| `boot` | Partition selected for the next boot |
| `next` | Partition offered for the next update |
| `slot_size` | Its size in bytes |
| `validation` | `pending_verify`, `valid`, or `undefined` |

After reboot, progress returns to `idle`; this does not mean the update was
lost. Check the version, `running`, and `validation`. Another OTA command is
not accepted while the current image awaits boot confirmation.

<a id="openocd"></a>
## 11. Connecting OpenOCD

Use an OpenOCD build with the **CMSIS-DAP TCP backend**. Support for the USB
CMSIS-DAP driver alone is insufficient. If `cmsis-dap backend tcp` is not
recognized, check the capabilities of your OpenOCD build.

Example `cmsis-dap-tcp.cfg` for the OLED preset:

```tcl
adapter driver cmsis-dap
cmsis-dap backend tcp
cmsis-dap tcp host 192.168.1.178
cmsis-dap tcp port 4441
cmsis-dap tcp min_timeout 300
transport select swd
adapter speed 1000
reset_config none
```

Here, `reset_config none` demonstrates a setup without a hardware reset line.
GPIO2 can be connected to target reset; configure OpenOCD according to the
target's requirements.

Example for an external STM32F1, using the appropriate target scripts from
the installed OpenOCD distribution:

```sh
openocd -f ./cmsis-dap-tcp.cfg -f target/stm32f1x.cfg
```

To program the **target STM32**, not the ESP32 adapter:

```sh
openocd -f ./cmsis-dap-tcp.cfg -f target/stm32f1x.cfg \
  -c "program target_firmware.elf verify reset exit"
```

A different target requires its corresponding target configuration. Start
at a moderate clock speed, then tune it for the wiring and target.
`adapter speed` is in kHz. Increase `cmsis-dap tcp min_timeout` for unstable
network connections.

On a preset with an activity LED, it can be controlled manually:

```tcl
cmsis-dap cmd 0x01 0x00 0x00
cmsis-dap cmd 0x01 0x00 0x01
```

The OLED preset disables the LED, so these commands do not switch on its
screen or an activity LED. For JTAG, select a preset with suitable GPIO
assignments and JTAG support enabled.

<a id="uart"></a>
## 12. UART over Wi-Fi

TCP port 4442 carries the UART1 byte stream. The OLED preset uses GPIO21 TX
and GPIO20 RX, initially configured for 115200 baud, eight data bits, no
parity, and one stop bit. This is raw TCP without Telnet commands or RFC2217
baud-rate configuration.

Connect from the host:

```sh
nc "$DEVICE_IP" 4442
```

For applications that require a local serial port, use
[host/uart_bridge.sh](host/uart_bridge.sh):

```sh
HOST="$DEVICE_IP" PORT=4442 FILE=/tmp/tty_uart sh host/uart_bridge.sh
```

The script requires `socat` and stays running. In another terminal:

```sh
screen /tmp/tty_uart 115200
```

The remote UART settings are controlled by `uart` on console port 4440,
not by the baud rate selected for the local PTY. In `status`, `TX` counts
bytes forwarded by the adapter to UART, while `RX` counts bytes received
from UART. Counters belong to the current connection and reset when it closes.

To test physical TX/RX independently, disconnect the target, connect GPIO21
to GPIO20, and check that sent bytes are returned. The basic `check_wifi.py`
uses only TCP and the TX counter; it does not automatically enable loopback.

<a id="adc"></a>
## 13. Internal ADC streaming

ADC streaming is a separate experimental module. It does not use INA219 and
is not the `power` ring-buffer reader. It is disabled by default in the OLED
preset.

Enable `CONFIG_ESP_ADC_STREAM_ENABLED=y`, then select an unused GPIO belonging
to an **ADC1** channel on your board. Do not use GPIOs assigned to SWD, UART,
or I2C. This module does not support ADC2.

Syntax:

```text
adc <gpio> <output_sps> <averaging> <format>
```

- `output_sps`: output measurements per second after averaging.
- `averaging`: a power of two from 1 to 1024.
- `format`: `text` or `binary16`; `bin` is also accepted.
- Raw sample rate = `output_sps × averaging`. It must fit the chip target's
  `SOC_ADC_SAMPLE_FREQ_THRES_LOW/HIGH` limits.
- `output_sps=0` removes stored NVS settings. The remaining arguments must
  still be present in the command.
- Settings take effect on the next client connection; the command does not
  reconfigure the current stream.

The stream uses TCP port 4451 and starts sampling when a client connects.
Formats:

| Format | Data |
|---|---|
| `text` | Integer mV, one measurement per line |
| `binary16` | Signed 16-bit little-endian mV |

Additional curvature, gain, and offset corrections are build-time parameters
under `CONFIG_ESP_ADC_STREAM_CORRECTION_*`. To display the stream:

```sh
python -m pip install matplotlib
python host/adc_stream_plot.py "$DEVICE_IP" --port 4451 --format auto \
  --window 500 --interval 50
```

`--rate` sets the plot's time scale; it does not change the device sample rate.
`--secondary-label`, `--secondary-slope`, and `--secondary-offset` add a second
axis using a linear transformation of the measured voltage.

![Original ADC streaming example](img/adc_streaming.png)

<a id="verification"></a>
## 14. Verification and tests

### 14.1. Logic tests without hardware

A C compiler is required:

```sh
cc -std=c11 -Wall -Wextra -Werror -I main \
  main/ina219_history.c tests/test_ina219_history.c \
  -o /tmp/test_ina219_history
/tmp/test_ina219_history
```

Tests cover signed values, mean power, measurement errors, window boundaries,
empty windows, interval changes, record ordering, and ring-buffer overwrite.

### 14.2. Network services

```sh
python host/check_wifi.py "$DEVICE_IP" --power 10
```

Use `--dap-port`, `--uart-port`, or `--console-port` if firmware ports differ
from the defaults. The check sends 27 test bytes to UART; account for that
if a connected device interprets incoming bytes as commands.

A successful result establishes:

- DAP_Info, capabilities, and adapter-level SWD connect/disconnect;
- connection to the UART bridge and an increase in its TX counter;
- TCP console responses and the requested number of INA219 readings.

It does not establish SWD operation on external wires, target-memory access,
or physical UART reception.

### 14.3. Hardware history test

```sh
python host/check_ina219_history.py "$DEVICE_IP"
```

The test checks 1000 ms averaging and invalid arguments, then temporarily
selects a 100 ms interval and waits 21 seconds to overwrite the entire ring.
It checks 200 records, timestamps, ordering, and latest-N reads. On exit it
attempts to restore the original interval. Previous history is overwritten.
The script uses the default console port, 4440.

### 14.4. OTA

The positive test is performed by `ota_update.py` itself: download, slot
switch, and transition from `pending_verify` to `valid`.

```sh
python host/check_ota_failures.py "$DEVICE_IP"
```

This additional test covers invalid arguments, HTTP 404, invalid headers,
truncated bodies, SHA mismatches, and concurrent OTA requests. It does not
select the test image for boot, but it overwrites the **inactive slot**.
Do not run it if that slot's old image is needed for rollback.

### 14.5. Hardware verification performed

The following local checks were performed on an ESP32-C3 OLED rev. 0.4 with
4 MB flash:

| Check | Hardware test result |
|---|---|
| ESP-IDF 6.0.2 build and USB flash | Successful; written hashes verified |
| INA219 GPIO5/GPIO6, address `0x40` | Successful one-shot readings through USB/Wi-Fi |
| One-second averages | 20 successful samples in each checked window |
| 200-entry history | Filling, overwrite, ordering, and repeated reads checked |
| HTTP OTA | Download to the other slot and boot confirmation succeeded |
| OTA failures | Invalid hash, truncation, HTTP 404, and invalid image rejected |
| Rollback | Reset during `pending_verify` restored the previous valid slot |
| CMSIS-DAP | Protocol and adapter SWD connect/disconnect checked |
| UART | TCP and 27 transmitted bytes checked through the firmware counter |

These local tests **did not verify** INA219 accuracy under load, physical
UART RX/loopback, external target-memory access, HTTPS transport, or physical
power removal during OTA. Do not confuse these results with the upstream
performance measurements below.

Local reports and logs are under `build/verification/` when saved during
testing. They are not tracked by Git; the main report is `result.txt`.
Changes in this working copy are saved as local commits without pushing
to a Git remote.

<a id="troubleshooting"></a>
## 15. Troubleshooting

| Symptom | What to check |
|---|---|
| Podman cannot connect to its socket | VM state (`podman machine list`) and whether it has started |
| Container image not found | Exact image tag; the build script intentionally does not pull |
| ESP32-S3 builds instead of C3 | Explicitly set `IDF_PRESET=board_esp32c3_oled_042` |
| Changed board defaults have no effect | Effective `build/.../sdkconfig` takes precedence; inspect menuconfig |
| esptool cannot open the USB port | Port name, data-capable cable, and closed miniterm/screen/monitor sessions |
| Wi-Fi does not connect | SSID, password, and auth mode; NVS credentials override defaults |
| Device IP changed | USB log or DHCP lease; optionally reserve its address on the router |
| Connection dropped and history is empty | Wi-Fi loss after service startup triggers reboot; history is RAM-only |
| `power` is not recognized | `CONFIG_ESP_INA219_ENABLED` must be enabled; `ina219` is no longer a command |
| `power scan` finds no devices | VCC=3V3, common GND, GPIO5 SDA, GPIO6 SCL, contacts, and pull-ups |
| `0x3C` appears but `0x40` does not | The OLED bus is visible; check external sensor power, wiring, and address |
| Sensor address differs | Set `CONFIG_ESP_INA219_ADDRESS` and rebuild |
| Error at `stage=trigger` | I2C write failed; check the address and scan first |
| Error at `stage=config` | Register verification failed; confirm the device is an INA219 and the bus is stable |
| Error at `stage=conversion` | Conversion did not complete, or bus reads failed |
| Negative current with no load | Floating inputs are not a zero reference; check the load circuit and shunt |
| Latest measurement is missing from history | Only completed windows appear; manual `power` readings do not enter history |
| A record has `samples:0` and `null` values | No successful samples in that window; inspect the sensor and `errors` |
| Timestamp does not look like a date | It is uptime in milliseconds, not Unix time |
| `ota` is not recognized | An OTA-enabled build and first USB installation of the new layout are required |
| OTA returns `ESP_ERR_INVALID_STATE` | Another OTA is running, or the current boot is still `pending_verify` |
| OTA returns `ESP_ERR_INVALID_SIZE` | HTTP 200, positive Content-Length, and image size within the slot |
| OTA returns `ESP_ERR_INVALID_RESPONSE` | Header/project mismatch; use this project's application `.bin` |
| OTA returns `ESP_ERR_INVALID_CRC` | SHA-256 mismatch; hash the exact file being served |
| OTA cannot download from the host | LAN IP, bind interface, port, firewall, and Wi-Fi client isolation |
| OTA returns to the old slot | A reset or failed startup occurred before confirmation; inspect the USB log |
| OpenOCD does not recognize the TCP backend | An OpenOCD build with that backend is required |
| CMSIS-DAP interface is busy | Close another OpenOCD or diagnostic client |
| UART TX increases but no reply arrives | Crossed RX/TX, GND, baud/bits/parity/stops, and target operation |
| ADC command is missing | Enable the separate `CONFIG_ESP_ADC_STREAM_ENABLED` module |

For additional CMSIS-DAP logs, enable `CONFIG_ESP_DAP_TCP_DEBUG_PRINTING` in
`menuconfig`. Debug logging affects performance. Driver and startup messages
are available through USB.

<a id="internals"></a>
## 16. Project structure and component integration

### 16.1. Main files

| Path | Purpose |
|---|---|
| [main/main.c](main/main.c) | Standalone startup: NVS, power monitor, console, Wi-Fi, network tasks, and OTA confirmation |
| [main/commands.c](main/commands.c) | Shared USB/TCP command handlers and argument validation |
| [main/cmsis_dap_tcp.c](main/cmsis_dap_tcp.c) | CMSIS-DAP TCP framing and interface tasks |
| [main/DAP_config.h](main/DAP_config.h) | CMSIS-DAP integration with ESP32 GPIOs and settings |
| [main/DAP_gpio_config.c](main/DAP_gpio_config.c) | Interface GPIO configuration |
| [main/wifi.c](main/wifi.c) | Wi-Fi, DHCP events, NVS network settings, and recovery by reboot |
| [main/socket_console.c](main/socket_console.c) | TCP console with a task per connection |
| [main/uart_bridge.c](main/uart_bridge.c) | UART-to-TCP forwarding, line settings, and counters |
| [main/ina219.c](main/ina219.c) | INA219 reads, background sampling, status, and history output |
| [main/ina219_history.c](main/ina219_history.c) | Averaging and the ring buffer without ESP-IDF dependencies |
| [main/ota_update.c](main/ota_update.c) | HTTP/HTTPS download, SHA-256, slot selection, and boot confirmation |
| [main/adc_stream.c](main/adc_stream.c) | Separate ADC1 stream |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | Project menuconfig options |
| [main/idf_component.yml](main/idf_component.yml) | Component Manager dependencies: led_strip and esp_linenoise |
| [host/](host/) | Build, flash, OTA, verification, UART PTY, and ADC plot helpers |
| [tests/test_ina219_history.c](tests/test_ina219_history.c) | Native averaging and history tests |
| [partitions.ota_4mb.csv](partitions.ota_4mb.csv) | OLED preset partition table |

The current `.gitignore` excludes `build/`, generated sdkconfig files,
`managed_components/`, `dependencies.lock`, and `.cache/`. Enter personal
Wi-Fi credentials through the console instead of adding them to board defaults.

### 16.2. Startup and concurrency

The standalone application initializes NVS, starts the INA219 monitor and USB
console, connects to Wi-Fi, then creates CMSIS-DAP, UART, optional-module,
and TCP-console tasks. A new OTA image then waits and confirms its boot.

Background sampling, manual measurements, and scans share an I2C lock.
History uses a separate lock, and network output is sent from a snapshot.
OTA runs in its own task without retaining a client console pointer.
Each CMSIS-DAP interface owns its task-specific protocol state.

CMSIS-DAP TCP prefixes each command with an eight-byte header. Multibyte
fields use little-endian encoding:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Signature `0x00504144`, bytes `44 41 50 00` (`DAP\0`) |
| 4 | 2 | CMSIS-DAP payload length, excluding the header |
| 6 | 1 | Type: `1` request, `2` response |
| 7 | 1 | Reserved, `0` |

The header is followed by a binary CMSIS-DAP payload. TCP does not preserve
message boundaries, so clients must assemble headers and bodies across
partial reads. Payload size is bounded by `CONFIG_ESP_DAP_TCP_MAX_PKT_SIZE`,
4096 bytes by default. See `dap_request()` in
[host/check_wifi.py](host/check_wifi.py) for a framing example. The `power`/`ota`
console uses a separate port and a text protocol; do not send console lines
to the CMSIS-DAP port.

### 16.3. Integrating with another ESP-IDF project

The `main/` directory can be consumed as a component. If `CMAKE_PROJECT_NAME`
is not `cmsis_dap_tcp_esp32`, the component omits its own `main.c`. The host
application is responsible for NVS, networking, and starting required services.

The main APIs are described in their headers:

| API | Usage requirement |
|---|---|
| `cmsis_dap_tcp_start(...)` | The supplied config must remain valid for the task's lifetime; use static storage |
| `uart_bridge_start(...)` | Separate config and available UART/GPIOs for each instance |
| `ina219_monitor_start()` | Start background averaging before commands access history |
| `ina219_set_interval(...)` | Set the interval in milliseconds after monitor startup |
| `ina219_print_sample(...)`, `ina219_print_history(...)` | Write a reading or snapshot to the supplied `FILE *` |
| `ota_update_start(...)` | Requires networking, two OTA slots, and suitable rollback configuration |
| `ota_update_confirm_boot(...)` | The integrating application must call it after its startup health check |

Disable `CONFIG_ESP_DAP_MANAGE_WIFI` if the integrating application manages
networking itself. Call `cmsis_dap_tcp_start()` after networking is ready.
Check task-start return values. Additional interfaces must not conflict in
ports or GPIO assignments.

The upstream project also references an
[ESPHome wrapper](https://github.com/w531t4/ESPHome-cmsis_dap_tcp_esp32-Wrapper).

<a id="upstream"></a>
## 17. Performance and upstream origins

The figures below are retained from the original README. They are historical
measurements on other adapter/target combinations, not new benchmarks of the
ESP32-C3 OLED running background monitoring and OTA.

| Adapter | CPU | Upstream measured max SWCLK | Average SRAM write | Average SRAM read |
|---|---|---|---|---|
| ESP32-S3 DevKitC-1 | 240 MHz, Xtensa | 5.3 MHz | About 200 KB/s | About 80 KB/s |
| ESP32-C3 SuperMini | 160 MHz, RISC-V | 5.0 MHz | About 175 KB/s | About 80 KB/s |
| XIAO ESP32-C6 alt | 160 MHz, RISC-V | 1.4 MHz | About 90 KB/s | About 50 KB/s |

Upstream tests reported approximately 13.4 seconds to flash 512 KB to an
STM32F401RE over SWD using an S3, and approximately 20 seconds using a C6,
including erase/program/verify. Actual performance depends on the target,
clock, wiring, and network; JTAG is generally slower than SWD.

![Upstream performance chart](img/performance.svg)

![SWD waveform example 1](img/scopeshot1.png)

![SWD waveform example 2](img/scopeshot2.png)

The CMSIS-DAP implementation originated from
[ARM CMSIS-DAP](https://github.com/ARM-software/CMSIS-DAP), firmware v2.1.2,
commit `1fd47bed772ea40923472c90dfe11516e76033ee`, with GPIO adaptations for ESP32.
The project source credits Brian Kuschak and other contributors. The original
documentation credits [w531t4](https://github.com/w531t4) for component
integration and the ESPHome wrapper.

The project is distributed under [Apache License 2.0](LICENSE). Preserve
copyright headers and the license terms of dependencies.

Additional primary references:

- [ESP-IDF OTA documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/ota.html).
- [ESP-IDF HTTP client documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/protocols/esp_http_client.html).
- [TI INA219 datasheet](https://www.ti.com/lit/ds/symlink/ina219.pdf).
