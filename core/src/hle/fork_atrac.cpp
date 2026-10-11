// sceAtrac3plus (see fork_atrac.hpp). Port of uOFW libatrac3plus.c state
// logic (MIT, Copyright (c) 2011, 2012 The uOFW team); hex comments are the
// uOFW routine addresses.
#include "p3p3ds/hle/fork_atrac.hpp"

#include "p3p3ds/hle/fork_media.hpp"
#include "p3p3ds/kernel_state.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace p3p3ds::hle::atrac {
namespace {

constexpr std::int32_t kErrNoId = 0x80630005, kErrUnknownFormat = 0x80630006, kErrWrongCodec = 0x80630007,
                       kErrBadCodecParams = 0x80630008, kErrAllDataLoaded = 0x80630009, kErrNoData = 0x80630010,
                       kErrSizeTooSmall = 0x80630011, kErrSecondBufferNeeded = 0x80630012,
                       kErrIncorrectReadSize = 0x80630013, kErrBadAlign = 0x80630014, kErrBadSample = 0x80630015,
                       kErrBadFirstResetSize = 0x80630016, kErrBadSecondResetSize = 0x80630017,
                       kErrAddDataIsTooBig = 0x80630018, kErrNotMonaural = 0x80630019, kErrNoLoopInformation = 0x80630021,
                       kErrSecondBufferNotNeeded = 0x80630022, kErrBufferIsEmpty = 0x80630023,
                       kErrAllDataDecoded = 0x80630024, kErrIsLowLevel = 0x80630031, kErrIsForSas = 0x80630040,
                       kErrNoIdLeft = 0x80630003, kErrInvalidCodec = 0x80630004;

// 3F08: ATRAC3 frame sizes and their codec parameter word.
struct At3Params { std::uint16_t sampleSize; std::uint8_t dataByte; std::uint8_t jointStereo; };
constexpr At3Params kAt3Params[] = {{0x0180, 0x4, 0}, {0x0130, 0x6, 0}, {0x00C0, 0xB, 1}, {0x00C0, 0xE, 0}, {0x0098, 0xF, 0}};

// 3EF8: the ATRAC3plus format GUID in WAVEFORMATEXTENSIBLE.
constexpr std::uint8_t kAt3PlusGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
                                           0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};

// uOFW SceAtracFile.
struct File {
    std::uint16_t channels{}, dataBlkSz{};
    std::uint8_t unk4[2]{};
    std::uint32_t dataOff{}, factSz{}, dataSize{}, samplesPerChan{};
    std::int32_t loopStart{-1}, loopEnd{-1};
};

std::int32_t is_valid_state(std::int8_t state) { // 1938
    if (state == 8) return kErrIsLowLevel;
    if (state == 1) return kErrNoData;
    if (state == 16) return kErrIsForSas;
    return 0;
}

std::uint32_t shift(const IdInfo &i) { return 0x100Bu - i.codec; } // 11 for ATRAC3plus, 10 for ATRAC3
std::uint32_t skip_samples(const IdInfo &i) { return i.codec == kCodecAt3 ? 69u : 368u; }

std::uint32_t get_off_from_sample(const IdInfo &i, std::uint32_t sample) { // 2348
    std::uint32_t off = ((sample >> shift(i)) - 1u) * i.sampleSize;
    if ((sample & (0x7FFu >> (i.codec & 1u))) < skip_samples(i) && off != 0u) off -= i.sampleSize;
    return off + i.dataOff;
}
std::uint32_t get_second_buf_pos(const IdInfo &i, std::uint32_t sample) { // 23A4
    return ((sample >> shift(i)) + 1u) * i.sampleSize + i.dataOff - 1u;
}
std::uint32_t get_frame_from_sample(const IdInfo &i, std::uint32_t sample) { // 23D4
    return (sample & (0x7FFu >> (i.codec & 1u))) < skip_samples(i) ? 2u : 1u;
}
std::uint32_t next_sample_of(const IdInfo &i) { // 240C
    const std::uint32_t mask = 0x7FFu >> (i.codec & 1u);
    const std::uint32_t frameLast = (i.decodePos & ~mask) + mask;
    const std::uint32_t pastEnd = i.endSample >= frameLast ? 0u : frameLast - i.endSample;
    const std::uint32_t used = (i.decodePos & mask) + pastEnd;
    const std::uint32_t perFrame = 0x800u >> (i.codec & 1u);
    return used >= perFrame ? 0u : perFrame - used;
}

