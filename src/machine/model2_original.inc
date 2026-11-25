// license:BSD-3-Clause
// copyright-holders:Dirk Best, AJR
// Original Model 2 I/O wiring, based on MAME's model1io, 315_5338a
// and msm6253 devices. The left side runs the cabinet's own Z80 firmware.
#define CHIPS_IMPL
#include "../../third_party/chips/z80.h"
#undef CHIPS_IMPL

struct model2a::original_io_board
{
	explicit original_io_board(model2a &board) : m(board), rom(board.m_roms.io_firmware) { reset(); }
	~original_io_board()
	{
		if (delay_ticks_skipped)
			std::fprintf(stderr, "IO Z80 delay: %llu T-states batched\n", (unsigned long long)delay_ticks_skipped);
		if (serial_ticks_skipped)
			std::fprintf(stderr, "IO Z80 serial status: %llu T-states batched\n", (unsigned long long)serial_ticks_skipped);
		if (transfer_ticks_skipped)
			std::fprintf(stderr, "IO Z80 serial transfer: %llu T-states batched\n", (unsigned long long)transfer_ticks_skipped);
		if (!pc_samples) return;
		unsigned total = 0;
		for (unsigned count : *pc_samples) total += count;
		std::fprintf(stderr, "IO Z80 PC samples: %u at one sample per 997 T-states\n", total);
		for (unsigned rank = 0; rank < 32; ++rank) {
			auto top = std::max_element(pc_samples->begin(), pc_samples->end());
			if (!*top) break;
			std::fprintf(stderr, "IO Z80 PC %04x %u\n", unsigned(top - pc_samples->begin()), *top);
			*top = 0;
		}
	}

	void reset()
	{
		pins = z80_reset(&cpu);
		ram.fill(0);
		shared.fill(0);
		ports.fill(0xff);
		direction = output = command = adc = 0;
		address = 0;
		secondary = false;
		pending_cycles = 0;
	}

	void run(int cycles, uint64_t start)
	{
		static const bool profile = std::getenv("VF2_IO_Z80_PROFILE") != nullptr;
		if (profile) {
			if (!pc_samples) pc_samples = std::make_unique<pc_histogram>();
			run_ticks<true, false>(cycles, start);
		} else if (batch_serial) {
			if (batch_delay) run_ticks<false, true, true>(cycles, start);
			else run_ticks<false, false, true>(cycles, start);
		} else if (batch_delay) run_ticks<false, true>(cycles, start);
		else run_ticks<false, false>(cycles, start);
	}

    // Accrue the same scheduler allotments and replay them in order before
    // the i960 observes shared RAM or the next frame changes cabinet inputs.
    void schedule(int cycles, uint64_t start)
    {
        if (pending_cycles && pending_start + pending_cycles != start)
            flush();
        if (!pending_cycles) pending_start = start;
        pending_cycles += cycles;
    }

    void flush()
    {
        if (!pending_cycles) return;
        run(pending_cycles, pending_start);
        pending_cycles = 0;
    }

private:
    uint64_t pending_start = 0;
    int pending_cycles = 0;
	uint64_t delay_ticks_skipped = 0;
	uint64_t serial_ticks_skipped = 0;
	uint64_t transfer_ticks_skipped = 0;

	int batch_taken_delay(int available)
	{
		// DEC A / JR NZ,$0868 is 16 T-states when taken. After the first
		// taken branch the two fetch boundaries differ only in A, F and R.
		// Each omitted iteration reads immutable program ROM and touches no
		// cabinet registers. Execute the fall-through and any partial
		// iteration through the original core.
		constexpr uint64_t fetch = Z80_M1 | Z80_MREQ | Z80_RD | 0x0868;
		if (pins != (fetch | (uint64_t(0x3d) << 16)) || available < 16 || cpu.a <= 1 ||
			cpu.pins != (fetch | (uint64_t(0xfd) << 16)) || cpu.pc != 0x0869 ||
			cpu.step != Z80_M1_T2 || cpu.wz != 0x0868 || cpu.opcode != 0x20 ||
			cpu.dlatch != 0xfd || cpu.int_bits || cpu.prefix_active || cpu.hlx_idx)
			return 0;
		const int iterations = std::min(available / 16, int(cpu.a) - 1);
		cpu.a = _z80_dec8(&cpu, u8(cpu.a - iterations + 1));
		cpu.r = u8((cpu.r & 0x80) | ((cpu.r + 2 * iterations) & 0x7f));
		const int skipped = 16 * iterations;
		delay_ticks_skipped += skipped;
		return skipped;
	}

