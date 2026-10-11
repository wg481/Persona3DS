#pragma once
// sceAtrac3plus (Persona3DS fork): the ATRAC3 / ATRAC3plus player library.
//
// Port of the state logic of uOFW src/kd/libatrac3plus/libatrac3plus.c (MIT,
// Copyright (c) 2011, 2012 The uOFW team; addresses in comments are uOFW's),
// operating on guest memory. The codec call (sceAudiocodecDecode) is replaced
// by silence: no ATRAC3 / ATRAC3plus decoder exists in the workspace, so every
// decoded frame is zero samples while all container-level values (IDs, buffer
// states, stream positions, loops, remaining frames) follow the library.
// Checked against pspautotests tests/audio/atrac (hardware) by
// tests/test_fork_atrac.cpp; where hardware and uOFW disagree, the hardware
// result is used and the difference is noted at the change.
//
// Persona (ULUS-10432) imports 14 of these (first run's import diff); the
// others are modelled because the hardware tests need them.
#include <cstdint>

namespace psprecomp { class Runtime; class GuestMemory; }
namespace p3p3ds { class KernelState; }

namespace p3p3ds::hle::atrac {

constexpr std::uint16_t kCodecAt3Plus = 0x1000u, kCodecAt3 = 0x1001u;
constexpr int kMaxIds = 6;

// uOFW SceAtracIdInfo (libatrac3plus.h), as host fields.
struct IdInfo {
    std::uint32_t decodePos{}, endSample{}, loopStart{}, loopEnd{};
    std::int32_t samplesPerChan{};   // first valid sample (PPSSPP: firstValidSample)
    std::int8_t numFrame{};          // frames still to skip
    std::int8_t state{};             // -1 free, 0 unused, 1 no data, 2..6 buffer states, 8 low level, 16 sas
    std::int8_t unk22{};             // current buffer: 0 first, 1 second, 2 done
    std::int8_t numChan{};
    std::uint16_t sampleSize{}, codec{};
    std::uint32_t dataOff{}, curOff{}, dataEnd{};
    std::int32_t loopNum{};
    std::uint32_t streamDataByte{}, unk48{}, unk52{}; // unk48/unk52: stream offsets in the first/second buffer
    std::uint32_t buffer{}, secondBuffer{}, bufferByte{}, secondBufferByte{};
    std::int32_t codecErr{};         // SceAudiocodecCodec.err (sceAtracGetInternalErrorInfo)
};

// uOFW SceBufferInfo.
struct BufferInfo {
    std::uint32_t writePositionFirstBuf{}, writableByteFirstBuf{}, minWriteByteFirstBuf{}, readPositionFirstBuf{};
    std::uint32_t writePositionSecondBuf{}, writableByteSecondBuf{}, minWriteByteSecondBuf{}, readPositionSecondBuf{};
};

class Library {
public:
    Library() { (void)reinit(2, 2); } // sceAtracStartEntry: sceAtracReinit(2, 2)

    std::int32_t reinit(int numAt3, int numAt3Plus);
    std::int32_t get_atrac_id(std::uint32_t codecType);
    std::int32_t release_atrac_id(int id);
    std::int32_t set_halfway_buffer(psprecomp::GuestMemory &m, int id, std::uint32_t buffer, std::uint32_t readByte, std::uint32_t bufferByte);
    std::int32_t set_halfway_buffer_and_get_id(psprecomp::GuestMemory &m, std::uint32_t buffer, std::uint32_t readByte, std::uint32_t bufferByte);
    std::int32_t decode_data(psprecomp::GuestMemory &m, int id, std::uint32_t out, std::uint32_t *samples, std::uint32_t *finish, std::int32_t *remain);
    std::int32_t get_remain_frame(int id, std::int32_t *remain) const;
    std::int32_t get_stream_data_info(int id, std::uint32_t *writePtr, std::uint32_t *writable, std::uint32_t *readPosition) const;
    std::int32_t add_stream_data(int id, std::uint32_t addByte);
    std::int32_t get_second_buffer_info(int id, std::uint32_t *position, std::uint32_t *dataByte) const;
    std::int32_t set_second_buffer(int id, std::uint32_t secondBuffer, std::uint32_t secondBufferByte);
    std::int32_t get_next_sample(int id, std::uint32_t *next) const;
    std::int32_t get_sound_sample(int id, std::int32_t *end, std::int32_t *loopStart, std::int32_t *loopEnd) const;
    std::int32_t get_next_decode_position(int id, std::uint32_t *position) const;
    std::int32_t get_loop_status(int id, std::int32_t *loopNum, std::uint32_t *loopStatus) const;
    std::int32_t set_loop_num(int id, std::int32_t loopNum);
    std::int32_t get_buffer_info_for_resetting(psprecomp::GuestMemory &m, int id, std::uint32_t sample, BufferInfo *info);
    std::int32_t reset_play_position(psprecomp::GuestMemory &m, int id, std::uint32_t sample, std::uint32_t writeFirst, std::uint32_t writeSecond);
    std::int32_t is_second_buffer_needed(int id) const;
    std::int32_t get_channel(int id, std::uint32_t *channel) const;
    std::int32_t get_output_channel(int id, std::uint32_t *channel) const;
    std::int32_t get_max_sample(int id, std::uint32_t *max) const;
    std::int32_t get_bitrate(int id, std::uint32_t *bitrate) const;
    std::int32_t get_internal_error_info(int id, std::int32_t *result) const;

    [[nodiscard]] const IdInfo *info(int id) const noexcept { return id >= 0 && id < kMaxIds ? &ids_[id] : nullptr; }
    void set_state_for_test(int id, std::int8_t state) noexcept { ids_[id].state = state; }

private:
    IdInfo ids_[kMaxIds]{};
    [[nodiscard]] bool usable(int id) const noexcept { return id >= 0 && id < kMaxIds && ids_[id].state > 0; }
    std::int32_t set_halfway(psprecomp::GuestMemory &m, IdInfo &id, std::uint32_t buffer, std::uint32_t readByte, std::uint32_t bufferByte);
    std::int32_t reset_id(psprecomp::GuestMemory &m, IdInfo &id);
    std::int32_t decode_frame(psprecomp::GuestMemory &m, IdInfo &id, std::uint32_t out, std::uint32_t *samples, std::uint32_t *finish);
};

// Registers the sceAtrac3plus imports on `kernel`'s library instance.
void register_fork_atrac(psprecomp::Runtime &runtime, KernelState &kernel);
[[nodiscard]] Library &fork_atrac_library() noexcept;

} // namespace p3p3ds::hle::atrac