std::uint32_t round_from(std::uint32_t base, std::uint32_t grain, std::uint32_t size) { // 3230
    return base >= size ? size : ((size - base) / grain) * grain + base;
}
std::uint32_t sub_2FA8(std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) {
    const std::uint32_t num = a0 + a2;
    if (a0 < a1) a1 = ((a1 - a0) / a3) * a3 + a0;
    if (num < a1) return num;
    if (a1 >= a2) return num - a1;
    return 0u;
}
std::uint32_t sub_2FEC(std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) {
    const std::uint32_t num = a1 + a2;
    return num < a0 && a2 < a3 ? num : 0u;
}
std::uint32_t sub_3004(std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4) {
    if (a1 >= a4) return 0u;
    const std::uint32_t end = a0 + a1;
    if (end < a3 + 1u) return end;
    return a2 + (end - a3 - 1u) % (a3 - a2 + 1u);
}
std::uint32_t sub_316C(std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3) {
    if (a0 < a1) a1 = ((a1 - a0) / a3) * a3 + a0;
    if (a0 + a2 < a1) return a1 - (a0 + a2);
    if (a1 >= a2) return a1 - a2;
    return 0u;
}
std::uint32_t sub_31B4(const IdInfo &i) { // stream bytes still in the first buffer
    if (i.loopEnd < i.decodePos && i.unk22 == 1) {
        const std::uint32_t num1 = i.unk52;
        std::uint32_t num2 = i.secondBufferByte;
        if (num1 < num2) num2 = ((num2 - num1) / i.sampleSize) * i.sampleSize + num1;
        if (num2 < num1) return 0u;
        if (num2 - num1 >= i.streamDataByte) return 0u;
        return i.streamDataByte - (num2 - num1);
    }
    return i.streamDataByte;
}
std::uint32_t sub_30C0(const IdInfo &i) {
    return i.curOff + i.streamDataByte < i.dataEnd ? i.dataEnd - (i.curOff + i.streamDataByte) : 0u;
}
std::uint32_t sub_30E0(const IdInfo &i) {
    const std::uint32_t loopEndPos = get_second_buf_pos(i, i.loopEnd);
    const std::uint32_t sum = i.curOff + i.streamDataByte;
    if (sum >= loopEndPos + 1u) {
        const std::uint32_t loopBytes = loopEndPos - get_off_from_sample(i, i.loopStart) + 1u;
        return loopBytes - (sum - loopEndPos - 1u) % loopBytes;
    }
    return loopEndPos - sum + 1u;
}
std::uint32_t sub_3048(const IdInfo &i) {
    std::int8_t state = i.state;
    if (state == 6) state = i.loopEnd < i.decodePos ? 4 : 5;
    if (state == 4) return sub_30C0(i);
    if (state == 5) return sub_30E0(i);
    return 0u;
}
std::int32_t sub_2DB8(const IdInfo &i) {
    if (i.streamDataByte >= i.dataEnd - i.curOff) return -2;
    return std::max<std::int32_t>(static_cast<std::int32_t>(i.streamDataByte / i.sampleSize) - i.numFrame, 0);
}
std::int32_t sub_2EA4(const IdInfo &i) {
    const std::uint32_t loopEndPos = get_second_buf_pos(i, i.loopEnd);
    const std::uint32_t sum = i.curOff + i.streamDataByte;
    const std::uint32_t past = sum - loopEndPos - 1u;
    std::int32_t count;
    if (loopEndPos + 1u < sum) {
        const std::uint32_t skip = get_frame_from_sample(i, i.loopStart);
        const std::uint32_t loopBytes = loopEndPos - get_off_from_sample(i, i.loopStart) + 1u;
        count = static_cast<std::int32_t>((loopEndPos - i.curOff + 1u) / i.sampleSize + (past / loopBytes) * (loopBytes / i.sampleSize - skip));
        if (skip * i.sampleSize < past % loopBytes) count += static_cast<std::int32_t>((past % loopBytes) / i.sampleSize - skip);
    } else {
        count = static_cast<std::int32_t>(i.streamDataByte / i.sampleSize);
    }
    return std::max<std::int32_t>(count - i.numFrame, 0);
}
std::int32_t sub_2DF8(const IdInfo &i) {
    std::int32_t ret = sub_2EA4(i);
    if (i.loopNum < 0) return ret;
    const std::uint32_t loopEndPos = get_second_buf_pos(i, i.loopEnd);
    const std::uint32_t sum = i.curOff + i.streamDataByte;
    if (sum >= loopEndPos + 1u &&
        static_cast<std::int32_t>(sum - (loopEndPos + 1u)) / static_cast<std::int32_t>(loopEndPos - get_off_from_sample(i, i.loopStart) + 1u) >= i.loopNum)
        ret = -3;
    return ret;
}
std::int32_t get_remain_frame_of(const IdInfo &i) { // 2ABC
    switch (i.state) {
    case 2: return -1;
    case 3: {
        if (i.curOff >= i.dataOff + i.streamDataByte) return 0;
        const std::int32_t ret = static_cast<std::int32_t>((i.dataOff + i.streamDataByte - i.curOff) / i.sampleSize) - i.numFrame;
        return std::max<std::int32_t>(ret, 0);
    }
    case 4: return sub_2DB8(i);
    case 5: return sub_2DF8(i);
    case 6: return i.loopEnd < i.decodePos ? sub_2DB8(i) : sub_2DF8(i);
    default: return 0;
    }
}
std::uint32_t get_write_ptr(const IdInfo &i) { // 2B88
    std::uint32_t count = 0u;
    switch (i.state) {
    case 4:
        if (i.curOff + i.streamDataByte < i.dataEnd) count = sub_2FA8(i.unk48, i.bufferByte, i.streamDataByte, i.sampleSize);
        break;
    case 5: count = sub_2FA8(i.unk48, i.bufferByte, i.streamDataByte, i.sampleSize); break;
    case 6:
        if (i.loopEnd < i.decodePos && i.unk22 == 1) count = sub_2FA8(0u, i.bufferByte, sub_31B4(i), i.sampleSize);
        else count = sub_2FA8(i.unk48, i.bufferByte, i.streamDataByte, i.sampleSize);
        break;
    default: break;
    }
    return i.buffer + count;
}
std::uint32_t get_read_position(const IdInfo &i) { // 2C68
    switch (i.state) {
    case 4: return sub_2FEC(i.dataEnd, i.curOff, i.streamDataByte, i.bufferByte);
    case 6:
        if (i.loopEnd < i.decodePos) {
            const std::uint32_t inFirst = sub_31B4(i);
            return sub_2FEC(i.dataEnd, i.curOff + i.streamDataByte - inFirst, inFirst, i.bufferByte);
        }
        [[fallthrough]];
    case 5:
        return sub_3004(i.curOff, i.streamDataByte, get_off_from_sample(i, i.loopStart), get_second_buf_pos(i, i.loopEnd), i.bufferByte);
    default: return 0u;
    }
}
std::uint32_t get_writable_byte(const IdInfo &i) { // 2D54
    const std::uint32_t num1 = sub_316C(i.unk22 != 1 ? i.unk48 : 0u, i.bufferByte, sub_31B4(i), i.sampleSize);
    const std::uint32_t num2 = sub_3048(i);
    return num2 >= num1 ? num1 : num2;
}

std::uint32_t read_le(psprecomp::GuestMemory &m, std::uint32_t base, std::uint32_t &off, int size) { // 2310
    std::uint32_t v = 0u;
    for (int b = 0; b < size; ++b) v |= static_cast<std::uint32_t>(m.load8(base + off + static_cast<std::uint32_t>(b))) << (8 * b);
    off += static_cast<std::uint32_t>(size);
    return v;
}