	int batch_serial_status(int available)
	{
		constexpr int elapsed = 97;
		constexpr uint64_t entry = Z80_M1 | Z80_MREQ | Z80_RD | 0x0815;
		constexpr uint64_t exit = Z80_M1 | Z80_MREQ | Z80_RD | 0x0826;
		if (available < elapsed || pins != (entry | (uint64_t(0xf5) << 16)) ||
			cpu.pc != 0x0816 || cpu.step != Z80_M1_T2 || cpu.prefix_active ||
			cpu.hlx_idx || cpu.int_bits || cpu.iy != 0x8000 ||
			cpu.sp < 0x4004 || cpu.sp > 0x6000)
			return 0;

		// PUSH AF, PUSH BC, read the fixed serial status, then POP BC and
		// POP AF. Keep all four private RAM writes and stop at RET fetch.
		ram[(cpu.sp - 1) & 0x1fff] = cpu.a;
		ram[(cpu.sp - 2) & 0x1fff] = cpu.f;
		ram[(cpu.sp - 3) & 0x1fff] = cpu.b;
		ram[(cpu.sp - 4) & 0x1fff] = cpu.c;
		cpu.pc = 0x0827;
		cpu.wz = 0x0827;
		cpu.addr = cpu.hl;
		cpu.dlatch = 0xf5;
		cpu.opcode = 0xf1;
		cpu.r = u8((cpu.r & 0x80) | ((cpu.r + 12) & 0x7f));
		cpu.pins = exit | (uint64_t(cpu.a) << 16);
		pins = exit | (uint64_t(0xc9) << 16);
		serial_ticks_skipped += elapsed;
		return elapsed;
	}

	int batch_serial_transfer(int available)
	{
		constexpr uint64_t entry = Z80_M1 | Z80_MREQ | Z80_RD | 0x082f;
		if (pins != (entry | (uint64_t(0xf5) << 16)) ||
			cpu.pc != 0x0830 || cpu.step != Z80_M1_T2 || cpu.prefix_active ||
			cpu.hlx_idx || cpu.int_bits || cpu.iy != 0x8000 ||
			cpu.sp < 0x4004 || cpu.sp > 0x6000 ||
			(cpu.sp >= 0x5804 && cpu.sp <= 0x5807))
			return 0;

		// The status read returns 8: the poll completes immediately.
		// Both resulting paths use private RAM only, until their RET.
		const u8 control = ram[0x1803];
		const int elapsed = control ? 146 : 169;
		if (available < elapsed) return 0;
		ram[(cpu.sp - 1) & 0x1fff] = cpu.a;
		ram[(cpu.sp - 2) & 0x1fff] = cpu.f;
		ram[(cpu.sp - 3) & 0x1fff] = cpu.b;
		ram[(cpu.sp - 4) & 0x1fff] = cpu.c;
		if (control) ram[0x1803] = control - 1;
		const u16 ret = control ? 0x084b : 0x085c;
		const uint64_t exit = Z80_M1 | Z80_MREQ | Z80_RD | ret;
		cpu.hl = cpu.addr = 0x5803;
		cpu.pc = ret + 1;
		cpu.wz = control ? 0x084c : 0x085a;
		cpu.dlatch = control ? u8(control - 1) : 0x80;
		cpu.opcode = 0xf1;
		cpu.r = u8((cpu.r & 0x80) | ((cpu.r + (control ? 18 : 21)) & 0x7f));
		cpu.pins = exit | (uint64_t(cpu.a) << 16);
		pins = exit | (uint64_t(0xc9) << 16);
		transfer_ticks_skipped += elapsed;
		return elapsed;
	}

