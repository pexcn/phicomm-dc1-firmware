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
    assert(argc == 2);
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
}
