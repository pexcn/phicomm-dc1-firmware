#pragma once

#include <Updater.h>
#include <stdint.h>
#include <string.h>

// Shared with scripts/name-firmware.py; change for incompatible hardware/layouts.
#define FIRMWARE_ID "phicomm-dc1-firmware:esp01_1m:1"
// Config slot B occupies 0x74000..0x74fff. Bound both the running image and
// the core's top-down OTA staging allocation so neither can touch this slot.
#define FIRMWARE_MAX_SIZE 0x74000U

class FirmwareUpload
{
    static const size_t TRAILER_SIZE = 68; // Identity[32], LE image size, hex MD5[32].
    uint8_t tail[TRAILER_SIZE] = {};
    size_t used = 0;
    size_t written = 0;

    bool writeImage(uint8_t *data, size_t size)
    {
        // Published images are raw ESP8266 binaries. A compressed image could
        // expand over the reserved config sector even when its upload fits.
        if (written == 0 && data[0] != 0xe9) return false;
        // Leave one byte unused so end() without 'true' can only cancel.
        if (written + size >= Update.size()) return false;
        if (Update.write(data, size) != size) return false;
        written += size;
        return true;
    }

public:
    static size_t capacity(size_t available)
    {
        return available < FIRMWARE_MAX_SIZE ? available : FIRMWARE_MAX_SIZE;
    }

    bool begin(size_t capacity)
    {
        cancel();
        used = written = 0;
        return Update.begin(FirmwareUpload::capacity(capacity), U_FLASH);
    }

    bool write(uint8_t *data, size_t size)
    {
        if (written + used + size > FIRMWARE_MAX_SIZE) return false;
        if (used + size <= TRAILER_SIZE)
        {
            memcpy(tail + used, data, size);
            used += size;
            return true;
        }
        size_t release = used + size - TRAILER_SIZE;
        size_t fromTail = release < used ? release : used;
        size_t fromInput = release - fromTail;
        if (fromTail && !writeImage(tail, fromTail)) return false;
        if (fromInput && !writeImage(data, fromInput)) return false;
        memmove(tail, tail + fromTail, used - fromTail);
        memcpy(tail + used - fromTail, data + fromInput, size - fromInput);
        used = TRAILER_SIZE;
        return true;
    }

    bool finish()
    {
        const char identity[32] = FIRMWARE_ID;
        uint32_t imageSize = uint32_t(tail[32]) | uint32_t(tail[33]) << 8
            | uint32_t(tail[34]) << 16 | uint32_t(tail[35]) << 24;
        if (used != TRAILER_SIZE || written == 0 || imageSize != written
            || memcmp(tail, identity, sizeof(identity)) != 0)
        {
            cancel();
            return false;
        }
        char md5[33] = {};
        memcpy(md5, tail + 36, 32);
        for (size_t i = 0; i < 32; ++i)
            if (!((md5[i] >= '0' && md5[i] <= '9') || (md5[i] >= 'a' && md5[i] <= 'f')))
            {
                cancel();
                return false;
            }
        // ESP8266 core verifies the payload before writing the boot command.
        bool ok = Update.setMD5(md5) && Update.end(true);
        if (!ok) cancel();
        return ok;
    }

    void cancel()
    {
        if (Update.isRunning()) Update.end();
    }
};
