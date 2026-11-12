#!/usr/bin/env python3
"""Run actual persistence code against a NOR-flash simulator (after `pio run`)."""
from pathlib import Path
import subprocess
import os
import tempfile
import hashlib
import re
import struct
import gzip

root = Path(__file__).resolve().parents[2]
nanopb = root / '.pio/libdeps/dc1/Nanopb'
if not nanopb.is_dir():
    raise SystemExit('Install the firmware dependencies first: pio run')
with tempfile.TemporaryDirectory(prefix='dc1-tests-') as build:
    binary = str(Path(build) / 'persistence')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    littlefs = core / 'packages/framework-arduinoespressif8266/libraries/LittleFS/lib/littlefs'
    objects = []
    # Compile the SDK's actual CRC implementation with the host flash stubs.
    sdk_crc = Path(build) / 'core-crc32.cpp'
    sdk_crc.write_text((core / 'packages/framework-arduinoespressif8266/cores/esp8266/crc32.cpp').read_text())
    for name in ['lfs', 'lfs_util']:
        obj = str(Path(build) / (name + '.o'))
        subprocess.run(['gcc', '-std=c99', '-DLFS_NAME_MAX=32', '-DLFS_NO_DEBUG',
                        '-DLFS_NO_WARN', '-DLFS_NO_ERROR', '-I' + str(littlefs),
                        '-c', str(littlefs / (name + '.c')), '-o', obj], check=True)
        objects.append(obj)
    command = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
               '-no-pie', '-Wl,--defsym,_EEPROM_start=0x402fb000',
               '-Wl,--defsym,_CONFIG_BACKUP_start=0x40274000',
               '-Wl,--defsym,_RELAY_STATE_start=0x402fa000',
               '-Itests/host/stubs', '-Itests/host/auth', '-Ilib/esp_framework/include', '-Iinclude',
               '-I' + str(nanopb), '-I' + str(littlefs), '-DLFS_NAME_MAX=32',
               'tests/host/persistence.cpp', 'tests/host/littlefs_adapter.cpp',
               'lib/esp_framework/src/Config.cpp', 'lib/esp_framework/src/GlobalConfig.pb.c',
               'src/RuntimeStorage.cpp', 'src/DC1Config.pb.c',
               str(nanopb / 'pb_common.c'), str(nanopb / 'pb_encode.c'),
               str(nanopb / 'pb_decode.c'), str(sdk_crc), *objects, '-o', binary]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([binary], check=True)
# Exercise the logger and HTTP escaping with the original fixed buffer sizes.
with tempfile.TemporaryDirectory(prefix='dc1-log-tests-') as build:
    source = (root / 'lib/esp_framework/src/Http.cpp').read_text()
    start = source.index('                    size_t j = 0;', source.index('void Http::handleGetStatus()'))
    end = source.index('                    cflg = true;', start)
    (Path(build) / 'log-json.inc').write_text(source[start:end])
    start = source.index('        bool cflg = ', source.index('void Http::handleGetStatus()'))
    closing = r'        server->sendContent_P(PSTR("\""));'
    end = source.index(closing, start) + len(closing)
    (Path(build) / 'log-batch.inc').write_text(source[start:end])
    binary = str(Path(build) / 'logging')
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-Itests/host/logging', '-Ilib/esp_framework/include', '-I' + build,
                    'tests/host/logging/logging.cpp', 'lib/esp_framework/src/Debug.cpp',
                    '-o', binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
# Compile the real button handler with GPIO/action spies, without the Wi-Fi stack.
with tempfile.TemporaryDirectory(prefix='dc1-button-tests-') as build:
    header = (root / 'include/DC1.h').read_text()
    source = (root / 'src/DC1.cpp').read_text()
    fields = header[header.index('    uint8_t buttonDebounceTime'):header.index('    void checkButton(')]
    handler = source[source.index('void DC1::checkButton('):source.index('void DC1::energyUpdate()')]
    (Path(build) / 'button-fields.inc').write_text(fields)
    (Path(build) / 'button-handler.inc').write_text(handler)
    binary = str(Path(build) / 'buttons')
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I' + build,
                    'tests/host/buttons.cpp', '-o', binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
# Feed physical 4800-baud 8E1 edges through the installed serial library and meter.
with tempfile.TemporaryDirectory(prefix='dc1-meter-tests-') as build:
    binary = str(Path(build) / 'meter')
    serial = root / '.pio/libdeps/dc1/EspSoftwareSerial/src'
    ghostl = root / '.pio/libdeps/dc1/ghostl/src'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    # The dependency's unused TX path assumes a 32-bit unsigned long.
                    '-Wno-overflow', '-Itests/host/meter', '-Iinclude',
                    '-I' + str(serial), '-I' + str(ghostl),
                    'tests/host/meter/meter.cpp', 'src/CSE7766.cpp',
                    str(serial / 'SoftwareSerial.cpp'), '-o', binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