// 1D7C: parses the RIFF WAVE header in the first `size` bytes; returns the codec or an error.
std::int32_t load_wave_file(psprecomp::GuestMemory &m, std::uint32_t size, File &info, std::uint32_t in) {
    std::uint32_t off = 0u;
    info.loopEnd = -1;
    info.samplesPerChan = 0u;
    info.loopStart = -1;
    info.factSz = 0u;
    info.dataSize = 0u;
    std::int32_t fmt = kErrUnknownFormat;
    bool multiChan = false;
    if (size != 0u && !m.contains(in, size)) return kErrSizeTooSmall;
    for (;;) {
        if (size < off + 12u) return kErrSizeTooSmall;
        if (read_le(m, in, off, 4) != 0x46464952u) return kErrUnknownFormat; // "RIFF"
        const std::uint32_t ck = read_le(m, in, off, 4) - 4u;
        const std::uint32_t inc = ck + (ck & 1u);
        if (read_le(m, in, off, 4) == 0x45564157u) break; // "WAVE"
        if (size < off + inc) return kErrSizeTooSmall;
        off += inc;
    }
    if (off + 8u >= size) return kErrSizeTooSmall;
    do {
        const std::uint32_t ckid = read_le(m, in, off, 4);
        std::uint32_t cksize = read_le(m, in, off, 4);
        cksize += cksize & 1u;
        if (size < off + cksize && ckid != 0x61746164u) return kErrSizeTooSmall;
        switch (ckid) {
        case 0x61746164u: // "data"
            info.dataSize = cksize;
            info.dataOff = off;
            if (info.samplesPerChan == 0u) info.samplesPerChan = fmt != kCodecAt3 ? 2048u : 1024u;
            if (multiChan && fmt == kCodecAt3Plus) {
                info.samplesPerChan -= 184u;
                if (info.loopEnd != -1) { info.loopEnd -= 184; info.loopStart -= 184; }
            }
            return fmt;
        case 0x6C706D73u: // "smpl"
            if (info.loopStart < 0) {
                if (cksize < 32u) return kErrUnknownFormat;
                off += 28u;
                cksize -= 32u;
                if (read_le(m, in, off, 4) != 0u) {
                    if (cksize < 20u) return kErrUnknownFormat;
                    off += 12u;
                    info.loopStart = static_cast<std::int32_t>(read_le(m, in, off, 4));
                    info.loopEnd = static_cast<std::int32_t>(read_le(m, in, off, 4));
                    cksize -= 20u;
                    if (info.loopStart >= info.loopEnd) return kErrBadCodecParams;
                }
            }
            break;
        case 0x74636166u: // "fact"
            if (cksize < 4u) return kErrUnknownFormat;
            cksize -= 4u;
            info.factSz = read_le(m, in, off, 4);
            if (cksize >= 8u) {
                (void)read_le(m, in, off, 4);
                multiChan = true;
                cksize -= 8u;
                info.samplesPerChan = read_le(m, in, off, 4);
            } else if (cksize >= 4u) {
                cksize -= 4u;
                info.samplesPerChan = read_le(m, in, off, 4);
            }
            break;
        case 0x20746D66u: { // "fmt "
            if (fmt != kErrUnknownFormat) return kErrUnknownFormat;
            if (cksize < 32u) return kErrUnknownFormat;
            const auto fmtCode = static_cast<std::uint16_t>(read_le(m, in, off, 2));
            const auto channels = static_cast<std::uint16_t>(read_le(m, in, off, 2));
            info.channels = channels;
            if (static_cast<std::uint16_t>(channels - 1u) >= 2u) return kErrUnknownFormat;
            if (read_le(m, in, off, 4) != 44100u) return kErrUnknownFormat;
            off += 4u;
            const auto blk = static_cast<std::uint16_t>(read_le(m, in, off, 2));
            info.dataBlkSz = blk;
            if (blk == 0u) return kErrUnknownFormat;
            if (fmtCode == 0x270u) {
                off += 4u;
                const auto valid = static_cast<std::uint16_t>(read_le(m, in, off, 2));
                if (valid != 1u) return kErrUnknownFormat;
                off += 4u;
                const auto unk = static_cast<std::uint16_t>(read_le(m, in, off, 2));
                info.unk4[0] = static_cast<std::uint8_t>(unk);
                info.unk4[1] = static_cast<std::uint8_t>(unk >> 8);
                if (unk != static_cast<std::uint16_t>(read_le(m, in, off, 2))) return kErrUnknownFormat;
                if (read_le(m, in, off, 4) != valid) return kErrUnknownFormat;
                fmt = kCodecAt3;
                cksize -= 32u;
            } else {
                if (fmtCode != 0xFFFEu) return kErrUnknownFormat;
                if (cksize < 52u) return kErrUnknownFormat;
                for (std::uint32_t b = 0; b < 16u; ++b)
                    if (m.load8(in + off + 10u + b) != kAt3PlusGuid[b]) return kErrUnknownFormat;
                info.unk4[0] = m.load8(in + off + 28u);
                info.unk4[1] = m.load8(in + off + 29u);
                if (((info.unk4[0] >> 2) & 7u) != info.channels) return kErrUnknownFormat;
                fmt = kCodecAt3Plus;
                cksize -= 52u;
                off += 38u;
            }
            break;
        }
        default: break;
        }
        off += cksize;
    } while (size >= off && off + 8u < size);
    return kErrSizeTooSmall;
}

void set_buffer_info(IdInfo &i, std::uint32_t factSize, std::uint32_t dataSize, std::uint32_t samplesPerChan, std::int32_t loopStart,
                     std::int32_t loopEnd) { // 298C
    const std::uint32_t add = skip_samples(i);
    const std::uint32_t samples = samplesPerChan + add;
    factSize = factSize != 0u ? factSize + samples : (dataSize / i.sampleSize) << shift(i);
    i.decodePos = samples;
    i.loopNum = 0;
    i.endSample = factSize - 1u;
    i.numFrame = static_cast<std::int8_t>(samples >> shift(i));
    if (loopStart < 0) {
        i.loopEnd = 0u;
        i.loopStart = 0u;
    } else {
        i.loopEnd = static_cast<std::uint32_t>(loopEnd) + add;
        i.loopStart = static_cast<std::uint32_t>(loopStart) + add;
    }
}

std::int32_t set_buffer_size(IdInfo &i, std::uint32_t readByte, std::uint32_t bufferByte) { // 2A04
    if (bufferByte < i.dataEnd) {
        if (i.streamDataByte < i.sampleSize * 3u) return kErrSizeTooSmall;
        if (i.loopEnd == 0u) { i.state = 4; return 0; }
        if (i.loopEnd == i.endSample) { i.state = 5; return 0; }
        const std::uint32_t untilLoopEnd = get_second_buf_pos(i, i.loopEnd) - i.dataOff + 1u;
        i.state = 6;
        if (untilLoopEnd < i.streamDataByte) i.streamDataByte = untilLoopEnd;
        i.unk52 = 0u;
        i.secondBuffer = 0u;
        i.secondBufferByte = 0u;
    } else if (readByte < i.dataEnd) {
        i.state = 3;
    } else {
        i.state = 2;
    }
    return 0;
}

std::int32_t set_buffer(IdInfo &i, std::uint32_t buffer, std::uint32_t readByte, std::uint32_t bufferByte, const File &f) { // 245C
    i.numChan = static_cast<std::int8_t>(f.channels);
    i.samplesPerChan = static_cast<std::int32_t>(f.samplesPerChan + skip_samples(i));
    i.sampleSize = f.dataBlkSz;
    set_buffer_info(i, f.factSz, f.dataSize, f.samplesPerChan, f.loopStart, f.loopEnd);
    i.streamDataByte = readByte - f.dataOff;
    i.buffer = buffer;
    i.curOff = f.dataOff;
    i.dataOff = f.dataOff;
    i.dataEnd = f.dataSize + f.dataOff;
    i.unk22 = 0;
    i.bufferByte = bufferByte;
    i.unk48 = f.dataOff;
    if (i.endSample >= i.loopEnd && (i.endSample >> shift(i)) * i.sampleSize < f.dataSize) {
        const std::int32_t ret = set_buffer_size(i, readByte, bufferByte);
        if (ret != 0) return ret;
        if (i.codec == kCodecAt3) {
            const std::uint16_t param = static_cast<std::uint16_t>(f.unk4[0] | (f.unk4[1] << 8));
            for (const auto &p : kAt3Params)
                if (p.sampleSize == i.sampleSize && p.jointStereo == param) return 0;
            return kErrBadCodecParams;
        }
        return ret;
    }
    return kErrBadCodecParams;
}

