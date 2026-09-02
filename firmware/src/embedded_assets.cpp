#include "embedded_assets.h"

#include <Arduino.h>
#include <FSImpl.h>
#include <cstring>
#include <memory>

// src/generated_assets.h is produced by tools/convert_assets.py and is
// not committed. Without it this translation unit still compiles -- the
// firmware just has no embedded assets and needs the SD card.
#if __has_include("generated_assets.h")
#include "generated_assets.h"
#define HAVE_GENERATED_ASSETS 1
#endif

namespace
{

// Minimal fs::FileImpl over a fixed span of flash. Read-only; directory
// and write operations are inert. Enough for the sequential read + seek
// the asset loaders in main.cpp do.
class MemFileImpl : public fs::FileImpl
{
public:
    MemFileImpl(const uint8_t *data, size_t len) : _d(data), _len(len), _pos(0) {}

    size_t read(uint8_t *buf, size_t size) override
    {
        size_t avail = _len - _pos;
        size_t n = size < avail ? size : avail;
        memcpy(buf, _d + _pos, n);
        _pos += n;
        return n;
    }

    bool seek(uint32_t pos, fs::SeekMode mode) override
    {
        size_t target;
        switch (mode)
        {
        case fs::SeekSet:
            target = pos;
            break;
        case fs::SeekCur:
            target = _pos + pos;
            break;
        case fs::SeekEnd:
            target = _len + pos; // pos is normally 0 here
            break;
        default:
            return false;
        }
        if (target > _len)
            return false;
        _pos = target;
        return true;
    }

    size_t position() const override { return _pos; }
    size_t size() const override { return _len; }
    void close() override {}
    operator bool() override { return _d != nullptr; }

    size_t write(const uint8_t *, size_t) override { return 0; }
    void flush() override {}
    bool setBufferSize(size_t) override { return false; }
    time_t getLastWrite() override { return 0; }
    const char *path() const override { return ""; }
    const char *name() const override { return ""; }
    boolean isDirectory() override { return false; }
    fs::FileImplPtr openNextFile(const char *) override { return fs::FileImplPtr(); }
    boolean seekDir(long) override { return false; }
    String getNextFileName() override { return String(); }
    String getNextFileName(bool *) override { return String(); }
    void rewindDirectory() override {}

private:
    const uint8_t *_d;
    size_t _len;
    size_t _pos;
};

} // namespace

bool haveEmbeddedAssets()
{
#ifdef HAVE_GENERATED_ASSETS
    return GENERATED_ASSET_COUNT > 0;
#else
    return false;
#endif
}

fs::File openEmbeddedAsset(const char *path)
{
#ifdef HAVE_GENERATED_ASSETS
    for (int i = 0; i < GENERATED_ASSET_COUNT; ++i)
    {
        if (strcmp(path, GENERATED_ASSET_TOC[i].path) == 0)
        {
            return fs::File(std::make_shared<MemFileImpl>(
                GENERATED_ASSET_BLOB + GENERATED_ASSET_TOC[i].offset,
                GENERATED_ASSET_TOC[i].length));
        }
    }
#else
    (void)path;
#endif
    return fs::File();
}
