// license:BSD-3-Clause
// Based on the SCSP DSP interpreter by ElSemi and R. Belmont.
#include "scspdsp.h"

#if __has_include("vf2-scsp-native-steps.inc")
#define VF2_SCSP_NATIVE_PROGRAM 1
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
enum Flag : u16 {
    write_temp = 1 << 0, input_x = 1 << 1, write_mem = 1 << 2,
    table = 1 << 3, ram_write = 1 << 4, ram_read = 1 << 5,
    effect = 1 << 6, latch_address = 1 << 7, latch_fraction = 1 << 8,
    latch_y = 1 << 9, negate_b = 1 << 10, zero_b = 1 << 11,
    acc_b = 1 << 12, no_float = 1 << 13, add_address = 1 << 14,
    next_address = 1 << 15
};

struct DecodedStep {
    u16 flags;
    u8 temp_read, temp_write, y_source, input, mem_write, effect_write;
    u8 shift, coefficient, ram_address;
    bool needs_input, needs_shift;
};

constexpr DecodedStep decode(const u16 *words, bool odd) {
    const auto [a, b, c, d] = std::array<u16, 4>{words[0], words[1], words[2], words[3]};
    const u16 memory = odd ? c : c & ~0x6000;
    const u16 flags =
        (a & 0x0080 ? write_temp : 0) |
        (b & 0x8000 ? input_x : 0) |
        (b & 0x0020 ? write_mem : 0) |
        (c & 0x8000 ? table : 0) |
        (memory & 0x4000 ? ram_write : 0) |
        (memory & 0x2000 ? ram_read : 0) |
        (c & 0x1000 ? effect : 0) |
        (c & 0x0080 ? latch_address : 0) |
        (c & 0x0040 ? latch_fraction : 0) |
        (c & 0x0008 ? latch_y : 0) |
        (c & 0x0004 ? negate_b : 0) |
        (c & 0x0002 ? zero_b : 0) |
        (c & 0x0001 ? acc_b : 0) |
        (d & 0x8000 ? no_float : 0) |
        (d & 0x0002 ? add_address : 0) |
        (d & 0x0001 ? next_address : 0);
    const u8 shift = (c >> 4) & 3;
    return {flags, u8((a >> 8) & 127), u8(a & 127), u8((b >> 13) & 3),
            u8((b >> 6) & 63), u8(b & 31), u8((c >> 8) & 15), shift,
            u8((d >> 9) & 63), u8((d >> 2) & 31),
            bool((flags & (input_x | latch_y)) || ((flags & latch_address) && shift != 3)),
            bool((flags & (write_temp | ram_write | effect | latch_fraction)) ||
                 ((flags & latch_address) && shift == 3))};
}

u16 pack_sample(s32 val) {
    const unsigned sign = BIT(val, 23);
    u32 temp = (u32(val) ^ (u32(val) << 1)) & 0xffffff;
    unsigned exponent = 0;
    while (exponent < 12 && !(temp & 0x800000)) {
        temp <<= 1;
        ++exponent;
    }
    const u32 shifted = exponent < 12 ? (u32(val) << exponent) & 0x3fffff : u32(val) << 11;
    return u16(((shifted >> 11) & 0x7ff) | (sign << 15) | (exponent << 11));
}

s32 unpack_sample(u16 val) {
    const unsigned sign = BIT(val, 15);
    const unsigned exponent = (val >> 11) & 15;
    const unsigned sign_bits = (sign << 23) | ((exponent > 11 ? sign : sign ^ 1) << 22);
    return util::sext(s32(sign_bits | ((val & 0x7ff) << 11)), 24) >> std::min(exponent, 11u);
}

struct SampleState {
    s32 accumulator = 0, memory_value = 0, fraction = 0, y_register = 0;
    u32 address_register = 0;
};