void copy_buffer(psprecomp::GuestMemory &m, const IdInfo &i, std::uint32_t bufferByte) { // 325C
    std::uint32_t num1 = i.bufferByte;
    if (i.dataOff < num1) num1 = ((num1 - i.dataOff) / i.sampleSize) * i.sampleSize + i.dataOff;
    const std::uint32_t count = bufferByte - num1;
    if (count == 0u || count > bufferByte) return;
    std::uint8_t *dst = m.raw_pointer(i.buffer, count);
    const std::uint8_t *src = m.raw_pointer(i.buffer + num1, count);
    if (dst != nullptr && src != nullptr) std::memmove(dst, src, count);
}

// 1C54: after a frame was consumed in a streaming state.
void advance_stream(psprecomp::GuestMemory &m, IdInfo &i) {
    i.streamDataByte -= i.sampleSize;
    if (i.unk22 == 1) {
        const std::uint32_t next = i.unk52 + i.sampleSize;
        if (i.secondBufferByte >= next + i.sampleSize) {
            i.unk52 = next;
        } else {
            i.unk48 = 0u;
            i.unk22 = 2;
            i.unk52 = 0u;
        }
        return;
    }
    std::uint32_t next = i.unk48 + i.sampleSize;
    if (i.bufferByte < next + i.sampleSize) next = 0u;
    i.unk48 = next;
    if (i.loopEnd != 0u) {
        if (i.loopNum != 0) return;
        if (i.loopEnd >= i.decodePos) return;
    }
    if (i.unk22 != 0 || i.state != 6) return;
    if (get_second_buf_pos(i, i.loopEnd) >= i.curOff) return;
    i.unk22 = 1;
    i.streamDataByte = i.secondBufferByte;
    i.unk52 = 0u;
    const std::uint32_t tail = i.secondBufferByte % i.sampleSize;
    if (tail != 0u) {
        std::uint8_t *dst = m.raw_pointer(i.buffer, tail);
        const std::uint8_t *src = m.raw_pointer(i.secondBuffer + i.secondBufferByte - tail, tail);
        if (dst != nullptr && src != nullptr) std::memmove(dst, src, tail);
    }
}

void get_buffer_info(const IdInfo &i, std::uint32_t sample, BufferInfo &b) { // 32B4
    bool filled = false;
    switch (i.state) {
    case 3: {
        const std::uint32_t num1 = i.dataOff + i.streamDataByte;
        const std::uint32_t num2 = i.dataOff + ((sample >> shift(i)) + 1u) * i.sampleSize;
        b.writePositionFirstBuf = i.buffer + num1;
        b.writableByteFirstBuf = i.dataEnd - num1;
        b.readPositionFirstBuf = num1;
        b.minWriteByteFirstBuf = num2 >= num1 ? num2 - num1 : 0u;
        filled = true;
        break;
    }
    case 4: case 5: {
        const std::uint32_t num1 = round_from(0u, i.sampleSize, i.bufferByte);
        b.writePositionFirstBuf = i.buffer;
        const std::uint32_t off = get_off_from_sample(i, sample);
        b.readPositionFirstBuf = off;
        b.writableByteFirstBuf = std::min(i.dataEnd - off, num1);
        b.minWriteByteFirstBuf = (get_frame_from_sample(i, sample) + 1u) * i.sampleSize;
        filled = true;
        break;
    }
    case 6: {
        const std::uint32_t off = get_off_from_sample(i, sample);
        const std::uint32_t loopEndPos = get_second_buf_pos(i, i.loopEnd);
        std::uint32_t whole = round_from(0u, i.sampleSize, i.bufferByte);
        const std::uint32_t mult = (get_frame_from_sample(i, sample) + 1u) * i.sampleSize;
        if (off < loopEndPos) {
            std::uint32_t diff = loopEndPos - off + 1u;
            if (diff < whole) whole = diff;
            if (diff >= mult) diff = mult;
            b.writePositionFirstBuf = i.buffer;
            b.writableByteFirstBuf = whole;
            b.minWriteByteFirstBuf = diff;
            b.readPositionFirstBuf = off;
            filled = true;
            break;
        }
        std::uint32_t num = loopEndPos + round_from(0u, i.sampleSize, i.secondBufferByte);
        if (off >= num) {
            b.readPositionFirstBuf = off;
            b.minWriteByteFirstBuf = mult;
            b.writePositionFirstBuf = i.buffer;
            b.writableByteFirstBuf = std::min(i.dataEnd - off, whole);
            filled = true;
        } else if (loopEndPos + i.secondBufferByte + 1u < i.dataEnd) {
            ++num;
            b.readPositionFirstBuf = num;
            const std::uint32_t end = off + mult;
            b.writePositionFirstBuf = i.buffer;
            b.writableByteFirstBuf = std::min(i.dataEnd - num, whole);
            b.minWriteByteFirstBuf = end >= num ? end - num : 0u;
            filled = true;
        }
        break;
    }
    default: break;
    }
    if (!filled) {
        b.writableByteFirstBuf = 0u;
        b.writePositionFirstBuf = i.buffer;
        b.minWriteByteFirstBuf = 0u;
        b.readPositionFirstBuf = 0u;
    }
    b.readPositionSecondBuf = 0u;
    b.writePositionSecondBuf = i.buffer;
    b.writableByteSecondBuf = 0u;
    b.minWriteByteSecondBuf = 0u;
}

std::int32_t reset_play_pos(IdInfo &i, std::uint32_t sample, std::uint32_t writeFirst, std::uint32_t writeSecond) { // 3540
    BufferInfo b;
    get_buffer_info(i, sample, b);
    if (writeFirst < b.minWriteByteFirstBuf || b.writableByteFirstBuf < writeFirst) return kErrBadFirstResetSize;
    if (writeSecond < b.minWriteByteSecondBuf || b.writableByteSecondBuf < writeSecond) return kErrBadSecondResetSize;
    i.decodePos = sample;
    i.numFrame = static_cast<std::int8_t>(get_frame_from_sample(i, sample));
    i.loopNum = 0;
    i.curOff = get_off_from_sample(i, sample);
    switch (i.state) {
    case 6: {
        const std::uint32_t off = get_off_from_sample(i, sample);
        const std::uint32_t loopEndPos = get_second_buf_pos(i, i.loopEnd);
        if (off < loopEndPos) {
            i.streamDataByte = writeFirst;
            i.unk22 = 0;
            i.unk48 = 0u;
        } else {
            const std::uint32_t num = loopEndPos + round_from(0u, i.sampleSize, i.secondBufferByte);
            if (off >= num) {
                i.streamDataByte = writeFirst;
                i.unk22 = 2;
                i.unk48 = 0u;
            } else {
                i.streamDataByte = num - off + writeFirst + 1u;
                i.unk22 = 1;
                i.unk52 = off - loopEndPos - 1u;
            }
        }
        break;
    }
    case 4: case 5:
        i.streamDataByte = writeFirst;
        i.unk22 = 0;
        i.unk48 = 0u;
        break;
    case 3:
        i.unk22 = 0;
        i.streamDataByte += writeFirst;
        if (i.dataOff + i.streamDataByte >= i.dataEnd) i.state = 2;
        break;
    default: break;
    }
    return 0;
}

} // namespace