# Exercise the real authentication code with the core's portable BearSSL sources.
bear = core / 'packages/framework-arduinoespressif8266/tools/sdk/ssl/bearssl'
with tempfile.TemporaryDirectory(prefix='dc1-auth-tests-') as build:
    objects = []
    for name in ['hash/sha2small', 'codec/dec32be', 'codec/enc32be']:
        obj = str(Path(build) / (Path(name).name + '.o'))
        subprocess.run(['gcc', '-std=c99', '-I' + str(root / 'tests/host/auth'),
                        '-I' + str(bear / 'inc'), '-I' + str(bear / 'src'),
                        '-c', str(bear / 'src' / (name + '.c')), '-o', obj], check=True)
        objects.append(obj)
    binary = str(Path(build) / 'auth')
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-Itests/host/auth', '-Itests/host/stubs', '-Ilib/esp_framework/include',
                    '-I' + str(bear.parents[1] / 'include'), '-I' + str(bear / 'inc'),
                    'tests/host/auth/auth.cpp', 'lib/esp_framework/src/WebAuth.cpp',
                    *objects, '-o', binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
# Check the published binary envelope and the streaming activation gate.
firmware = root / '.pio/build/dc1/firmware.bin'
image = firmware.read_bytes()
version_header = (root / '.pio/build/dc1/generated/FirmwareVersion.h').read_text()
version = re.search(r'^#define FIRMWARE_VERSION "([^"]+)"', version_header, re.MULTILINE).group(1)
assert re.fullmatch(r'\d{8}-(?:[0-9a-f]{7}(?:-dirty)?|dev)', version)
assert version.encode() + b'\0' in image
assert b'2020.07.11.2000\0' not in image
print('PASS: generated firmware version embedded in the published binary: ' + version)
identity, size, digest = struct.unpack('<32sI32s', image[-68:])
header = (root / 'lib/esp_framework/include/FirmwareUpload.h').read_text()
expected_id = re.search(r'^#define FIRMWARE_ID "([^"]+)"', header, re.MULTILINE).group(1).encode()
assert identity.rstrip(b'\0') == expected_id and size == len(image) - 68
assert hashlib.md5(image[:size]).hexdigest().encode() == digest
assert (root / 'output/dc1.bin').read_bytes() == image
with tempfile.TemporaryDirectory(prefix='dc1-ota-tests-') as build:
    compressed = gzip.compress(image[:size], mtime=0)
    compressed += struct.pack('<32sI32s', identity, len(compressed),
        hashlib.md5(compressed).hexdigest().encode())
    compressed_path = Path(build) / 'compressed.bin'
    compressed_path.write_bytes(compressed)
    binary = str(Path(build) / 'firmware')
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-Itests/host/ota', '-Ilib/esp_framework/include',
                    'tests/host/ota/firmware.cpp', '-o', binary], cwd=root, check=True)
    subprocess.run([binary, str(firmware), str(compressed_path)], check=True)
print('PASS: firmware identity, checksum, chunk boundaries, corruption and aborted uploads')
# Check the linked image, not just the declarations in the linker script.
elf = root / '.pio/build/dc1/firmware.elf'
nm = core / 'packages/toolchain-xtensa/bin/xtensa-lx106-elf-nm'
symbols = {}
for line in subprocess.check_output([str(nm), '-n', str(elf)], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
assert symbols['_FS_start'] == 0x402ea000
assert symbols['_FS_end'] == symbols['_RELAY_STATE_start'] == 0x402fa000
assert symbols['_EEPROM_start'] == 0x402fb000
assert symbols['_CONFIG_BACKUP_start'] == 0x40274000
assert symbols['_irom0_text_end'] <= symbols['_CONFIG_BACKUP_start']
image_size = (root / '.pio/build/dc1/firmware.bin').stat().st_size
rounded_size = (image_size + 4095) & ~4095
update_start = symbols['_FS_start'] - 0x40200000 - rounded_size
limit = int(re.search(r'^#define FIRMWARE_MAX_SIZE (0x[0-9a-fA-F]+)U', header, re.MULTILINE).group(1), 16)
assert image_size <= limit == symbols['_CONFIG_BACKUP_start'] - 0x40200000
assert rounded_size <= limit
assert symbols['_FS_start'] - 0x40200000 - limit >= limit + 4096
assert limit <= update_start
print('PASS: runtime/configuration layout and space for same-size OTA image')
