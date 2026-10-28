#pragma once
// P3P3DS Builder pipeline: user's ISO of the profile's game -> .3dsx (profile_info.hpp).
//
//   1. read EBOOT.BIN and PARAM.SFO from the ISO, check the disc ID
//   2. decrypt EBOOT.BIN (psp_eboot.cpp), or (P3P3DS) take the user's decrypted
//      EBOOT dump (PPSSPP) when the built-in decryptor does not know the game's
//      key; check the reference SHA-256
//   3. generate the AOT C++ units (bundled psp_recomp, the profile's options)
//   4. compile them with the user's devkitARM, one job per core
//   5. link with the prebuilt runtime objects shipped in sdk/lib
//   6. smdhtool + 3dsxtool (romfs: the decrypted EBOOT)
//   7. P3P3DS: convert the music and movies the output folder does not have yet
//      (media.cpp, bundled ffmpeg), into <output folder>/media
//   8. P3P3DS: optionally the .cia (bundled bannertool and makerom)
//
// Everything the game is not involved in (runtime, HLE, GPU renderer) is
// prebuilt in the release package; only code derived from the user's own
// executable is generated and compiled on the user's machine. Steps 3-4 are
// cached in the work directory, so an interrupted build resumes.
//
// Runs on Windows (CreateProcessW, job object) and on POSIX hosts
// (posix_spawn); tool names get the host executable suffix.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace p3p3ds::builder {

struct Settings {
    std::filesystem::path iso, devkitpro, sdk, work, output; // output: the .3dsx file
    std::filesystem::path eboot;  // P3P3DS: decrypted EBOOT (PPSSPP dump); empty: decrypt the ISO's
    std::filesystem::path banner; // P3P3DS: folder with banner.png, audio.wav, icon.png for the .cia; empty: sdk/cia
    bool cia{};                   // P3P3DS: also build <output>.cia
    unsigned jobs{};              // 0: automatic
};

struct Progress {
    std::string step;        // short user-facing description
    double fraction{};       // 0..1 over the whole build
    double eta_seconds{-1};  // < 0: unknown
    unsigned units_done{}, units_total{};
};

class Pipeline {
public:
    using Report = std::function<void(const Progress &)>;
    using Log = std::function<void(const std::string &)>;
    Pipeline(Settings settings, Report report, Log log);
    // Runs the whole build; throws std::runtime_error with a user-facing message.
    void run();
    void cancel() { cancelled_ = true; }

    // Validation helpers for the wizard.
    static std::string check_iso(const std::filesystem::path &iso);           // "" when it is the profile's disc
    static std::string check_devkitpro(const std::filesystem::path &root);    // "" when usable
    // P3P3DS: "" when the file is the profile's decrypted executable.
    static std::string check_eboot(const std::filesystem::path &eboot);
    // P3P3DS: true when the built-in decryptor reads this ISO's EBOOT (no dump needed).
    static bool can_decrypt(const std::filesystem::path &iso);
    // P3P3DS: the converted music and movies next to the .3dsx.
    static std::filesystem::path media_folder(const std::filesystem::path &output) { return output.parent_path() / "media"; }
    static std::filesystem::path cia_file(const std::filesystem::path &output) {
        return std::filesystem::path(output).replace_extension(".cia");
    }
    static std::filesystem::path find_devkitpro();                            // best guess, may be empty
    static unsigned automatic_jobs();

private:
    Settings s_;
    Report report_;
    Log log_;
    std::atomic<bool> cancelled_{false};
    std::filesystem::path elf_, gen_, obj_;
    void *job_{}; // Windows job object: child tools die with the builder (unused on POSIX)

    void decrypt();
    void generate();
    void compile();
    void link_and_package();
    void convert_media();  // P3P3DS
    void make_cia();       // P3P3DS
    void progress(const std::string &step, double fraction, double eta = -1, unsigned done = 0, unsigned total = 0);
    // argv[0] is the tool's full path; stdout and stderr go to log_file.
    int run_tool(const std::vector<std::filesystem::path> &argv, const std::filesystem::path &log_file);
};

} // namespace p3p3ds::builder
