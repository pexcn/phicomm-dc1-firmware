#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include "FirmwareUpload.h"

UpdaterClass Update;

static bool upload(std::vector<uint8_t> image, size_t chunk, size_t capacity)
{
    FirmwareUpload firmware;
    assert(firmware.begin(capacity));
    for (size_t offset = 0; offset < image.size(); offset += chunk)
    {
        size_t count = std::min(chunk, image.size() - offset);
        if (!firmware.write(image.data() + offset, count))
        {
            firmware.cancel();
            return false;
        }
    }
    return firmware.finish();
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), {});
    assert(image.size() > 68);
    size_t payload = image.size() - 68;
    Update.expected.assign(image.begin(), image.begin() + payload);
    Update.checksum.assign(image.end() - 32, image.end());
    for (size_t chunk : {1, 2, 67, 68, 69, 1436})
    {
        assert(upload(image, chunk, image.size()));
        assert(Update.activated && Update.bytes == Update.expected);
    }
    // Foreign files, damaged payload/identity/length/hash, and truncation fail closed.
    for (size_t offset : {size_t(0), payload, payload + 32, image.size() - 1})
    {
        auto damaged = image;
        damaged[offset] ^= 1;
        assert(!upload(damaged, 1436, image.size()));
        assert(!Update.activated);
    }
    for (size_t size : {size_t(4), payload, image.size() - 1})
    {
        auto truncated = image;
        truncated.resize(size);
        assert(!upload(truncated, 1436, image.size()));
        assert(!Update.activated);
    }
    // A full-capacity or aborted transfer must never schedule a boot command.
    assert(!upload(image, 1436, payload) && !Update.activated);
    FirmwareUpload firmware;
    assert(firmware.begin(image.size()));
    assert(firmware.write(image.data(), image.size() / 2));
    firmware.cancel();
    assert(!Update.running && !Update.activated);
    assert(upload(image, 1436, image.size())); // Retry after failure.
    // Even a generous caller must not place top-down OTA staging over config B.
    for (size_t available : {size_t(FIRMWARE_MAX_SIZE), size_t(0xea000), size_t(1024 * 1024)})
    {
        assert(upload(image, 1436, available));
        assert(Update.capacity == FIRMWARE_MAX_SIZE);
        assert(0xea000 - Update.capacity >= 0x74000 + 4096);
    }
    std::vector<uint8_t> oversized(FIRMWARE_MAX_SIZE + 68, 0xe9);
    assert(!upload(oversized, 1436, 1024 * 1024) && !Update.activated);
    assert(firmware.begin(1024 * 1024));
    oversized.resize(FIRMWARE_MAX_SIZE + 1);
    assert(!firmware.write(oversized.data(), oversized.size()));
    firmware.cancel();
    // Reject even correctly checksummed compressed uploads, whose expanded
    // size cannot be bounded by the raw-image partition limit.
    std::ifstream gzipFile(argv[2], std::ios::binary);
    std::vector<uint8_t> compressed((std::istreambuf_iterator<char>(gzipFile)), {});
    assert(compressed.size() > 68 && compressed[0] == 0x1f && compressed[1] == 0x8b);
    Update.expected.assign(compressed.begin(), compressed.end() - 68);
    Update.checksum.assign(compressed.end() - 32, compressed.end());
    assert(!upload(compressed, 1436, 1024 * 1024) && !Update.activated);
}
