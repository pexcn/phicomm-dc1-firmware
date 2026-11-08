#pragma once
#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
extern "C" {
#include "lfs.h"
}

struct LittleFSConfig { explicit LittleFSConfig(bool value = true) : autoFormat(value) {} bool autoFormat; };
class TestFile;
class TestLittleFS
{
public:
    std::array<uint8_t, 65536> flash;
    std::array<unsigned, 16> erases = {};
    lfs_t fs = {};
    lfs_config config = {};
    bool mounted = false, autoFormat = true, powerOff = false;
    bool failRename = false, shortWrite = false, corruptWrite = false;
    bool failMount = false;
    int failAfter = -1;
    unsigned mutations = 0, formatCalls = 0;
    TestLittleFS();
    void setConfig(const LittleFSConfig &value) { autoFormat = value.autoFormat; }
    bool begin();
    void end();
    bool format();
    TestFile open(const char *path, const char *mode);
    bool exists(const char *path);
    bool rename(const char *from, const char *to);
    bool mutation();
};
extern TestLittleFS LittleFS;

class TestFile
{
    struct Handle
    {
        lfs_file_t file = {};
        bool opened = false;
        ~Handle() { if (opened) lfs_file_close(&LittleFS.fs, &file); }
    };
    std::shared_ptr<Handle> handle;
public:
    TestFile() {}
    TestFile(const char *path, int flags)
    {
        auto candidate = std::make_shared<Handle>();
        if (lfs_file_open(&LittleFS.fs, &candidate->file, path, flags) == 0)
        {
            candidate->opened = true;
            handle = candidate;
        }
    }
    explicit operator bool() const { return handle && handle->opened; }
    size_t size() { return lfs_file_size(&LittleFS.fs, &handle->file); }
    int read(uint8_t *data, size_t size)
    { return lfs_file_read(&LittleFS.fs, &handle->file, data, size); }
    size_t write(const uint8_t *data, size_t size)
    {
        if (LittleFS.shortWrite) { LittleFS.shortWrite = false; --size; }
        std::vector<uint8_t> copy(data, data + size);
        if (LittleFS.corruptWrite) { LittleFS.corruptWrite = false; copy[0] ^= 1; }
        int result = lfs_file_write(&LittleFS.fs, &handle->file, copy.data(), copy.size());
        return result < 0 ? 0 : result;
    }
    void flush() { lfs_file_sync(&LittleFS.fs, &handle->file); }
    void close()
    {
        if (*this) { lfs_file_close(&LittleFS.fs, &handle->file); handle->opened = false; }
    }
};