std::int32_t Library::reinit(int numAt3, int numAt3Plus) { // sceAtracReinit, without the EDRAM accounting
    for (const auto &id : ids_)
        if (id.state > 0) return static_cast<std::int32_t>(0x80000021u); // SCE_ERROR_BUSY
    for (auto &id : ids_) id = IdInfo{};
    for (int k = 0; k < numAt3 + numAt3Plus && k < kMaxIds; ++k) {
        ids_[k].codec = k >= numAt3Plus ? kCodecAt3 : kCodecAt3Plus;
        ids_[k].state = -1;
    }
    return 0;
}

std::int32_t Library::get_atrac_id(std::uint32_t codecType) {
    if (codecType < kCodecAt3Plus || codecType > kCodecAt3) return kErrInvalidCodec;
    for (int k = 0; k < kMaxIds; ++k) {
        if (ids_[k].codec == codecType && ids_[k].state == -1) {
            ids_[k].state = 1;
            return k;
        }
    }
    return kErrNoIdLeft;
}

std::int32_t Library::release_atrac_id(int id) {
    if (id < 0 || id >= kMaxIds) return kErrNoId;
    if (ids_[id].state > 0 && ids_[id].state != 16) {
        ids_[id].state = -1;
        tracks_[id].reset();
        track_names_[id].clear();
        return 0;
    }
    return kErrNoId;
}

std::int32_t Library::set_halfway(psprecomp::GuestMemory &m, IdInfo &id, std::uint32_t buffer, std::uint32_t readByte,
                                  std::uint32_t bufferByte) {
    File f;
    // The caller already parsed the header; parse again for the file info (uOFW passes it along).
    (void)load_wave_file(m, readByte, f, buffer);
    std::int32_t ret = set_buffer(id, buffer, readByte, bufferByte, f);
    if (ret < 0) return ret;
    attach_track(m, id, buffer, readByte);
    ret = reset_id(m, id); // 1284: sceAudiocodecInit always succeeds here
    if ((id.state & 4) != 0) copy_buffer(m, id, bufferByte);
    return ret;
}

// Persona3DS: the converted track for the .at3 data starting at `buffer`, if any.
void Library::attach_track(psprecomp::GuestMemory &m, IdInfo &id, std::uint32_t buffer, std::uint32_t readByte) {
    const auto n = static_cast<int>(&id - ids_);
    track_names_[n].clear();
    track_offsets_[n] = 0;
    tracks_[n] = media::fork_media().find_atrac(m, buffer, readByte, track_offsets_[n], &track_names_[n]);
}

std::int32_t Library::set_halfway_buffer(psprecomp::GuestMemory &m, int id, std::uint32_t buffer, std::uint32_t readByte,
                                         std::uint32_t bufferByte) {
    if (!usable(id)) return kErrNoId;
    if (bufferByte < readByte) return kErrIncorrectReadSize;
    File f;
    const std::int32_t codec = load_wave_file(m, readByte, f, buffer);
    if (codec < 0) return codec;
    if (ids_[id].codec != codec) return kErrWrongCodec;
    return set_halfway(m, ids_[id], buffer, readByte, bufferByte);
}

std::int32_t Library::set_halfway_buffer_and_get_id(psprecomp::GuestMemory &m, std::uint32_t buffer, std::uint32_t readByte,
                                                    std::uint32_t bufferByte) {
    if (bufferByte < readByte) return kErrIncorrectReadSize;
    File f;
    const std::int32_t codec = load_wave_file(m, readByte, f, buffer);
    if (codec < 0) return codec;
    const std::int32_t id = get_atrac_id(static_cast<std::uint32_t>(codec));
    if (id < 0) return id;
    const std::int32_t ret = set_halfway(m, ids_[id], buffer, readByte, bufferByte);
    if (ret < 0) {
        if (ids_[id].state > 0 && ids_[id].state != 16) ids_[id].state = -1;
        return ret;
    }
    return id;
}

std::int32_t Library::reset_id(psprecomp::GuestMemory &m, IdInfo &id) { // 13D8
    std::uint32_t finish = 0u;
    while (id.numFrame != 0) {
        const std::int32_t ret = decode_frame(m, id, 0u, nullptr, &finish);
        if (ret != 0) return ret;
    }
    return 0;
}

std::int32_t Library::decode_frame(psprecomp::GuestMemory &m, IdInfo &i, std::uint32_t out, std::uint32_t *samples,
                                   std::uint32_t *finish) { // 19A4
    const std::uint32_t next = next_sample_of(i);
    if (i.dataEnd >= i.sampleSize + i.curOff && i.endSample >= i.decodePos) {
        if (((i.state & 4) != 0 && i.streamDataByte < i.sampleSize) ||
            (i.state == 3 && i.dataOff + i.streamDataByte < i.sampleSize + i.curOff)) {
            *finish = 0u;
            return kErrBufferIsEmpty;
        }
        // sceAudiocodecDecode: no decoder here, the frame is silence. A frame
        // before the audio data is not a valid frame: [INFERRED] from
        // resetpos.expected (reset to sample -2048 puts curOff at 0xFFFFFEE8:
        // 0x80630002, codec_err 0x20x, the frame stays unconsumed).
        if (static_cast<std::int32_t>(i.curOff) < static_cast<std::int32_t>(i.dataOff)) {
            i.codecErr = 0x200;
            *finish = 0u;
            return static_cast<std::int32_t>(0x80630002u);
        }
        i.codecErr = 0;
        i.curOff += i.sampleSize;
        if (i.numFrame == 0) {
            *samples = next;
            *finish = i.endSample < i.decodePos + next ? (i.loopNum == 0 ? 1u : 0u) : 0u;
            if (next != 0u && out != 0u && m.contains(out, next * 4u)) { // stereo S16 output (SetData, not MOut)
                const auto n = static_cast<std::size_t>(&i - ids_);
                std::uint8_t *dst = tracks_[n] != nullptr ? m.raw_pointer(out, next * 4u) : nullptr;
                if (dst != nullptr && (reinterpret_cast<std::uintptr_t>(dst) & 1u) == 0u)
                    tracks_[n]->read_stereo(static_cast<std::int64_t>(i.decodePos) + track_offsets_[n], next,
                                            reinterpret_cast<std::int16_t *>(dst));
                else
                    m.zero(out, next * 4u);
            }
            i.decodePos += next;
            if (i.loopEnd != 0u && (i.loopNum != 0 || i.loopEnd >= i.decodePos) && i.loopEnd < i.decodePos) {
                i.curOff = get_off_from_sample(i, i.loopStart);
                i.numFrame = static_cast<std::int8_t>(get_frame_from_sample(i, i.loopStart));
                i.decodePos = i.loopStart;
                if (i.loopNum > 0) --i.loopNum;
            }
        } else {
            --i.numFrame;
        }
        if ((i.state & 4) != 0) advance_stream(m, i);
        return 0;
    }
    *finish = 1u;
    return kErrAllDataDecoded;
}

