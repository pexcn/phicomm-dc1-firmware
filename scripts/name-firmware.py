Import('env')
from pathlib import Path
import shutil
import hashlib
import re
import struct


def copy_firmware(source, target, env):
    root = Path(env.subst('$PROJECT_DIR'))
    firmware = Path(str(target[0]))
    identity_header = root / 'lib/esp_framework/include/FirmwareUpload.h'
    settings = identity_header.read_text()
    identity = re.search(r'^#define FIRMWARE_ID "([^"]+)"',
                         settings, re.MULTILINE).group(1).encode()
    limit = int(re.search(r'^#define FIRMWARE_MAX_SIZE (0x[0-9a-fA-F]+)U',
                          settings, re.MULTILINE).group(1), 16)
    image = firmware.read_bytes()
    # Keep the output idempotent if a post-action is run twice.
    if len(image) >= 68 and image[-68:-36] == identity.ljust(32, b'\0'):
        size = struct.unpack('<I', image[-36:-32])[0]
        if size == len(image) - 68 and hashlib.md5(image[:size]).hexdigest().encode() == image[-32:]:
            image = image[:size]
    trailer = struct.pack('<32sI32s', identity, len(image), hashlib.md5(image).hexdigest().encode())
    if len(image) + len(trailer) > limit:
        raise RuntimeError('Firmware overlaps the reserved configuration backup sector')
    firmware.write_bytes(image + trailer)
    output = root / 'output'
    output.mkdir(exist_ok=True)
    shutil.copyfile(firmware, output / (env.subst('$PIOENV') + '.bin'))


env.Depends('$BUILD_DIR/${PROGNAME}.bin',
            env.subst('$PROJECT_DIR') + '/lib/esp_framework/include/FirmwareUpload.h')
env.AddPostAction('$BUILD_DIR/${PROGNAME}.bin', copy_firmware)