	template<bool Profile, bool BatchDelay, bool BatchSerial = false>
	void run_ticks(int cycles, uint64_t start)
	{
		for (int tick = 0; tick < cycles; ++tick) {
			pins = z80_tick(&cpu, pins);
			if constexpr (Profile) {
				if (--sample_after == 0) {
					++(*pc_samples)[cpu.pc];
					sample_after = 997;
				}
			}
			if (!(pins & Z80_MREQ)) continue;
			const u16 addr = Z80_GET_ADDR(pins);
			if (pins & Z80_RD) {
				const u8 data = read(addr, (start + tick) * 25 / 4);
				Z80_SET_DATA(pins, data);
				if constexpr (BatchSerial) {
					if (addr == 0x0815) tick += batch_serial_status(cycles - tick - 1);
					else if (addr == 0x082f && batch_transfer) tick += batch_serial_transfer(cycles - tick - 1);
				}
			}
			if (pins & Z80_WR) write(addr, Z80_GET_DATA(pins), (start + tick) * 25 / 4);
			if constexpr (BatchDelay) tick += batch_taken_delay(cycles - tick - 1);
		}
	}

public:
	std::array<u8, 0x800> shared{};

	// Save states (src/machine/state_io.h): the cabinet's Z80, its pins and
	// every register the i960 reads through `shared`. Without these a loaded
	// checkpoint restarts the I/O firmware from power-on and the game's input
	// polling sees a board that was never initialised.
	template <class IO> void state(IO &io)
	{
		io.bytes(&cpu, sizeof cpu);
		io.pod(pins);
		io.bytes(ram.data(), ram.size());
		io.bytes(ports.data(), ports.size());
		io.pod(address);
		io.pod(direction); io.pod(output); io.pod(command); io.pod(adc);
		io.pod(secondary);
		io.bytes(shared.data(), shared.size());
	}

private:
	using pc_histogram = std::array<unsigned, 65536>;
	std::unique_ptr<pc_histogram> pc_samples;
	unsigned sample_after = 997;

	u8 port_in(unsigned port, uint64_t now) const
	{
		if (port == 1) return secondary ? 0xff : m.m_in0;
		if (port == 2 && !secondary) {
			static constexpr u8 gearbox[]{0, 2, 1, 6, 5};
			return (m.m_in1 & 0x8f) | (gearbox[m.m_driving_gear] << 4);
		}
		if (port == 6) return 0x7f | (m.m_eeprom.do_r(now) << 7);
		return 0xff;
	}

	void port_out(unsigned port, u8 data, uint64_t now)
	{
		if (port != 0) return;
		m.m_eeprom.clk_w(BIT(data, 7), now);
		m.m_eeprom.di = BIT(data, 5);
		m.m_eeprom.cs_w(BIT(data, 6), now);
		secondary = BIT(data, 0);
	}

	u8 read(u16 addr, uint64_t now)
	{
		if (addr < 0x4000) return addr < rom.size() ? rom[addr] : 0xff;
		if (addr < 0x6000) return ram[addr & 0x1fff];
		if (addr >= 0xc000 && addr < 0xc004) {
			const u8 bit = adc >> 7;
			adc <<= 1;
			return bit;
		}
		if (addr < 0x8000 || addr >= 0x8010) return 0xff;
		const unsigned port = addr & 15;
		if (port < 7) return BIT(direction, port) ? port_in(port, now) : ports[port];
		switch (port) {
		case 8: return direction;
		case 10: return output;
		case 11: return command;
		case 12: return shared[address & 0x7ff];
		case 13: return 8; // serial transfer complete
		default: return 0xff;
		}
	}

	void write(u16 addr, u8 data, uint64_t now)
	{
		if (addr >= 0x4000 && addr < 0x6000) {
			ram[addr & 0x1fff] = data;
			return;
		}
		if (addr >= 0xc000 && addr < 0xc004) {
			const unsigned channel = addr & 3;
			adc = !secondary && channel < m.m_driving_analog.size() ? m.m_driving_analog[channel] : 0xff;
			return;
		}
		if (addr < 0x8000 || addr >= 0x8010) return;
		const unsigned port = addr & 15;
		if (port < 7) {
			ports[port] = data;
			port_out(port, data, now);
		} else if (port == 8) {
			for (unsigned i = 0; i < ports.size(); ++i)
				if (BIT(direction & ~data, i)) port_out(i, ports[i], now);
			direction = data;
		} else if (port == 10) {
			output = data;
		} else if (port == 9) {
			command = data;
			if (data == 0) address = (address & 0xff00) | output;
			else if (data == 1) address = (address & 0xff) | (output << 8);
			else if (data == 7) shared[address & 0x7ff] = output;
			else if (data >= 0x70 && data <= 0x77) shared[data & 7] = output;
		}
	}

