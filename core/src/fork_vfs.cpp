// See fork_vfs.hpp.
#include "p3p3ds/fork_vfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace p3p3ds::vfs {
namespace {
constexpr std::uint64_t kSector = 2048u;

class RawSectorSource final : public Source {
public:
    RawSectorSource(std::shared_ptr<std::ifstream> stream, std::shared_ptr<std::mutex> lock, std::uint64_t offset, std::uint64_t size)
        : stream_(std::move(stream)), lock_(std::move(lock)), offset_(offset), size_(size) {}
    [[nodiscard]] std::uint64_t size() const override { return size_; }
    std::size_t read(std::uint64_t offset, void *destination, std::size_t length) const override {
        if (offset >= size_) return 0u;
        length = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset));
        std::lock_guard guard(*lock_);
        stream_->clear();
        stream_->seekg(static_cast<std::streamoff>(offset_ + offset));
        stream_->read(static_cast<char *>(destination), static_cast<std::streamsize>(length));
        return static_cast<std::size_t>(stream_->gcount());
    }

private:
    std::shared_ptr<std::ifstream> stream_;
    std::shared_ptr<std::mutex> lock_;
    std::uint64_t offset_, size_;
};
} // namespace

std::optional<LbnPath> parse_sce_lbn_path(std::string_view path) {
    constexpr std::string_view kPrefix = "sce_lbn";
    if (path.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    const std::string text(path);
    const auto size_at = text.find("_size");
    if (size_at == std::string::npos) return std::nullopt;
    const auto lbn = std::strtoul(text.c_str() + kPrefix.size(), nullptr, 16);
    const auto size = std::strtoul(text.c_str() + size_at + 5u, nullptr, 16);
    return LbnPath{static_cast<std::uint32_t>(lbn), static_cast<std::uint32_t>(size)};
}

LbnIsoFileSystem::LbnIsoFileSystem(std::shared_ptr<const FileSystem> inner, const std::filesystem::path &image)
    : inner_(std::move(inner)), stream_(std::make_shared<std::ifstream>(image, std::ios::binary)), lock_(std::make_shared<std::mutex>()) {
    if (!*stream_) throw std::runtime_error("cannot open UMD image: " + image.string());
}

std::shared_ptr<const Source> LbnIsoFileSystem::open(std::string_view path) const {
    if (const auto lbn = parse_sce_lbn_path(path))
        return std::make_shared<RawSectorSource>(stream_, lock_, static_cast<std::uint64_t>(lbn->lbn) * kSector, lbn->size);
    return inner_->open(path);
}

} // namespace p3p3ds::vfs