std::int32_t Library::decode_data(psprecomp::GuestMemory &m, int id, std::uint32_t out, std::uint32_t *samples,
                                  std::uint32_t *finish, std::int32_t *remain) {
    if (!usable(id)) return kErrNoId;
    auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if ((out & 1u) != 0u) return kErrBadAlign;
    if (i.state == 6 && i.secondBufferByte == 0u) return kErrSecondBufferNeeded;
    // [INFERRED] the frame count is read once: the frames left to skip, then
    // one output frame (uOFW's decompiled loop re-reads numFrame).
    const int frames = i.numFrame;
    for (int k = 0; k <= frames; ++k) {
        const std::int32_t ret = decode_frame(m, i, out, samples, finish);
        if (ret != 0) {
            *samples = 0u;
            return ret;
        }
    }
    *remain = get_remain_frame_of(i);
    return 0;
}

std::int32_t Library::get_remain_frame(int id, std::int32_t *remain) const {
    if (!usable(id)) return kErrNoId;
    if (const auto ret = is_valid_state(ids_[id].state); ret < 0) return ret;
    *remain = get_remain_frame_of(ids_[id]);
    return 0;
}

std::int32_t Library::get_stream_data_info(int id, std::uint32_t *writePtr, std::uint32_t *writable, std::uint32_t *readPosition) const {
    if (!usable(id)) return kErrNoId;
    const auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if (i.state == 2) {
        *writable = 0u;
        *writePtr = i.buffer;
        *readPosition = 0u;
    } else if (i.state == 3) {
        const std::uint32_t pos = i.dataOff + i.streamDataByte;
        *writable = i.bufferByte - pos;
        *writePtr = i.buffer + pos;
        *readPosition = pos;
    } else {
        *readPosition = get_read_position(i);
        *writePtr = get_write_ptr(i);
        *writable = get_writable_byte(i);
    }
    return 0;
}

std::int32_t Library::add_stream_data(int id, std::uint32_t addByte) {
    if (!usable(id)) return kErrNoId;
    auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if (i.state == 2) return kErrAllDataLoaded;
    std::uint32_t remaining;
    if (i.state == 3) {
        remaining = i.dataEnd - i.dataOff - i.streamDataByte;
        if (remaining == addByte) i.state = 2;
    } else {
        remaining = get_writable_byte(i);
    }
    if (remaining < addByte) return kErrAddDataIsTooBig;
    i.streamDataByte += addByte;
    return 0;
}

std::int32_t Library::get_second_buffer_info(int id, std::uint32_t *position, std::uint32_t *dataByte) const {
    if (!usable(id)) return kErrNoId;
    const auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if (i.state != 6) {
        *position = 0u;
        *dataByte = 0u;
        return kErrSecondBufferNotNeeded;
    }
    const std::uint32_t pos = get_second_buf_pos(i, i.loopEnd) + 1u;
    *position = pos;
    // [VERIFIED] second/getinfo.expected: the bytes from the loop end to the
    // end of the data (0x24B8 = 0xB54C - 0x9094); uOFW's decompilation reads
    // bufferByte here [WRONG].
    *dataByte = i.dataEnd - pos;
    return 0;
}

std::int32_t Library::set_second_buffer(int id, std::uint32_t secondBuffer, std::uint32_t secondBufferByte) {
    if (!usable(id)) return kErrNoId;
    auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    const std::uint32_t pos = get_second_buf_pos(i, i.loopEnd);
    if (secondBufferByte < i.sampleSize * 3u && secondBufferByte < i.dataEnd - pos - 1u) return kErrSizeTooSmall;
    if (i.state != 6) return kErrSecondBufferNotNeeded;
    i.secondBuffer = secondBuffer;
    i.secondBufferByte = secondBufferByte;
    i.unk52 = 0u;
    return 0;
}

std::int32_t Library::get_next_sample(int id, std::uint32_t *next) const {
    if (!usable(id)) return kErrNoId;
    if (const auto ret = is_valid_state(ids_[id].state); ret < 0) return ret;
    *next = next_sample_of(ids_[id]);
    return 0;
}

std::int32_t Library::get_sound_sample(int id, std::int32_t *end, std::int32_t *loopStart, std::int32_t *loopEnd) const {
    if (!usable(id)) return kErrNoId;
    const auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    *end = static_cast<std::int32_t>(i.endSample) - i.samplesPerChan;
    if (i.loopEnd != 0u) {
        *loopStart = static_cast<std::int32_t>(i.loopStart) - i.samplesPerChan;
        *loopEnd = static_cast<std::int32_t>(i.loopEnd) - i.samplesPerChan;
    } else {
        *loopStart = -1;
        *loopEnd = -1;
    }
    return 0;
}

std::int32_t Library::get_next_decode_position(int id, std::uint32_t *position) const {
    if (!usable(id)) return kErrNoId;
    const auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    // [VERIFIED] stream.expected: positions keep being reported while the
    // streamed file offset is past the buffer size; the bound is the end of
    // the data. uOFW's decompilation reads bufferByte here [WRONG], the same
    // field confusion as in sceAtracGetSecondBufferInfo.
    if (i.dataEnd - i.curOff < i.sampleSize || i.endSample < i.decodePos) return kErrAllDataDecoded;
    *position = i.decodePos - static_cast<std::uint32_t>(i.samplesPerChan);
    return 0;
}

std::int32_t Library::get_loop_status(int id, std::int32_t *loopNum, std::uint32_t *loopStatus) const {
    if (!usable(id)) return kErrNoId;
    const auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    *loopNum = i.loopNum;
    if (i.loopEnd == 0u) *loopStatus = 0u;
    else if (i.loopNum != 0) *loopStatus = 1u;
    else *loopStatus = i.loopEnd < i.decodePos ? 0u : 1u;
    return 0;
}

std::int32_t Library::set_loop_num(int id, std::int32_t loopNum) {
    if (!usable(id)) return kErrNoId;
    auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if (i.loopEnd == 0u || (i.loopNum == 0 && i.loopEnd < i.decodePos)) return kErrNoLoopInformation;
    i.loopNum = loopNum;
    return 0;
}

std::int32_t Library::get_buffer_info_for_resetting(psprecomp::GuestMemory &m, int id, std::uint32_t sample, BufferInfo *info) {
    if (!usable(id)) return kErrNoId;
    auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if (i.state == 6 && i.secondBufferByte == 0u) return kErrSecondBufferNeeded;
    if (i.endSample < sample + static_cast<std::uint32_t>(i.samplesPerChan)) return kErrBadSample;
    get_buffer_info(i, sample + static_cast<std::uint32_t>(i.samplesPerChan), *info);
    return reset_id(m, i);
}

