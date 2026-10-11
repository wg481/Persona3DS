#pragma once
// UMD raw-sector paths for the Persona3DS fork. Kept out of vfs.hpp/vfs.cpp
// so upstream P3P3DS changes keep merging; the runners mount this wrapper
// around their IsoFileSystem.
#include "p3p3ds/vfs.hpp"

#include <cstdint>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace p3p3ds::vfs {

struct LbnPath {
    std::uint32_t lbn{}, size{};
};

// uOFW src/kd/isofs/isofs.c (isofsParseSceLbnPath): "/sce_lbn<hex>_size<hex>";
// both numbers are read with strtol(..., 16), so a "0x" prefix is accepted.
// uOFW also rejects paths longer than 0x1F characters, but pspautotests
// umd/raw_access opens "/sce_lbn0x%08X_size0x%08X" (33 characters) on
// hardware successfully, so that limit is not applied here (the hardware
// result wins; the decompiled check likely applies elsewhere). `path` is the
// normalized device-relative path (no leading '/').
[[nodiscard]] std::optional<LbnPath> parse_sce_lbn_path(std::string_view path);

// disc0:/sce_lbn0x<lbn>_size0x<size> opens `size` bytes from sector `lbn`
// (2,048 bytes) of the image; pspautotests umd/raw_access (hardware) reads
// the same bytes as opening the file by name. Every other path goes to the
// wrapped file system.
class LbnIsoFileSystem final : public FileSystem {
public:
    LbnIsoFileSystem(std::shared_ptr<const FileSystem> inner, const std::filesystem::path &image);
    [[nodiscard]] std::shared_ptr<const Source> open(std::string_view path) const override;
    [[nodiscard]] std::optional<Entry> stat(std::string_view path) const override { return inner_->stat(path); }
    [[nodiscard]] std::optional<std::vector<Entry>> list(std::string_view path) const override { return inner_->list(path); }

private:
    std::shared_ptr<const FileSystem> inner_;
    std::shared_ptr<std::ifstream> stream_;
    std::shared_ptr<std::mutex> lock_;
};

} // namespace p3p3ds::vfs
