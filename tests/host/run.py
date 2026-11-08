#!/usr/bin/env python3
"""Run actual persistence code against a NOR-flash simulator (after `pio run`)."""
from pathlib import Path
import subprocess
import os
import tempfile

root = Path(__file__).resolve().parents[2]
nanopb = root / '.pio/libdeps/dc1/Nanopb'
if not nanopb.is_dir():
    raise SystemExit('Install the firmware dependencies first: pio run')
with tempfile.TemporaryDirectory(prefix='dc1-tests-') as build:
    binary = str(Path(build) / 'persistence')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    littlefs = core / 'packages/framework-arduinoespressif8266/libraries/LittleFS/lib/littlefs'
    objects = []
    for name in ['lfs', 'lfs_util']:
        obj = str(Path(build) / (name + '.o'))
        subprocess.run(['gcc', '-std=c99', '-DLFS_NAME_MAX=32', '-DLFS_NO_DEBUG',
                        '-DLFS_NO_WARN', '-DLFS_NO_ERROR', '-I' + str(littlefs),
                        '-c', str(littlefs / (name + '.c')), '-o', obj], check=True)
        objects.append(obj)
    command = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-DPB_FIELD_16BIT=1',
               '-no-pie', '-Wl,--defsym,_EEPROM_start=0x402fb000',
               '-Wl,--defsym,_RELAY_STATE_start=0x402fa000',
               '-Itests/host/stubs', '-Ilib/esp_framework/include', '-Iinclude',
               '-I' + str(nanopb), '-I' + str(littlefs), '-DLFS_NAME_MAX=32',
               'tests/host/persistence.cpp', 'tests/host/littlefs_adapter.cpp',
               'lib/esp_framework/src/Config.cpp', 'lib/esp_framework/src/GlobalConfig.pb.c',
               'src/RuntimeStorage.cpp', 'src/DC1Config.pb.c',
               str(nanopb / 'pb_common.c'), str(nanopb / 'pb_encode.c'),
               str(nanopb / 'pb_decode.c'), *objects, '-o', binary]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([binary], check=True)
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
assert symbols['_irom0_text_end'] <= symbols['_FS_start']
image_size = (root / '.pio/build/dc1/firmware.bin').stat().st_size
rounded_size = (image_size + 4095) & ~4095
update_start = symbols['_FS_start'] - 0x40200000 - rounded_size
assert rounded_size <= update_start
print('PASS: runtime/configuration layout and space for same-size OTA image')
