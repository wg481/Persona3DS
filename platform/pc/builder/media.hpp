#pragma once
// P3P3DS: music and movie conversion for the Builder. C++ port of tools/convert_media.py with the same
// output: IMA-ADPCM WAVs for the ATRAC music, H.264 Baseline streams with a frame index (.h264 + .idx)
// plus IMA-ADPCM audio for the PSMF movies, and index.txt in the format of
// core/include/p3p3ds/hle/fork_media.hpp. Input files are read from a vfs::FileSystem (the ISO's
// PSP_GAME/USRDIR in the Builder, a host folder in the tests); ffmpeg and ffprobe run through a callback.
// Files that already exist in the output folder are not converted again (as the Python tool without --force).
#include "p3p3ds/vfs.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace p3p3ds::builder::media {

std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size);

struct At3Info {
    std::uint32_t data_off{}, data_size{}, block_align{}, frame_samples{};
    [[nodiscard]] std::uint64_t raw_samples() const { return static_cast<std::uint64_t>(data_size / block_align) * frame_samples; }
};
// Throws std::runtime_error when the data is not an ATRAC3/ATRAC3plus RIFF WAVE file.
At3Info parse_at3(const std::vector<std::uint8_t> &data);

// (offset, size) of each access unit of an Annex B stream, split at access unit delimiters (NAL type 9).
std::vector<std::pair<std::uint32_t, std::uint32_t>> h264_access_units(const std::vector<std::uint8_t> &data);

// The ATRAC3plus stream of a PSMF movie as an OMA file; empty without audio.
std::vector<std::uint8_t> psmf_audio_oma(const std::vector<std::uint8_t> &pmf, std::uint8_t channel = 0);

// Sample offset and sample count of a converted IMA-ADPCM WAV; writes the count into its fact chunk.
// raw_samples < 0: no reference count (movie audio).
std::pair<std::int64_t, std::int64_t> finish_wav(const std::filesystem::path &path, std::int64_t raw_samples);

struct Tools {
    std::filesystem::path ffmpeg, ffprobe, temp; // temp: folder for extracted inputs and tool logs
    // Runs argv (argv[0] = the tool) with stdout and stderr in log_file; returns the exit code.
    std::function<int(const std::vector<std::filesystem::path> &argv, const std::filesystem::path &log_file)> run;
    std::function<void(const std::string &)> log;
    // Called before each file: files done, files in total, name of the next file.
    std::function<void(unsigned done, unsigned total, const std::string &name)> progress;
};

struct Result { unsigned tracks{}, movies{}, converted{}; };

// Converts every .at3 and .pmf under `root` of `input` into `output` and writes output/index.txt.
Result convert(const vfs::FileSystem &input, const std::string &root, const std::filesystem::path &output, const Tools &tools);

} // namespace p3p3ds::builder::media
