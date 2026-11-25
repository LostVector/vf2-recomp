#include "emu.h"
#include "i960.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>

struct frame_bus : mame_bus32 {
    static constexpr u32 base = 0x00500000;
    bool use_direct;
    unsigned offers = 0;
    unsigned accepted = 0;
    std::array<u8, 128> ram{};
    std::vector<std::pair<bool, u32>> accesses;

    explicit frame_bus(bool direct) : use_direct(direct) {}
    u8 read_byte(offs_t) override { std::abort(); }
    u16 read_word(offs_t) override { std::abort(); }
    void write_byte(offs_t, u8) override { std::abort(); }
    void write_word(offs_t, u16) override { std::abort(); }

    u32 read_dword(offs_t a) override {
        accesses.emplace_back(false, a);
        u32 data;
        memcpy(&data, ram.data() + (a & 124), sizeof data);
        return data;
    }
    void write_dword(offs_t a, u32 data) override {
        accesses.emplace_back(true, a);
        memcpy(ram.data() + (a & 124), &data, sizeof data);
    }
    u8 *direct_register_frame(offs_t a) override {
        ++offers;
        if (!use_direct || a - base > 64u) return nullptr;
        ++accepted;
        return ram.data() + (a - base);
    }
};

struct frame_cpu : i80960kb_device {
    explicit frame_cpu(frame_bus &bus)
        : i80960kb_device(machine_config{}, "fixture", nullptr, 25000000) {
        set_bus(&bus);
        start();
    }
    void seed(u32 a) {
        for (unsigned i = 0; i < 16; ++i) m_r[i] = 0xfabc0000u + i;
        m_r[I960_FP] = a;
        m_r[I960_SP] = a + 64;
        m_IP = 0x11004;
        m_icount = 200;
        m_rcache_pos = I960_RCACHE_SIZE;
    }
    void call() { do_call(0x12100, 0, m_r[I960_SP]); }
    void ret() { do_ret(); }
    void prepare_flush(u32 a) {
        seed(a);
        m_rcache_pos = 1;
        m_rcache_frame_addr[0] = a;
        for (unsigned i = 0; i < 16; ++i) m_rcache[0][i] = 0xeabc0000u + i;
    }
    void flush() { execute_op(0x66000000u | (0xdu << 7)); }
    bool matches(const frame_cpu &other) const {
        return memcmp(m_r, other.m_r, sizeof m_r) == 0 &&
            memcmp(m_rcache, other.m_rcache, sizeof m_rcache) == 0 &&
            m_IP == other.m_IP && m_icount == other.m_icount &&
            m_rcache_pos == other.m_rcache_pos;
    }
};

void require(bool condition) {
    if (!condition) std::abort();
}

int main() {
    constexpr u32 ram = frame_bus::base;
    for (u32 address : {ram, ram + 64, ram + 128, 0x00884000u}) {
        frame_bus fast(true), ref(false);
        frame_cpu a(fast), b(ref);
        a.seed(address);
        b.seed(address);
        a.call();
        b.call();
        require(a.matches(b) && fast.ram == ref.ram);
        a.ret();
        b.ret();
        require(a.matches(b) && fast.ram == ref.ram);
        require(fast.offers == 2 && ref.accesses.size() == 32);
        if (address <= ram + 64)
            require(fast.accepted == 2 && fast.accesses.empty());
        else
            require(fast.accepted == 0 && fast.accesses == ref.accesses);
    }
    for (u32 address : {ram, ram + 64, ram + 4, 0x00884000u}) {
        frame_bus fast(true), ref(false);
        frame_cpu a(fast), b(ref);
        a.prepare_flush(address);
        b.prepare_flush(address);
        a.flush();
        b.flush();
        require(a.matches(b) && fast.ram == ref.ram && ref.accesses.size() == 16);
        if (address == ram || address == ram + 64)
            require(fast.accepted == 1 && fast.accesses.empty());
        else {
            require(fast.accepted == 0 && fast.accesses == ref.accesses);
            if (address == ram + 4) require(fast.offers == 0);
        }
    }
    std::cout << "Eight i960 register frame cases matched state, cycles and RAM; device and unaligned accesses retained word order\n";
}