[[gnu::always_inline]] inline void execute_op(SCSPDSP &dsp, SampleState &state, u32 dec,
                                              const DecodedStep &op) {
    auto &[accumulator, memory_value, fraction, y_register, address_register] = state;
    const auto flags = op.flags;
    s32 input = 0;
    if (op.needs_input) {
        if (op.input < 0x20)
            input = dsp.MEMS[op.input];
        else if (op.input < 0x30)
            input = dsp.MIXS[op.input - 0x20] << 4;
        else
            input = dsp.EXTS[op.input - 0x30] << 8;
        input = util::sext(input, 24);
    }
    if (flags & write_mem) {
        dsp.MEMS[op.mem_write] = memory_value;
        if (op.input == op.mem_write)
            input = memory_value;
    }

    s32 temp = 0;
    if (!(flags & input_x) || !(flags & (zero_b | acc_b)))
        temp = util::sext(dsp.TEMP[(op.temp_read + dec) & 127], 24);
    const s32 b = flags & zero_b ? 0 : flags & acc_b ? accumulator : temp;
    const s32 x = flags & input_x ? input : temp;
    s32 y;
    switch (op.y_source) {
    case 0: y = util::sext(fraction, 13); break;
    case 1: y = dsp.COEF[op.coefficient] >> 3; break;
    case 2: y = util::sext(y_register >> 11, 13); break;
    default: y = (y_register >> 4) & 0xfff; break;
    }
    if (flags & latch_y)
        y_register = input;

    s32 shifted = 0;
    if (op.needs_shift) {
        const s32 value = s32(u32(accumulator) << (op.shift == 1 || op.shift == 2));
        shifted = op.shift < 2 ? std::clamp(value, -0x800000, 0x7fffff) : util::sext(value, 24);
    }
    accumulator = s32((s64(x) * y >> 12) + (flags & negate_b ? -s64(b) : b));

    if (flags & write_temp)
        dsp.TEMP[(op.temp_write + dec) & 127] = shifted;
    if (flags & latch_fraction)
        fraction = op.shift == 3 ? shifted & 0xfff : (shifted >> 11) & 0x1fff;

    if (flags & (ram_read | ram_write)) {
        u32 address = dsp.MADRS[op.ram_address];
        if (!(flags & table))
            address += dec;
        if (flags & add_address)
            address += address_register & 0xfff;
        if (flags & next_address)
            ++address;
        address &= flags & table ? 0xffff : dsp.RBL - 1;
        address = (address + (dsp.RBP << 12)) << 1;
        if (flags & ram_read) {
            const u16 word = dsp.read_word(address);
            memory_value = flags & no_float ? word << 8 : unpack_sample(word);
        }
        if (flags & ram_write)
            dsp.write_word(address, flags & no_float ? shifted >> 8 : pack_sample(shifted));
    }

    if (flags & latch_address)
        address_register = op.shift == 3 ? (shifted >> 12) & 0xfff : input >> 16;
    if (flags & effect)
        dsp.EFREG[op.effect_write] += shifted >> 8;
}

#ifdef VF2_SCSP_NATIVE_PROGRAM
template<u16 A, u16 B, u16 C, u16 D, bool Odd>
[[gnu::always_inline]] inline void native_op(SCSPDSP &dsp, SampleState &state, u32 dec) {
    constexpr u16 words[] = {A, B, C, D};
    constexpr auto op = decode(words, Odd);
    static_assert(op.input <= 0x31);
    execute_op(dsp, state, dec, op);
}
#include "vf2-scsp-native-steps.inc"
#ifdef VF2_SCSP_NATIVE_PROOF
unsigned native_samples = 0;
void report_native_samples() { std::fprintf(stderr, "VF2 SCSP native samples %u\n", native_samples); }
#endif
#endif

struct Program {
    std::array<u16, 512> words{};
    std::array<DecodedStep, 128> steps{};
    int length = -1;
    int end = 0;
#ifdef VF2_SCSP_NATIVE_PROGRAM
    bool native = false;
#endif

    void load(const SCSPDSP &dsp) {
        const auto bytes = sizeof(u16) * dsp.LastStep * 4;
        if (length == dsp.LastStep && !std::memcmp(words.data(), dsp.MPRO, bytes))
            return;
        length = dsp.LastStep;
        std::memcpy(words.data(), dsp.MPRO, bytes);
        end = 0;
        for (; end < length; ++end) {
            const auto op = decode(words.data() + 4 * end, end & 1);
            if (op.input > 0x31)
                break;
            steps[end] = op;
        }
#ifdef VF2_SCSP_LOCAL_PROGRAM
        static unsigned printed = 0;
        if (end == 82 && printed++ < 4) {
            std::fputs("SCSP local program", stderr);
            for (int i = 0; i < end * 4; ++i)
                std::fprintf(stderr, " %04x", unsigned(words[i]));
            std::fputc('\n', stderr);
        }
#endif
#ifdef VF2_SCSP_NATIVE_PROGRAM
        native = bytes == sizeof(native_words) && !std::memcmp(words.data(), native_words, bytes);
#endif
    }
};

void execute(SCSPDSP &dsp, const Program &program) {
    std::fill(std::begin(dsp.EFREG), std::end(dsp.EFREG), 0);
    SampleState state;
    const u32 dec = dsp.DEC;
#ifdef VF2_SCSP_NATIVE_PROGRAM
    static const bool use_native = std::getenv("VF2_SCSP_NATIVE") != nullptr;
    if (use_native && program.native) {
        execute_native_program(dsp, state, dec);
#ifdef VF2_SCSP_NATIVE_PROOF
        if (native_samples++ == 0)
            std::atexit(report_native_samples);
#endif
    } else
#endif
    {
        for (int i = 0; i < program.end; ++i)
            execute_op(dsp, state, dec, program.steps[i]);
    }
    if (program.end == program.length) {
        --dsp.DEC;
        std::fill(std::begin(dsp.MIXS), std::end(dsp.MIXS), 0);
    }
}
}

void vf2_scsp_decoded_step(SCSPDSP &dsp) {
    if (dsp.Stopped)
        return;
    thread_local Program program;
    program.load(dsp);
    execute(dsp, program);
}