std::int32_t Library::reset_play_position(psprecomp::GuestMemory &m, int id, std::uint32_t sample, std::uint32_t writeFirst,
                                          std::uint32_t writeSecond) {
    if (!usable(id)) return kErrNoId;
    auto &i = ids_[id];
    if (const auto ret = is_valid_state(i.state); ret < 0) return ret;
    if (i.state == 6 && i.secondBufferByte == 0u) return kErrSecondBufferNeeded;
    if (i.endSample < sample + static_cast<std::uint32_t>(i.samplesPerChan)) return kErrBadSample;
    if (const auto ret = reset_play_pos(i, sample + static_cast<std::uint32_t>(i.samplesPerChan), writeFirst, writeSecond); ret < 0) return ret;
    return reset_id(m, i);
}

std::int32_t Library::is_second_buffer_needed(int id) const {
    if (!usable(id)) return kErrNoId;
    if (const auto ret = is_valid_state(ids_[id].state); ret < 0) return ret;
    return ids_[id].state == 6 ? 1 : 0;
}

std::int32_t Library::get_channel(int id, std::uint32_t *channel) const {
    if (!usable(id)) return kErrNoId;
    if (ids_[id].state == 1) return kErrNoData;
    *channel = static_cast<std::uint32_t>(ids_[id].numChan);
    return 0;
}

std::int32_t Library::get_output_channel(int id, std::uint32_t *channel) const {
    if (!usable(id)) return kErrNoId;
    if (ids_[id].state == 1) return kErrNoData;
    *channel = 2u; // [INFERRED] stereo output for sceAtracSetData* (MOut variants are not modelled)
    return 0;
}

std::int32_t Library::get_max_sample(int id, std::uint32_t *max) const {
    if (!usable(id)) return kErrNoId;
    if (ids_[id].state == 1) return kErrNoData;
    *max = 0x800u >> (ids_[id].codec & 1u);
    return 0;
}

std::int32_t Library::get_bitrate(int id, std::uint32_t *bitrate) const {
    if (!usable(id)) return kErrNoId;
    if (ids_[id].state == 1) return kErrNoData;
    const auto &i = ids_[id];
    std::uint32_t rate = (i.sampleSize * 352800u) / 1000u;
    rate = i.codec == kCodecAt3Plus ? ((rate >> 11) + 8u) & 0xFFFFFFF0u : (rate + 511u) >> 10;
    *bitrate = rate;
    return 0;
}

std::int32_t Library::get_internal_error_info(int id, std::int32_t *result) const {
    if (!usable(id)) return kErrNoId;
    if (ids_[id].state == 1) return kErrNoData;
    *result = ids_[id].codecErr;
    return 0;
}

namespace {
Library g_library;
}

Library &fork_atrac_library() noexcept { return g_library; }