	model2a &m;
	const std::vector<u8> &rom;
	const bool batch_delay = std::getenv("VF2_IO_Z80_BATCH_DELAY") && rom.size() > 0x086a &&
		rom[0x0868] == 0x3d && rom[0x0869] == 0x20 && rom[0x086a] == 0xfd;
	// The shortcut is valid only for the tested cabinet firmware sequence.
	const bool batch_serial = std::getenv("DAYTONA_Z80_SERIAL_NATIVE") &&
		rom.size() >= 0x0827 && std::equal(serial_code.begin(), serial_code.end(), rom.begin() + 0x0815);
	static constexpr std::array<u8, 18> serial_code = {
		0xf5, 0xc5, 0x06, 0x0f, 0xfd, 0x7e, 0x0d, 0x05, 0xca,
		0x27, 0x08, 0xcb, 0x47, 0x20, 0xf5, 0xc1, 0xf1, 0xc9
	};
	const bool batch_transfer = std::getenv("DAYTONA_Z80_TRANSFER_NATIVE") &&
		rom.size() >= 0x085d && std::equal(transfer_code.begin(), transfer_code.end(), rom.begin() + 0x082f);
	static constexpr std::array<u8, 46> transfer_code = {
		0xf5, 0xc5, 0x06, 0x0f, 0xfd, 0x4e, 0x0d, 0x05, 0xca, 0x5d, 0x08, 0x79,
		0xcb, 0x5f, 0x28, 0xf4, 0x21, 0x03, 0x58, 0x7e, 0xfe, 0x00, 0xca, 0x4c, 0x08,
		0x35, 0xc1, 0xf1, 0xc9, 0x21, 0x03, 0x58, 0x7e, 0xfe, 0x80, 0xda, 0x5a, 0x08,
		0x21, 0x00, 0x80, 0xcb, 0x8e, 0xc1, 0xf1, 0xc9
	};
	z80_t cpu{};
	uint64_t pins = 0;
	std::array<u8, 0x2000> ram{};
	std::array<u8, 7> ports{};
	u16 address = 0;
	u8 direction = 0, output = 0, command = 0, adc = 0;
	bool secondary = false;
};

void model2a::configure_standalone_cabinet()
{
	// The Revision A program's factory settings block defaults to a linked
	// cabinet. Seed a fresh EEPROM from that block with link ID set to single.
	std::array<u8, 0x80> settings;
	memcpy(settings.data(), m_rom.data() + 0x39d60, settings.size());
	settings[0x0b] = 0;
	settings[0x7c] = 1; // initialized settings
	u16 crc = 0;
	for (size_t i = 0x0a; i < settings.size(); ++i) {
		crc ^= u16(settings[i]) << 8;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
	}
	settings[8] = u8(crc);
	settings[9] = u8(crc >> 8);
	for (size_t i = 0; i < settings.size() / 2; ++i)
		m_eeprom.cells[i] = (u16(settings[2 * i]) << 8) | settings[2 * i + 1];
}

bool model2a::configure_twin_display_curve()
{
    // Revision A stores the cabinet type at byte 0x1a of the EEPROM.
    constexpr unsigned cabinet_setting = 0x1a;
    constexpr unsigned twin = 1;
    if ((m_eeprom.cells[cabinet_setting / 2] >> 8) != twin)
        return false;

    // Twin's CRT COLOR test emits 31 non-black steps with the same
    // response in red, green, blue and gray. Recover evenly spaced
    // digital codes and fold the correction into the output lookup.
    constexpr std::array<int, 32> levels{
        0, 54, 61, 68, 74, 81, 88, 94, 101, 108, 114, 121, 128, 134, 141, 148,
        154, 161, 168, 174, 181, 188, 194, 201, 208, 214, 221, 228, 234, 241, 248, 255,
    };
    const auto target = [](int step) { return (255 * step + 15) / 31; };
    for (u8 &code : m_gamma) {
        if (code == 0 || code == 255)
            continue;
        const int hi = int(std::upper_bound(levels.begin(), levels.end(), code) - levels.begin());
        const int lo = hi - 1;
        const int span = levels[hi] - levels[lo];
        const int numerator = target(lo) * span + (code - levels[lo]) * (target(hi) - target(lo));
        const int value = numerator / span;
        const int twice_remainder = 2 * (numerator % span);
        code = u8(value + (twice_remainder > span || (twice_remainder == span && (value & 1))));
    }
    return true;
}
