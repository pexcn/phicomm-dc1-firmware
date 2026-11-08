#include "LittleFS.h"

static int readBlock(const lfs_config *cfg, lfs_block_t block, lfs_off_t offset,
                     void *data, lfs_size_t size)
{
    auto &fs = *static_cast<TestLittleFS *>(cfg->context);
    if (fs.powerOff) return LFS_ERR_IO;
    assert(block < 16 && offset + size <= 4096);
    memcpy(data, fs.flash.data() + block * 4096 + offset, size);
    return 0;
}
static int programBlock(const lfs_config *cfg, lfs_block_t block, lfs_off_t offset,
                        const void *data, lfs_size_t size)
{
    auto &fs = *static_cast<TestLittleFS *>(cfg->context);
    if (fs.powerOff) return LFS_ERR_IO;
    assert(block < 16 && offset + size <= 4096);
    bool failed = fs.mutation();
    const uint8_t *source = static_cast<const uint8_t *>(data);
    size_t count = failed ? size / 2 : size;
    for (size_t i = 0; i < count; ++i) fs.flash[block * 4096 + offset + i] &= source[i];
    return failed ? LFS_ERR_IO : 0;
}
static int eraseBlock(const lfs_config *cfg, lfs_block_t block)
{
    auto &fs = *static_cast<TestLittleFS *>(cfg->context);
    if (fs.powerOff) return LFS_ERR_IO;
    assert(block < 16);
    bool failed = fs.mutation();
    ++fs.erases[block];
    memset(fs.flash.data() + block * 4096, 0xff, 4096);
    return failed ? LFS_ERR_IO : 0;
}
static int syncBlock(const lfs_config *cfg)
{ return static_cast<TestLittleFS *>(cfg->context)->powerOff ? LFS_ERR_IO : 0; }

TestLittleFS LittleFS;
TestLittleFS::TestLittleFS()
{
    flash.fill(0xff);
    config.context = this;
    config.read = readBlock;
    config.prog = programBlock;
    config.erase = eraseBlock;
    config.sync = syncBlock;
    config.read_size = config.prog_size = config.cache_size = 64;
    config.block_size = 4096;
    config.block_count = 16;
    config.block_cycles = 16;
    config.lookahead_size = 64;
}
bool TestLittleFS::mutation()
{
    ++mutations;
    if (failAfter == 0) { powerOff = true; return true; }
    if (failAfter > 0) --failAfter;
    return false;
}
bool TestLittleFS::begin()
{
    if (mounted) return true;
    mounted = !failMount && lfs_mount(&fs, &config) == 0;
    if (!mounted && autoFormat && format()) mounted = lfs_mount(&fs, &config) == 0;
    return mounted;
}
void TestLittleFS::end()
{
    if (mounted) lfs_unmount(&fs);
    mounted = false;
    memset(&fs, 0, sizeof(fs));
}
bool TestLittleFS::format()
{
    ++formatCalls;
    return lfs_format(&fs, &config) == 0;
}
TestFile TestLittleFS::open(const char *path, const char *mode)
{
    if (!mounted) return {};
    int flags = *mode == 'w' ? LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC : LFS_O_RDONLY;
    return TestFile(path, flags);
}
bool TestLittleFS::exists(const char *path)
{
    lfs_info info;
    return mounted && lfs_stat(&fs, path, &info) == 0;
}
bool TestLittleFS::rename(const char *from, const char *to)
{
    return !failRename && lfs_rename(&fs, from, to) == 0;
}