void register_fork_atrac(psprecomp::Runtime &runtime, KernelState &) {
    g_library = Library{};
    const auto reg = [&runtime](std::uint32_t nid, psprecomp::Runtime::HleFunction fn) {
        runtime.register_hle("sceAtrac3plus", nid, std::move(fn));
    };
    using Ctx = psprecomp::AllegrexContext;
    using Rt = psprecomp::Runtime;
    const auto sid = [](const Ctx &ctx) { return static_cast<int>(ctx.gpr[4]); };
    const auto ret = [](Ctx &ctx, std::int32_t v) { ctx.set_gpr(2, static_cast<std::uint32_t>(v)); };
    // Persona3DS: event when a SetData call attached a converted track (fork_media.hpp);
    // `id` < 0 takes the ID from the result (the ...AndGetID calls).
    const auto set_ret = [](Rt &rt, Ctx &ctx, int id, std::int32_t v) {
        ctx.set_gpr(2, static_cast<std::uint32_t>(v));
        if (v < 0) return;
        const bool get_id = id < 0;
        if (get_id) id = v;
        const auto &name = g_library.track_name(id);
        const auto &stats = media::fork_media().stats();
        if (!name.empty()) rt.event("atrac_track", {{"id", static_cast<std::uint32_t>(id)}}, name);
        else if (stats.entries != 0u) rt.event("atrac_track_unmatched", {{"id", static_cast<std::uint32_t>(id)}, {"buffer", ctx.gpr[get_id ? 4 : 5]}});
    };
    // Out-parameter writes: a PSP crashes on an invalid pointer (pspautotests "Crashes." cases).
    const auto out32 = [](Rt &rt, std::uint32_t at, std::uint32_t v, const char *what) {
        if (!rt.memory().contains(at, 4u)) { rt.stop(std::string("sceAtrac: invalid ") + what + " pointer " + psprecomp::hex32(at)); return false; }
        rt.memory().store32(at, v);
        return true;
    };

    reg(0x780F88D1u, [ret](Rt &, Ctx &ctx) { ret(ctx, g_library.get_atrac_id(ctx.gpr[4])); });                 // sceAtracGetAtracID
    reg(0x61EB33F5u, [ret, sid](Rt &, Ctx &ctx) { ret(ctx, g_library.release_atrac_id(sid(ctx))); });           // sceAtracReleaseAtracID
    reg(0x0E2A73ABu, [set_ret, sid](Rt &rt, Ctx &ctx) {                                                             // sceAtracSetData
        set_ret(rt, ctx, sid(ctx), g_library.set_halfway_buffer(rt.memory(), sid(ctx), ctx.gpr[5], ctx.gpr[6], ctx.gpr[6]));
    });
    reg(0x7A20E7AFu, [set_ret](Rt &rt, Ctx &ctx) {                                                                  // sceAtracSetDataAndGetID
        set_ret(rt, ctx, -1, g_library.set_halfway_buffer_and_get_id(rt.memory(), ctx.gpr[4], ctx.gpr[5], ctx.gpr[5]));
    });
    reg(0x3F6E26B5u, [set_ret, sid](Rt &rt, Ctx &ctx) {                                                             // sceAtracSetHalfwayBuffer
        set_ret(rt, ctx, sid(ctx), g_library.set_halfway_buffer(rt.memory(), sid(ctx), ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]));
    });
    reg(0x0FAE370Eu, [set_ret](Rt &rt, Ctx &ctx) {                                                                  // sceAtracSetHalfwayBufferAndGetID
        set_ret(rt, ctx, -1, g_library.set_halfway_buffer_and_get_id(rt.memory(), ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]));
    });
    reg(0x6A8C3CD5u, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracDecodeData
        std::uint32_t samples = 0u, finish = 0u;
        std::int32_t remain = 0;
        const auto r = g_library.decode_data(rt.memory(), sid(ctx), ctx.gpr[5], &samples, &finish, &remain);
        ret(ctx, r);
        if (r == static_cast<std::int32_t>(0x80630005u) || r == static_cast<std::int32_t>(0x80630010u) ||
            r == static_cast<std::int32_t>(0x80630031u) || r == static_cast<std::int32_t>(0x80630040u) ||
            r == static_cast<std::int32_t>(0x80630014u) || r == static_cast<std::int32_t>(0x80630012u))
            return; // rejected before decoding: nothing written (decode.expected)
        if (!out32(rt, ctx.gpr[6], samples, "sample count")) return;
        if (r != 0) { (void)out32(rt, ctx.gpr[7], finish, "finish flag"); return; }
        if (!out32(rt, ctx.gpr[7], finish, "finish flag")) return;
        (void)out32(rt, ctx.gpr[8], static_cast<std::uint32_t>(remain), "remain frame");
    });
    reg(0x9AE849A7u, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetRemainFrame
        std::int32_t remain = 0;
        const auto r = g_library.get_remain_frame(sid(ctx), &remain);
        ret(ctx, r);
        if (r == 0) (void)out32(rt, ctx.gpr[5], static_cast<std::uint32_t>(remain), "remain frame");
    });
    reg(0x5D268707u, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetStreamDataInfo
        std::uint32_t ptr = 0u, writable = 0u, read = 0u;
        const auto r = g_library.get_stream_data_info(sid(ctx), &ptr, &writable, &read);
        ret(ctx, r);
        if (r == 0 && out32(rt, ctx.gpr[5], ptr, "write pointer") && out32(rt, ctx.gpr[6], writable, "writable bytes"))
            (void)out32(rt, ctx.gpr[7], read, "read position");
    });
    reg(0x7DB31251u, [ret, sid](Rt &, Ctx &ctx) { ret(ctx, g_library.add_stream_data(sid(ctx), ctx.gpr[5])); }); // sceAtracAddStreamData
    reg(0x83E85EA0u, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetSecondBufferInfo
        std::uint32_t pos = 0u, bytes = 0u;
        const auto r = g_library.get_second_buffer_info(sid(ctx), &pos, &bytes);
        ret(ctx, r);
        if ((r == 0 || r == static_cast<std::int32_t>(0x80630022u)) && out32(rt, ctx.gpr[5], pos, "position"))
            (void)out32(rt, ctx.gpr[6], bytes, "data byte");
    });
    reg(0x83BF7AFDu, [ret, sid](Rt &, Ctx &ctx) { ret(ctx, g_library.set_second_buffer(sid(ctx), ctx.gpr[5], ctx.gpr[6])); }); // sceAtracSetSecondBuffer
    reg(0x36FAABFBu, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetNextSample
        std::uint32_t next = 0u;
        const auto r = g_library.get_next_sample(sid(ctx), &next);
        ret(ctx, r);
        if (r == 0) (void)out32(rt, ctx.gpr[5], next, "next sample");
    });
    reg(0xA2BBA8BEu, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetSoundSample
        std::int32_t end = 0, ls = 0, le = 0;
        const auto r = g_library.get_sound_sample(sid(ctx), &end, &ls, &le);
        ret(ctx, r);
        if (r == 0 && out32(rt, ctx.gpr[5], static_cast<std::uint32_t>(end), "end sample") &&
            out32(rt, ctx.gpr[6], static_cast<std::uint32_t>(ls), "loop start"))
            (void)out32(rt, ctx.gpr[7], static_cast<std::uint32_t>(le), "loop end");
    });
    reg(0xE23E3A35u, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetNextDecodePosition
        std::uint32_t pos = 0u;
        const auto r = g_library.get_next_decode_position(sid(ctx), &pos);
        ret(ctx, r);
        if (r == 0) (void)out32(rt, ctx.gpr[5], pos, "sample position");
    });
    reg(0xFAA4F89Bu, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetLoopStatus
        std::int32_t num = 0;
        std::uint32_t status = 0u;
        const auto r = g_library.get_loop_status(sid(ctx), &num, &status);
        ret(ctx, r);
        if (r == 0 && out32(rt, ctx.gpr[5], static_cast<std::uint32_t>(num), "loop number")) (void)out32(rt, ctx.gpr[6], status, "loop status");
    });
    reg(0x868120B5u, [ret, sid](Rt &, Ctx &ctx) { ret(ctx, g_library.set_loop_num(sid(ctx), static_cast<std::int32_t>(ctx.gpr[5]))); }); // sceAtracSetLoopNum
    const auto buffer_info = [ret, sid](Rt &rt, Ctx &ctx) {                                                     // sceAtracGetBufferInfoForResetting
        BufferInfo info;
        const auto r = g_library.get_buffer_info_for_resetting(rt.memory(), sid(ctx), ctx.gpr[5], &info);
        ret(ctx, r);
        if (r < 0 && r != static_cast<std::int32_t>(0x80630023u) && r != static_cast<std::int32_t>(0x80630024u)) return;
        const std::uint32_t at = ctx.gpr[6];
        if (!rt.memory().contains(at, 32u)) { rt.stop("sceAtracGetBufferInfoForResetting: invalid info pointer " + psprecomp::hex32(at)); return; }
        const std::uint32_t words[8] = {info.writePositionFirstBuf, info.writableByteFirstBuf, info.minWriteByteFirstBuf, info.readPositionFirstBuf,
                                        info.writePositionSecondBuf, info.writableByteSecondBuf, info.minWriteByteSecondBuf, info.readPositionSecondBuf};
        for (std::uint32_t k = 0; k < 8u; ++k) rt.memory().store32(at + 4u * k, words[k]);
    };
    reg(0x2DD3E298u, buffer_info);
    reg(0xCA3CA3D2u, buffer_info); // sceAtracGetBufferInfoForReseting (alias)
    reg(0x644E5607u, [ret, sid](Rt &rt, Ctx &ctx) {                                                             // sceAtracResetPlayPosition
        ret(ctx, g_library.reset_play_position(rt.memory(), sid(ctx), ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]));
    });
    reg(0xECA32A99u, [ret, sid](Rt &, Ctx &ctx) { ret(ctx, g_library.is_second_buffer_needed(sid(ctx))); });   // sceAtracIsSecondBufferNeeded
    const auto getter = [ret, sid, out32](auto fn, const char *what) {
        return [ret, sid, out32, fn, what](Rt &rt, Ctx &ctx) {
            std::uint32_t value = 0u;
            const auto r = (g_library.*fn)(sid(ctx), &value);
            ret(ctx, r);
            if (r == 0) (void)out32(rt, ctx.gpr[5], value, what);
        };
    };
    reg(0x31668BAAu, getter(&Library::get_channel, "channel"));               // sceAtracGetChannel
    reg(0xB3B5D042u, getter(&Library::get_output_channel, "output channel")); // sceAtracGetOutputChannel
    reg(0xD6A5F2F7u, getter(&Library::get_max_sample, "max sample"));         // sceAtracGetMaxSample
    reg(0xA554A158u, getter(&Library::get_bitrate, "bitrate"));               // sceAtracGetBitrate
    reg(0xE88F759Bu, [ret, sid, out32](Rt &rt, Ctx &ctx) {                                                      // sceAtracGetInternalErrorInfo
        std::int32_t err = 0;
        const auto r = g_library.get_internal_error_info(sid(ctx), &err);
        ret(ctx, r);
        if (r == 0) (void)out32(rt, ctx.gpr[5], static_cast<std::uint32_t>(err), "error");
    });
}

} // namespace p3p3ds::hle::atrac
