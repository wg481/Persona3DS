// disc0:/sce_lbn raw-sector paths (core/src/fork_vfs.cpp, Persona3DS fork).
#include "p3p3ds/fork_vfs.hpp"
#include "p3p3ds/vfs.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#ifndef P3P_SOURCE_DIR
#define P3P_SOURCE_DIR "."
#endif

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)

// Stand-in for the ISO directory tree: one named file.
class OneFile final : public p3p3ds::vfs::FileSystem {
public:
    [[nodiscard]] std::shared_ptr<const p3p3ds::vfs::Source> open(std::string_view path) const override {
        return path == "NAMED.BIN" ? std::make_shared<p3p3ds::vfs::MemorySource>(std::vector<std::uint8_t>{1, 2, 3}) : nullptr;
    }
    [[nodiscard]] std::optional<p3p3ds::vfs::Entry> stat(std::string_view) const override { return std::nullopt; }
    [[nodiscard]] std::optional<std::vector<p3p3ds::vfs::Entry>> list(std::string_view) const override { return std::nullopt; }
};

std::vector<std::uint8_t> read_all(const p3p3ds::vfs::Source &s) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.size()));
    bytes.resize(s.read(0, bytes.data(), bytes.size()));
    return bytes;
}
} // namespace

int main() {
    using p3p3ds::vfs::parse_sce_lbn_path;
    // uOFW isofs.c isofsParseSceLbnPath.
    { const auto p = parse_sce_lbn_path("sce_lbn0xe020_size0x126b0"); CHECK(p && p->lbn == 0xE020u && p->size == 0x126B0u); } // Persona run 6
    // raw_access.c's format: 33 characters with '/', longer than uOFW's 0x1F limit, accepted on hardware.
    { const auto p = parse_sce_lbn_path("sce_lbn0x0000001A_size0x000001D8"); CHECK(p && p->lbn == 26u && p->size == 472u); }
    { const auto p = parse_sce_lbn_path("sce_lbn1a_size1d8"); CHECK(p && p->lbn == 26u && p->size == 472u); }
    CHECK(!parse_sce_lbn_path("sce_lbn0x10"));                                // no _size
    CHECK(!parse_sce_lbn_path("PSP_GAME/PARAM.SFO"));
    CHECK(!parse_sce_lbn_path("SCE_LBN0x10_size0x20"));                       // strncmp is case-sensitive

    // Synthetic image: sector n filled with byte n.
    const auto dir = std::filesystem::path(P3P_SOURCE_DIR) / ".tmp";
    std::filesystem::create_directories(dir);
    const auto image = dir / "test_fork_vfs.img";
    {
        std::ofstream out(image, std::ios::binary);
        for (int sector = 0; sector < 8; ++sector) out << std::string(2048, static_cast<char>(sector));
    }
    p3p3ds::vfs::LbnIsoFileSystem fs(std::make_shared<OneFile>(), image);
    {
        const auto src = fs.open("sce_lbn0x3_size0x1000");
        CHECK(src && src->size() == 0x1000u);
        const auto bytes = src ? read_all(*src) : std::vector<std::uint8_t>{};
        CHECK(bytes.size() == 0x1000u && bytes.front() == 3u && bytes[2047] == 3u && bytes[2048] == 4u && bytes.back() == 4u);
    }
    CHECK(fs.open("NAMED.BIN") && fs.open("NAMED.BIN")->size() == 3u); // other paths go to the wrapped tree
    CHECK(!fs.open("MISSING.BIN"));
    std::filesystem::remove(image);

    // pspautotests umd/raw_access (hardware: HandleRaw Ok, memcmp_Raw 0): the
    // raw read equals PSP_GAME/PARAM.SFO opened by name. The expected file was
    // recorded with LBN 26 / 472 bytes; in the raw_access.iso shipped with the
    // test, PARAM.SFO's directory record says LBN 25 / 420 bytes, so those are used.
    const auto iso = std::filesystem::path(P3P_SOURCE_DIR) / "references/pspautotests/tests/umd/raw_access/raw_access.iso";
    if (!std::filesystem::exists(iso)) {
        std::cout << "raw_access.iso absent (tools/fetch_references.py): hardware comparison skipped\n";
    } else {
        auto inner = std::make_shared<p3p3ds::vfs::IsoFileSystem>(iso);
        p3p3ds::vfs::LbnIsoFileSystem umd(inner, iso);
        const auto named = umd.open("PSP_GAME/PARAM.SFO"), raw = umd.open("sce_lbn0x00000019_size0x000001A4");
        CHECK(named && raw && named->size() == 420u && raw->size() == 420u);
        CHECK(!inner->open("sce_lbn0x00000019_size0x000001A4")); // without the wrapper: not found (Persona run 6)
        if (named && raw) CHECK(read_all(*named) == read_all(*raw));
        std::cout << "raw_access.iso: raw read compared with PARAM.SFO\n";
    }
    std::cout << "fork vfs checks failures=" << failures << "\n";
    return failures ? 1 : 0;
}
