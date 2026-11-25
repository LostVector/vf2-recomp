// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Dirk Best, Aaron Giles, smf, Robbbert
// Sega Model 2A-CRX board glue. Ported from MAME's sega/model2.cpp, model2_v.cpp (BSD-3-Clause,
// copyright-holders: R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels), with
// the 315-5649 I/O chip (sega/315_5649.cpp, Dirk Best), the 93C46 serial EEPROM
// (machine/eepromser.cpp, Aaron Giles) and the i8251 UART's transmitter (machine/i8251.cpp, smf, Robbbert)
// reduced to what VF2 uses; the scheduling is MAME's emu/schedule.cpp and emu/diexec.cpp (Aaron
// Giles), with machine/gen_fifo.cpp (Olivier Galibert) for the two FIFOs between i960 and TGP.
// The sound board's 68000 is MAME's (cpu/m68000, Olivier Galibert), on the board's bus.
#include "model2a.h"
#include "../core/scheduler_gap.h"
#include "../core/frame_clock.h"
#include "parallel.h"
#include <zlib.h>
#include <chrono>
#include <cstdlib>
#include <bit>
#include <random>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------------------------
// The TGP (MB86234): MAME's copro_tgp_* maps. Its FIFOs are the board's (fifo_pop/fifo_push).
// ---------------------------------------------------------------------------------------------

static const bool g_tgp_stable_program = getenv("VF2_TGP_STABLE_PROGRAM") != nullptr;
static const bool g_track_geometry_pages = getenv("VF2_TRACK_GEOMETRY_PAGES") != nullptr;

struct model2a::tgp_bus : mb86233_bus
{
	model2a &m;
	mb86233_device cpu{*this};
	u32 prog[0x1000]{};
	uint64_t program_revision_id=1;
    // The hardware's unmapped 0100-01ff range stays zero.
    u32 ram[0x400]{};
	u32 bank_reg = 0;
	u32 sincos_base = 0, inv_base = 0, isqrt_base = 0, atan_base[4]{};
	const u32 *tables;   // copro_tgp_tables as 32-bit words

	explicit tgp_bus(model2a &mm) : m(mm), tables(reinterpret_cast<const u32 *>(mm.m_roms.regions[romset::COPRO_TABLES].data())) {}

	int run(int cycles)
	{
		if(g_tgp_stable_program)return cpu.run_mapped(cycles,prog,program_revision_id);
		return cpu.run(cycles);
	}

	u32 prog_r(u32 a) override { return prog[a & 0xfff]; }
	const u32 *program_data()const override{return prog;}
	uint64_t program_revision()const override{return program_revision_id;}
	internal_ram direct_internal_ram()override{
        static const bool read_window=std::getenv("VF2_TGP_READ_WINDOW")!=nullptr;
        return {ram,ram+0x200,read_window?ram:nullptr};
    }
    bool isolated_internal_ram()const override{return true;}
    bool native_control_callbacks()const override{return true;}

	u32 data_r(u32 a) override
	{
		a &= 0xffff;
		if (a < 0x100 || (a >= 0x200 && a < 0x400)) return ram[a];
		return 0;
	}

	void data_w(u32 a, u32 v) override
	{
		a &= 0xffff;
		if (a < 0x100 || (a >= 0x200 && a < 0x400)) ram[a] = v;
	}

	// copro_tgp_io_map: the table units, and over them all the bank view when it's enabled
	bool view() const { return bank_reg & 0xc00000; }

	u32 memory_r(u32 offset)
	{
		u32 adr = (bank_reg & 0xff0000) | offset;
		if (adr & 0x800000) return 0;    // copro_data: empty on VF2
		if (adr & 0x400000) { u32 v; memcpy(&v, &m.m_bufferram[(adr & 0x7fff) * 4], 4); return v; }
		return 0;
	}

	void memory_w(u32 offset, u32 data)
	{
		u32 adr = (bank_reg & 0xff0000) | offset;
		if (adr & 0x400000) {
            const u32 offset=(adr&0x7fff)*4;
            if(g_track_geometry_pages)m.m_video->mark_geometry_page(offset);
            memcpy(&m.m_bufferram[offset],&data,4);
        }
	}

	u32 io_r(u32 a) override
	{
		a &= 0xffff;
		if (view()) return memory_r(a);
		if (a >= 0x20 && a < 0x24) return sincos_r(a - 0x20);
		if (a >= 0x24 && a < 0x28) return atan_r();
		if (a >= 0x28 && a < 0x2a) return inv_r(a - 0x28);
		if (a >= 0x2a && a < 0x2c) return isqrt_r(a - 0x2a);
		return 0;
	}

	void io_w(u32 a, u32 v) override
	{
		a &= 0xffff;
		if (view()) { memory_w(a, v); return; }
		if (a >= 0x20 && a < 0x24) sincos_base = v;
		else if (a >= 0x24 && a < 0x28) {
			atan_base[a - 0x24] = v;
			cpu.gpio0_w((atan_base[0] & 0x7fffffff) <= (atan_base[1] & 0x7fffffff));
		}
		else if (a >= 0x28 && a < 0x2a) inv_base = v;
		else if (a >= 0x2a && a < 0x2c) isqrt_base = v;
	}

	// copro_tgp_rf_map
	u32 rf_r(u32 a) override
	{
		if ((a & 0xf) == 1) return m.fifo_pop(false);   // copro_fifo_in
		return 0;
	}

	void rf_w(u32 a, u32 v) override
	{
		switch (a & 0xf) {
		case 2: m.fifo_push(true, v); break;              // copro_fifo_out
		case 3: bank_reg = v; break;
		}
	}

	u32 sincos_r(u32 offset)
	{
		offs_t ang = sincos_base + offset * 0x4000;
		offs_t index = ang & 0x3fff;
		if (ang & 0x4000)
			index = std::min(0x4000 - int(index), 0x3fff);
		u32 result = tables[index];
		if (ang & 0x8000)
			result ^= 0x80000000;
		return result;
	}

	u32 inv_r(u32 offset)
	{
		offs_t index = ((inv_base >> 9) & 0x3ffe) | (offset & 1);
		u32 result = tables[index | 0x8000];
		u8 bexp = (inv_base >> 23) & 0xff;
		u8 exp = (result >> 23) + (0x7f - bexp);
		result = (result & 0x007fffff) | (exp << 23);
		if (inv_base & 0x80000000 && offset)
			result |= 0x80000000;
		return result;
	}

	u32 isqrt_r(u32 offset)
	{
		offs_t index = 0x2000 ^ (((isqrt_base >> 10) & 0x3ffe) | (offset & 1));
		u32 result = tables[index | 0xc000];
		u8 bexp = (isqrt_base >> 24) & 0x7f;
		u8 exp = (result >> 23) + (0x3f - bexp);
		result = (result & 0x807fffff) | (exp << 23);
		if (!(offset & 1))
			result &= 0x7fffffff;
		return result;
	}

	u32 atan_r()
	{
		u8 ie = 0x88 - (atan_base[3] >> 23);
		bool s0 = atan_base[0] & 0x80000000;
		bool s1 = atan_base[1] & 0x80000000;
		bool s2 = (atan_base[0] & 0x7fffffff) <= (atan_base[1] & 0x7fffffff);
		offs_t im = atan_base[3] & 0x7fffff;
		offs_t index = ie <= 0x17 ? (im | 0x800000) >> ie : 0;
		if (index == 0x4000)
			index = 0x3fff;
		u32 result = tables[(index | 0x4000) & 0xffff];   // (the tables' 64K words: a far index (ie < 9, a program's own, as a snapshot's can be) read past them)
		if (s0 ^ s1 ^ s2)
			result >>= 16;
		if (s2)
			result += 0x4000;
		if ((s0 && !s2) || (s1 && s2))
			result += 0x8000;
		return result & 0xffff;
	}
};

mb86233_device &model2a::tgp() { return m_tgp->cpu; }

// ---------------------------------------------------------------------------------------------

void model2a::release_unused_roms()
{
	for (int id : {romset::COPRO_DATA, romset::OTHER_DATA, romset::VIDEO_UNK})
		std::vector<u8>().swap(m_roms.regions[id]);
#if defined(__linux__)
	// VF2's 36 MiB data and 32 MiB polygon regions contain large zero-padded
	// ranges. Keep their address/mirror semantics, but let Linux back untouched
	// zero pages with its shared zero page rather than private physical RAM.
	// Exclude allocator headers and any neighbouring allocation at both ends.
	const size_t page=size_t(sysconf(_SC_PAGESIZE));
	if (!page || (page & (page-1))) return;
	std::vector<u8> zero(page,0);
	for (auto &region : m_roms.regions) {
		if(region.empty())continue;
		uintptr_t begin=(reinterpret_cast<uintptr_t>(region.data())+page-1)&~uintptr_t(page-1);
		uintptr_t end=(reinterpret_cast<uintptr_t>(region.data())+region.size())&~uintptr_t(page-1),run=0;
		for(uintptr_t at=begin;at<end;at+=page){
			if(!std::memcmp(reinterpret_cast<const void*>(at),zero.data(),page)){if(!run)run=at;}
			else if(run){madvise(reinterpret_cast<void*>(run),at-run,MADV_DONTNEED);run=0;}
		}
		if(run)madvise(reinterpret_cast<void*>(run),end-run,MADV_DONTNEED);
	}
#endif
}

model2a::model2a(romset::result &&roms, bool recompile)
	: m_roms(std::move(roms)), m_rom(m_roms.regions[romset::MAINCPU]), m_data(m_roms.regions[romset::MAIN_DATA]),
	  m_cpu(*this)
{
	for (const std::vector<u8> &r : m_roms.regions) m_rom_hash = m_rom_hash * 0x100000001B3ull ^ m2::state_hash(r.data(), r.size());
	m_tgp = std::make_unique<tgp_bus>(*this);
	m_ram200.assign(0x40000, 0);
	m_workram.assign(0x100000, 0);
	m_bufferram.assign(0x20000, 0);
	m_backup.assign(0x4000, 0xff);     // NVRAM DEFAULT_ALL_1 (a never-saved board)
	m_cpuctl.assign(0x38, 0);
	m_palram.assign(0x2000, 0);
	m_colorxlat.assign(0x6000, 0);
	m_fbvram_a.assign(0x40000, 0);
	m_fbvram_b.assign(0x40000, 0);
	m_tex0.assign(0x80000, 0);
	m_tex1.assign(0x80000, 0);
	m_lumaram.assign(0x8000, 0);
	m_m2comm_share.assign(0x4000, 0);
	std::fill(std::begin(m_eeprom.cells), std::end(m_eeprom.cells), 0xffff);   // a never-written 93C46
	// model2_state::video_start: bias 64, gain 255/191
	for (int i = 0; i < 256; i++)
		m_gamma[i] = u8(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
	m_pens.fill(0xff000000);   // rgb_t::black(), until the game writes them
	const auto &tex = m_roms.regions[romset::TEXTURES], &poly = m_roms.regions[romset::POLYGONS];
	m_video = std::make_unique<model2_video>(reinterpret_cast<u32 *>(m_bufferram.data()), m_tex0.data(), m_tex1.data(),
		m_lumaram.data(), m_palram.data(), m_colorxlat.data(), m_gamma, tex.data(), tex.size(), poly.data(), poly.size());
	m_video->m_fbvramA = m_fbvram_a.data();
	m_video->m_fbvramB = m_fbvram_b.data();
	resize_screen();
	setup_pages();
	m_cpu.on_irq_ack = [this](int line, offs_t) { m_irq_count[line & 3]++; };
	// I80960KB at 50 MHz / 2; MB86234 at 50 MHz, a cycle every 3 clocks
	m_sched[CPU_I960].set_rate(25000000);
	m_sched[CPU_TGP].set_rate((50000000 + 3 - 1) / 3);
	m_sched[CPU_68K].set_rate(45158400 / 4);
	m_sched[CPU_Z80].set_rate(32000000 / 4);
	// the i960's program recompiled to native code now (src/rec), unless VF2_INTERP is set (MAME's
	// interpreter: the reference, cycle for cycle the same)
	if (recompile && !getenv("VF2_INTERP")) use_native();
}

// device_execute_interface::clock_changed
void model2a::cpu_sched::set_rate(uint64_t cycles_per_second)
{
	period = AS_PER_SEC / cycles_per_second;
	uint64_t attos = uint64_t(period);
	elapsed.set(attos);
	divshift = 0;
	while (attos >= (1ull << 31)) { divshift++; attos >>= 1; }
	divisor = u32(attos);
	divide_cycles.set(divisor);
}

model2a::~model2a() { render_wait(); m_video->geometry_wait(); }

// ---------------------------------------------------------------------------------------------
// The settings block in backup RAM (found by changing them in test mode): a CRC-16 (CCITT,
// polynomial 0x1021, from 0) of the bytes after a counter up to 0x2e, stored low byte first, a
// tag, the length (0x18), the game's name, then the settings from 0x18 (from 0x18 a counter the
// game keeps updating, outside the CRC: past 16 bits it reaches 0x1a, as in a board that has run
// for long). The coin/credit setting is byte 0x24: 0-based, #26 (0x19) is FREE PLAY.
// ---------------------------------------------------------------------------------------------

namespace {
constexpr u32 SETTINGS = 0x3300, SETTINGS_COIN = SETTINGS + 0x24, FREE_PLAY = 0x19;
uint64_t now_ns() { using namespace std::chrono; return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()); }
constexpr u32 NEXT_STAGE_PRINT = 0xfb0dda39;   // "TRY NEXT STAGE"'s backdrop_print (see wall_sides)

// (the counter before the CRC'd bytes grows into 0x1a and on as the board runs: in a backup that
// has run long, 0x1a moves too. The CRC is taken from the counter's end, the first of 0x1a-0x24
// it matches from; the bytes between are zero until the counter reaches them, and a CRC from 0
// over zeros stays 0, so a later start gives the same CRC while they are.)
u16 settings_crc(const std::vector<u8> &b, u32 from)
{
	u16 crc = 0;
	for (u32 i = SETTINGS + from; i < SETTINGS + 0x2f; i++) {
		crc ^= u16(b[i] << 8);
		for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? u16((crc << 1) ^ 0x1021) : u16(crc << 1);
	}
	return crc;
}

// where the CRC'd bytes start (0: the settings aren't valid)
u32 settings_from(const std::vector<u8> &b)
{
	if (b.size() < SETTINGS + 0x30 || memcmp(&b[SETTINGS + 8], "VIRTUA FIGHTER 2", 16) || b[SETTINGS + 6] != 0x18 || b[SETTINGS + 7] != 0) return 0;
	const u16 stored = u16(b[SETTINGS] | b[SETTINGS + 1] << 8);
	for (u32 from = 0x1a; from <= 0x24; from++)
		if (settings_crc(b, from) == stored) return from;
	return 0;
}

bool settings_valid(const std::vector<u8> &b) { return settings_from(b) != 0; }
}

bool model2a::free_play() const
{
	return settings_valid(m_backup) && m_backup[SETTINGS_COIN] == FREE_PLAY;
}

bool model2a::force_free_play() { return set_coin_setting(FREE_PLAY); }

int model2a::coin_setting() const { return settings_valid(m_backup) ? m_backup[SETTINGS_COIN] : -1; }

bool model2a::set_coin_setting(int v)
{
	const u32 from = settings_from(m_backup);
	if (!from || v < 0 || v > FREE_PLAY || m_backup[SETTINGS_COIN] == v) return false;
	m_backup[SETTINGS_COIN] = u8(v);
	const u16 crc = settings_crc(m_backup, std::min<u32>(from, 0x24));
	m_backup[SETTINGS] = u8(crc);
	m_backup[SETTINGS + 1] = u8(crc >> 8);
	return true;
}

// GAME ASSIGNMENT (found by changing each in test mode): the bytes from 0x3340, with their own
// CRC-16 (as the first block's: CCITT from 0) of 0x3340-0x335c, stored low byte first at 0x3302.
// DIFFICULTY sets the energy (0x3352 1P, 0x3354 VS, 16-bit) and the ring's width (0x334f, in
// hundreds) that the test mode shows under it; the switches are bits of 0x3351, set for the
// second choice.
namespace {
constexpr u32 ASSIGN = SETTINGS + 0x40, ASSIGN_END = SETTINGS + 0x5d, ASSIGN_CRC = SETTINGS + 2;
constexpr u32 WIDTH_AT = SETTINGS + 0x4f, FLAGS_AT = SETTINGS + 0x51;
struct difficulty_set { u8 width; u16 energy_1p, energy_vs; };
constexpr difficulty_set DIFFICULTY_SETS[4] = { { 16, 176, 220 }, { 15, 160, 200 }, { 14, 144, 180 }, { 13, 128, 160 } };   // EASY..HARDEST
struct assignment_at { u32 at; int lo, hi; u8 bit; };   // bit: a switch in the flags byte (else the byte, lo-hi)
constexpr assignment_at ASSIGNMENTS[] = {
	{ SETTINGS + 0x40, 1, 5, 0 },      // MATCH COUNT(1P): rounds to win (1 too: the test mode offers 2-5, the game takes 0-5, 0x9e7c)
	{ SETTINGS + 0x41, 2, 5, 0 },      // MATCH COUNT(VS)
	{ SETTINGS + 0x42, 0, 3, 0 },      // DIFFICULTY: EASY, NORMAL, HARD, HARDEST
	{ FLAGS_AT, 0, 1, 0x01 },          // ADVERTISE SOUND (the bit set: OFF)
	{ FLAGS_AT, 0, 1, 0x02 },          // CONTINUE (set: OFF)
	{ FLAGS_AT, 0, 1, 0x40 },          // VERSION (set: VERSION 2.1)
	{ FLAGS_AT, 0, 1, 0x20 },          // VS FINISH (set: ON)
};

u16 crc16(const u8 *p, size_t n)
{
	u16 crc = 0;
	for (size_t i = 0; i < n; i++) {
		crc ^= u16(p[i] << 8);
		for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? u16((crc << 1) ^ 0x1021) : u16(crc << 1);
	}
	return crc;
}

bool assignments_valid(const std::vector<u8> &b)
{
	return settings_valid(b) && (b[ASSIGN_CRC] | b[ASSIGN_CRC + 1] << 8) == crc16(&b[ASSIGN], ASSIGN_END - ASSIGN);
}

void assignments_sign(std::vector<u8> &b)
{
	const u16 crc = crc16(&b[ASSIGN], ASSIGN_END - ASSIGN);
	b[ASSIGN_CRC] = u8(crc);
	b[ASSIGN_CRC + 1] = u8(crc >> 8);
}

int assignment_in(const std::vector<u8> &b, model2a::game_assignment a)
{
	if (!assignments_valid(b)) return -1;
	const assignment_at &s = ASSIGNMENTS[int(a)];
	const u8 v = b[s.at];
	if (s.bit) {
		const bool set = v & s.bit;
		// ADVERTISE SOUND and CONTINUE are on while their bit is clear
		return (a == model2a::game_assignment::advertise_sound || a == model2a::game_assignment::continues) ? !set : set;
	}
	return std::clamp(int(v), s.lo, s.hi);
}
}

int model2a::assignment(game_assignment a) const { return assignment_in(m_backup, a); }

bool model2a::set_vs_rounds_in(std::vector<u8> &backup, int rounds)
{
	const assignment_at &s = ASSIGNMENTS[int(game_assignment::match_vs)];
	rounds = std::clamp(rounds, s.lo, s.hi);
	if (backup.size() < ASSIGN_END || assignment_in(backup, game_assignment::match_vs) < 0 || backup[s.at] == rounds) return false;
	backup[s.at] = u8(rounds);
	assignments_sign(backup);
	return true;
}

bool model2a::set_assignment(game_assignment a, int v)
{
	const int now = assignment(a);
	if (now < 0) return false;
	const assignment_at &s = ASSIGNMENTS[int(a)];
	v = std::clamp(v, s.lo, s.hi);
	if (v == now) return false;
	if (s.bit) {
		const bool set = (a == game_assignment::advertise_sound || a == game_assignment::continues) ? !v : v;
		m_backup[s.at] = u8(set ? m_backup[s.at] | s.bit : m_backup[s.at] & ~s.bit);
	} else
		m_backup[s.at] = u8(v);
	if (a == game_assignment::difficulty) {   // what the test mode sets with it
		const difficulty_set &d = DIFFICULTY_SETS[v];
		m_backup[SETTINGS + 0x52] = u8(d.energy_1p); m_backup[SETTINGS + 0x53] = u8(d.energy_1p >> 8);
		m_backup[SETTINGS + 0x54] = u8(d.energy_vs); m_backup[SETTINGS + 0x55] = u8(d.energy_vs >> 8);
		m_backup[WIDTH_AT] = m_ring_width ? u8(m_ring_width / 100) : d.width;
	}
	assignments_sign(m_backup);
	return true;
}

bool model2a::set_ring_width(int width)
{
	m_ring_width = width >= 1300 && width <= 1600 ? width / 100 * 100 : 0;
	const int d = assignment(game_assignment::difficulty);
	if (d < 0) return false;
	const u8 w = m_ring_width ? u8(m_ring_width / 100) : DIFFICULTY_SETS[d].width;
	if (m_backup[WIDTH_AT] != w) {
		m_backup[WIDTH_AT] = w;
		assignments_sign(m_backup);
	}
	return true;
}

// Cheats > Fight switches, in work RAM (found with a scratch watcher like tools/vf2detect and the program's own code):
//   the fighters' records at [0x500804] and [0x500808] (0x510980 and 0x512980), each one's energy at +0x1ac (16-bit),
//   the round's full energy at 0x5000a0 (what the round starts them with, 0xbb20; the "perfect" checks it at 0xe95c),
//   and bit 5 of the record's first word set as a fighter goes down (it ends the round: 0xbc1c);
//   the round's clock at 0x500028, in 64ths of a second (1920 = 30 s, set as the fight starts, 0xbaa4), taken down by
//   1 each frame while the round's on (0xbc30, 0xc394: the time's up at 0), shown as it was before (25"59: 1638);
//   the round's stage at 0x500030 (9 while the round's on: the clock runs) and at 0x500031 the one a frame before;
//   the ring's half width at 0x50a00c (a float, in hundreds: 8.0 for 1600), set from 0x50a700 (versus) or 0x50a704
//   (against the computer) as a match starts (0xcf50) and from a replay's record as it plays (0xe128): the width the
//   ring outs are checked against.
//   Jacky's stage's bird: called when the word at 0x515d40 is 2 (the arcade's code sets it: both players holding Start,
//   Down and P+K+G; found by the work RAM with and without the code, and checked by writing it alone: the bird comes
//   down from the rock and flies round the fighters). The players' inputs as the game reads them at 0x500700 (the
//   starts, 0x10 1P and 0x20 2P), 0x500701 (1P: 0x10 down, 0x07 P+K+G) and 0x500702 (2P); the stage at 0x500064
//   (Jacky's, 7).
void model2a::fight_cheats(int infinite, bool one_hit, bool no_time, int ring, int bird)
{
	if (bird && game_mode() == 9 && m_workram.size() > 0x15d44 && m_workram[0x64] == 7 && m_workram[0x15d40] == 0) {
		const u8 st = m_workram[0x700], p1 = m_workram[0x701], p2 = m_workram[0x702];
		const bool one = ((st & 0x10) && (p1 & 0x17) == 0x17) || ((st & 0x20) && (p2 & 0x17) == 0x17);
		if (bird == 2 || one) m_workram[0x15d40] = 2;
	}
	if (game_mode() != 9 || (!infinite && !one_hit && !no_time && !ring && !m_ring_own_kept)) return;
	auto at = [&](u32 a, u32 n) -> u8 * { return a >= 0x500000 && a + n - 0x500000 <= m_workram.size() ? &m_workram[a - 0x500000] : nullptr; };
	auto rd16 = [&](u32 a) { const u8 *p = at(a, 2); return p ? int16_t(p[0] | p[1] << 8) : int16_t(0); };
	auto rd32 = [&](u32 a) { const u8 *p = at(a, 4); return p ? u32(p[0] | p[1] << 8 | p[2] << 16 | u32(p[3]) << 24) : 0u; };
	auto wr16 = [&](u32 a, int16_t v) { if (u8 *p = at(a, 2)) { p[0] = u8(v); p[1] = u8(u16(v) >> 8); } };
	const bool on = m_workram[0x30] == 9;   // (the round on, or starting this frame: its clock running)
	const u32 fighter[2] = { rd32(0x500804), rd32(0x500808) };
	const bool ok = at(fighter[0], 0x1b0) && at(fighter[1], 0x1b0);
	static int told = 0;   // (log.txt: what the cheats found, once each way)
	if (told < 2 && (ok ? told == 1 || told == 0 : told == 0)) {
		fprintf(stderr, "cheats: fighters at %06x %06x%s, energy %d %d of %d, clock %d, ring %.2f\n", fighter[0], fighter[1],
				ok ? "" : " (not set yet)", ok ? rd16(fighter[0] + 0x1ac) : 0, ok ? rd16(fighter[1] + 0x1ac) : 0, rd16(0x5000a0),
				rd16(0x500028), [&] { float f; memcpy(&f, at(0x50a00c, 4), 4); return double(f); }());
		told = ok ? 2 : 1;
	}
	// The ring: its half width each frame of a fight (a replay's too: its record holds the width it was played on). The
	// game sets it only as a match starts, so the width it set is kept (any not a cheat's), and Normal again puts it back
	static const float HALF[2] = { 3.25f, 25.0f };   // (Tiny 650: half the smallest ring, 1300; Huge 5000: far past the stage's floor)
	u8 *width = at(0x50a00c, 4);
	const bool cheat_width = !memcmp(width, &HALF[0], 4) || !memcmp(width, &HALF[1], 4);
	if (ring == 1 || ring == 2) {
		if (!cheat_width) { memcpy(&m_ring_own, width, 4); m_ring_own_kept = true; }
		memcpy(width, &HALF[ring - 1], 4);
	} else if (cheat_width && m_ring_own_kept)
		memcpy(width, &m_ring_own, 4);
	// No time limit: while the round's on, the clock taken down by the last frame is put back to its start before the
	// next (the HUD shows it as the frame starts, before it's taken down: 30"00). A round over (a knockout, a ring
	// out) leaves it where it is.
	if (no_time && on) {
		const int t = rd16(0x500028);
		if (t > 0 && t < 1920) wr16(0x500028, 1920);
	}
	if ((!infinite && !one_hit) || !ok || !on) return;
	const int16_t full = rd16(0x5000a0);
	if (full <= 0 || full > 1000) return;
	for (int p = 0; p < 2; p++) {
		const u32 a = fighter[p] + 0x1ac;
		int16_t e = rd16(a);
		if (infinite >> p & 1) e = full;
		else if (one_hit && e > 1) e = 1;
		else continue;
		wr16(a, e);
	}
}

// The notices are in the data ROM: the attract's FREE PLAY, a 17 x 2 tile image drawn by 0x8f1c
// from a record at 0x2a6b96a (base tile, 16-bit entries, rows, columns, then the entries, added to
// the base): base 0x0020, the blank the text layer is cleared with, and every entry 0 blank it.
// The fight's "FREEPLAY" (0x2b62c, 0x2bcc4: in free play, instead of the credits) is a string at
// 0x201e0bd printed by 0x8918: spaces.
int model2a::hide_free_play_text()
{
	auto &d = m_roms.regions[romset::MAIN_DATA];
	// (each checked with what's around it: the image's whole record; the string with the credit
	// texts before it, 0x201e09c-0x201e0c5)
	if (!m_free_play_text.empty()) return 2;   // (hidden already)
	auto patch = [&](u32 at, const u8 *to, size_t n, u32 check, size_t check_n, u32 crc) {
		if (check + check_n > d.size() || ::crc32(0, &d[check], uInt(check_n)) != crc) return 0;
		m_free_play_text.push_back({ at, std::vector<u8>(&d[at], &d[at] + n) });
		memcpy(&d[at], to, n);
		return 1;
	};
	u8 blank_image[12 + 2 * 17 * 2] = { 0x20, 0x00, 1, 0, 2, 0, 0, 0, 17, 0, 0, 0 };
	const u8 spaces[8] = { ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
	return patch(0xa6b96a, blank_image, sizeof blank_image, 0xa6b96a, sizeof blank_image, 0x7a1e2a07) +
		   patch(0x1e0bd, spaces, sizeof spaces, 0x1e09c, 0x1e0c6 - 0x1e09c, 0xebd6b7bb);
}

void model2a::show_free_play_text()
{
	auto &d = m_roms.regions[romset::MAIN_DATA];
	for (const auto &[at, bytes] : m_free_play_text) memcpy(&d[at], bytes.data(), bytes.size());
	m_free_play_text.clear();
}

bool model2a::patch_program(u32 addr, const u8 *to, size_t n, u32 check, size_t check_n, u32 crc)
{
	auto &rom = m_roms.regions[romset::MAINCPU];
	if (addr + n > rom.size() || check + check_n > rom.size() || ::crc32(0, &rom[check], uInt(check_n)) != crc)
		return false;
	memcpy(&rom[addr], to, n);
	if (m_rec) m_rec->code_changed(u32(addr), u32(n));
	return true;
}

static u32 rom_word(void *ctx, u32 a)
{
	auto &rom = *static_cast<const std::vector<u8> *>(ctx);
	u32 v = 0;
	if (a + 4 <= rom.size()) memcpy(&v, &rom[a], 4);
	return v;
}

bool model2a::use_generated()
{
	if (!i960rec::gen_nfns) return false;
	auto r = std::make_unique<i960rec::runtime>();
	r->cpu = &m_cpu;
	r->install(i960rec::gen_fns, i960rec::gen_nfns, i960rec::gen_insns, i960rec::gen_ninsns, rom_word, (void *)&m_roms.regions[romset::MAINCPU]);
	map_plain_pages(*r);
	m_rec = std::move(r);
	m_cpu.set_rec(m_rec.get());
	return true;
}

bool model2a::use_native(const std::string &map_path, i960rec::native_stats *stats)
{
	i960rec::hints map;
	if (map_path.empty()) map.parse(i960rec::vf2_map_text);
	else if (!map.read(map_path)) return false;
	auto r = std::make_unique<i960rec::runtime>();
	r->cpu = &m_cpu;
	auto &rom = m_roms.regions[romset::MAINCPU];
	map_plain_pages(*r);
	i960rec::native_stats st;
	if (!i960rec::build_native(*r, rom_word, (void *)&rom, u32(rom.size()), map, &st)) return false;
	m_native_stats = st;
	if (stats) *stats = st;
	m_rec = std::move(r);
	m_cpu.set_rec(m_rec.get());
	return true;
}

void model2a::use_interpreter()
{
	m_cpu.set_rec(nullptr);
	m_rec.reset();
}

// The boot, from power-on (frames as MAME counts them, the board's 57.52 a second):
//     0-23  the board's reset, then the game's colour fills; on a board without its settings
//           yet, "BACKUP RAM IS BROKEN. INITIALIZED." (the settings block written, frame 11)
//    24-84  "Sound Initialize ...": a fixed delay loop at 0x9f9c (700,000 divr) while the sound
//           board starts
//   85-725  the WARNING screen: drawn at 0xa834, which sets the countdown at 0x500024 to 640
//           (shlo 7,5,r15 at 0xa950); 0xa974 counts it down a frame at a time and moves on at 0
//   726-    the attract: a wipe to grey (726-727), the SEGA logo fading in (728-, past_boot())
// The game skips the WARNING altogether when backup RAM's byte 0x3350 is set or when it has
// restarted itself (it leaves a marker at 0x59cfe0 first, 0x6116c); neither on a power-on. Here
// only its countdown gets shorter: `mov 1,r15` puts 1 in it, so the boot moves on at the next
// frame, as it does at frame 724. Checked against the whole routine (0xa834-0xa9a3, both states).
static const u8 MOV_1_R15[4] = { 0x01, 0x1e, 0x78, 0x5c }, SHLO_7_5_R15[4] = { 0x07, 0x5e, 0x79, 0x59 };   // 5c781e01, 59795e07
bool model2a::shorten_warning()
{
	return patch_program(0xa950, MOV_1_R15, 4, 0xa834, 0xa9a4 - 0xa834, 0x4bbdbdf6);
}

// The WARNING's countdown as the game has it again (an online match shortens it on both sides, the boot unseen:
// after it, a player who sees the boot sees it whole)
void model2a::restore_warning()
{
	auto &rom = m_roms.regions[romset::MAINCPU];
	if (rom.size() < 0xa954 || memcmp(&rom[0xa950], MOV_1_R15, 4) != 0) return;
	memcpy(&rom[0xa950], SHLO_7_5_R15, 4);
	if (m_rec) m_rec->code_changed(0xa950, 4);
}

// The player select's fighters by slot (0x33e4c: Akira, Pai, Lau, Wolf, Jeffry, Kage, Sarah,
// Jacky, Shun, Lion, then Dural's hidden slot) are read for the fighter a slot sets (12 places
// in the select's code, each `ldib 0x33e4c(r4),r15` then `stib r15,0x1b0(g7)`: the fighter, which
// the select loads it by, and its portrait) and for the name and profile it shows (3, ldob). The
// first ones are pointed at a copy past the program's end (in the 2 MB ROM space, zeros there),
// Akira's slot the secret character; the name and profile stay Akira's. 0 puts it all back.
bool model2a::set_secret_character(int fighter)
{
	static constexpr u32 TABLE = 0x33e4c, COPY = 0x1fff00, N = 11;
	static constexpr u32 SETS[] = { 0x3213c, 0x322b4, 0x32508, 0x32680, 0x32800, 0x3297c, 0x32b30, 0x32cf4,
		0x32e6c, 0x32fec, 0x33168, 0x3331c };
	if (fighter != 0 && fighter != 9 && fighter != 11 && fighter != 12) return false;
	auto &rom = m_roms.regions[romset::MAINCPU];
	auto word = [&](u32 a) { u32 v; memcpy(&v, &rom[a], 4); return v; };
	if (rom.size() < COPY + N || ::crc32(0, &rom[TABLE], N) != 0xaa40f124) return false;
	for (u32 a : SETS)
		if (word(a) != 0xc0793400 || (word(a + 4) != TABLE && word(a + 4) != COPY) || word(a + 8) != 0xc27de1b0) return false;
	// (only bytes that change: Off on a program never patched leaves it as it is)
	u8 copy[N] = {};
	if (fighter) { memcpy(copy, &rom[TABLE], N); copy[0] = u8(fighter); }
	if (memcmp(&rom[COPY], copy, N)) {
		memcpy(&rom[COPY], copy, N);
		if (m_rec) m_rec->code_changed(COPY, N);
	}
	// (the code made again once for all of them, not once each: each is a whole native rebuild)
	const u32 to = fighter ? COPY : TABLE;
	u32 lo = ~0u, hi = 0;
	for (u32 a : SETS)
		if (word(a + 4) != to) {
			memcpy(&rom[a + 4], &to, 4);
			lo = std::min(lo, a); hi = std::max(hi, a + 8);
		}
	if (m_rec && hi) m_rec->code_changed(lo, hi - lo);
	return true;
}

// smooth motion's backdrop move: the screen's move for a unit of the backdrop's scroll (its x
// goes against the horizontal scroll, its y with the vertical; found by matching the backdrop
// drawn a frame on, moved back, to the frame before's)
constexpr double BACKDROP_SX = -1, BACKDROP_SY = 1;
constexpr double FC_SKY_SX = 1, FC_SKY_SY = -1;   // (the free camera's: the backdrop's way for a yaw to the right, a pitch up)

// a / b for the scheduler's times: the same quotient, in 64 bits when they fit (a 128-bit
// division is a library call)
static inline uint64_t div_atime(vf2::uint128_t a, vf2::uint128_t b)
{
	return (a >> 64) == 0 && (b >> 64) == 0 ? uint64_t(a) / uint64_t(b) : uint64_t(a / b);
}

uint64_t model2a::now() const
{
	const cpu_sched &c = m_sched[CPU_I960];
	return c.cycles + (m_executing == CPU_I960 ? uint64_t(m_cpu.cycles_used()) : 0);
}

uint64_t model2a::tgp_now() const
{
	const cpu_sched &c = m_sched[CPU_TGP];
	return c.cycles - m_tgp_cycle_base + (m_executing == CPU_TGP ? uint64_t(m_tgp->cpu.cycles_used()) : 0);
}

void model2a::reset()
{
	m_cooperative_ready=m_cooperative_pumping=m_cooperative_wait_input=false;
	render_wait();
	m_snd_queue.clear();
	m_snd_next = 0;
	m_cur_2d.reset();   // (what was kept for drawing pictures again belongs to the run before)
	m_snap_epoch++;     // (states saved before: the next save copies everything)
	m_video->smooth_reset();
	m_last_pic.reset();
	m_ram_log.clear();
	for (int k = 0; k < 4; k++) m_video->note_ram(k);   // (the GPU's copies of the RAMs: sent again)
	for (auto &in : m_scroll_in) in = scroll_input{};
	for (auto &sm : m_scroll_hist) sm = scroll_sample{};
	// model2_state::machine_reset and the devices' resets, at time 0
	for (auto &t : m_timer) t = timer{};
	rebuild_timer_queue();
	m_timer_seq = 0;
	m_head = -1;
	for (auto &q : m_line_queue) q.clear();
	for (auto &c : m_sched) { c.local = 0; c.cycles = 0; c.suspend = c.nextsuspend = 0; c.eat = c.nexteat = false; }
	for (int i = 0; i < N_CPUS; i++) m_order[i] = i;
	m_suspend_changes = false;
	if (!m_sound_on) suspend(CPU_68K, SUSPEND_HALT);   // the debugger's `suspend :audiocpu`, before it runs at all
	// Cost probe only: run the board's timers, UART link and scheduler while
	// holding the 68000, to separate its execution cost from the rest.
	if (m_sound_on && std::getenv("VF2_68K_HALT")) suspend(CPU_68K, SUSPEND_HALT);
	m_tgp_cycle_base = 0;
	m_base = 0;
	m_executing = -1;
	m_in_callback = false;
	m_frame_number = 0;
	m_intreq = m_intena = 0;
	m_coproctl = m_coprocnt = m_geoctl = m_geocnt = 0;
	m_ctrlmode = false;
	for (int i = 0; i < 4; i++) { m_timervals[i] = m_timerorig[i] = 0xfffff; m_timerrun[i] = false; }
	for (size_t i = 0; i < m_bufferram.size(); i += 4) { u32 v = 0x07800f0f; memcpy(&m_bufferram[i], &v, 4); }
	m_video->invalidate_geometry_pages();
	m_geo_write_start = 0;
	m_video->m_geo_read_start_address = 0;
	m_videocontrol = 0;
	m_tgp->bank_reg = 0;
	m_tgp->cpu.reset();
	m_fifo_in = gen_fifo{};
	m_fifo_out = gen_fifo{};
	std::fill(std::begin(m_io_port), std::end(m_io_port), 0xff);
	m_io_config = 0xff; m_io_mode = 0; m_io_analog = 0;
	m_eeprom.state = eeprom93c46::IN_RESET; m_eeprom.locked = true; m_eeprom.bits = 0;
	m_eeprom.cs = m_eeprom.clk = m_eeprom.di = 0; m_eeprom.completion = 0;
	m_uart = uart8251{};
	m_m2comm_cn = m_m2comm_fg = 0;
	sound_reset();
	m_cpu.reset();
	for (int l = 0; l < 4; l++) m_cpu.set_input_line(l, CLEAR_LINE);
	// the screen starts at the top of vblank (line 384): the first vblank is a frame later
	timer_adjust(T_VBLANK_START, atime(CYCLES_PER_FRAME) * m_sched[CPU_I960].period);
	// the UART's clock: low from the start, a toggle every 1 us (or with the byte link, T_UART only
	// when the transmitter has something to do: nothing yet)
	m_link_fast = m_byte_link;
	m_link_mode_edge = 0;
	m_link_t0 = 0;
	m_link_delivered = m_link_in_step = false;
	if (!m_link_fast && !m_sound_only) timer_adjust(T_UART, AS_PER_SEC / 1000000);   // (a sound board alone: its bytes' own, sound_bytes)
	irq_update();
	set_input_line(L_TGP_HALT, ASSERT_LINE);   // model2_tgp_state: the TGP waits for its program
}

void model2a::power_on()
{
	render_wait();
	std::fill(m_ram200.begin(), m_ram200.end(), 0);
	std::fill(m_workram.begin(), m_workram.end(), 0);
	m_ring_own_kept = false;   // (fight_cheats: the ring's width the game sets, found again from the next match)
	std::fill(m_bufferram.begin(), m_bufferram.end(), 0);
	std::fill(m_backup.begin(), m_backup.end(), 0xff);
	std::fill(m_cpuctl.begin(), m_cpuctl.end(), 0);
	std::fill(m_palram.begin(), m_palram.end(), 0);
	std::fill(m_colorxlat.begin(), m_colorxlat.end(), 0);
	std::fill(m_fbvram_a.begin(), m_fbvram_a.end(), 0);
	std::fill(m_fbvram_b.begin(), m_fbvram_b.end(), 0);
	std::fill(m_tex0.begin(), m_tex0.end(), 0);
	std::fill(m_tex1.begin(), m_tex1.end(), 0);
	std::fill(m_lumaram.begin(), m_lumaram.end(), 0);
	std::fill(m_m2comm_share.begin(), m_m2comm_share.end(), 0);
	std::fill(m_tiles.tile_ram.begin(), m_tiles.tile_ram.end(), 0);
	std::fill(m_tiles.char_ram.begin(), m_tiles.char_ram.end(), 0);
	m_tiles.char_version++;
	m_tiles.mark_dirty();
	m_pens.fill(0xff000000);
	m_palette_dirty = true;
	for (int k = 0; k < 4; k++) m_video->note_ram(k);
	m_eeprom = eeprom93c46{};
	std::fill(std::begin(m_eeprom.cells), std::end(m_eeprom.cells), 0xffff);
	m_cpu.power_on();
	m_tgp->cpu.power_on();
	std::fill(std::begin(m_tgp->prog), std::end(m_tgp->prog), 0);
	++m_tgp->program_revision_id;
	std::fill(std::begin(m_tgp->ram), std::end(m_tgp->ram), 0);
	m_tgp->bank_reg = m_tgp->sincos_base = m_tgp->inv_base = m_tgp->isqrt_base = 0;
	std::fill(std::begin(m_tgp->atan_base), std::end(m_tgp->atan_base), 0);
	m_video->power_on();
	m_coproctl = m_coprocnt = m_geoctl = m_geocnt = m_geo_write_start = 0;
	m_videocontrol = m_framenum = 0;
	m_render_test_mode = m_render_mode = m_render_unk = false;
	m_in0 = m_in1 = m_in2 = m_sw = 0xff;
	m_link_byte = 0;
	reset();
}

// ---------------------------------------------------------------------------------------------
// Save states. The state: a header (for this board's saves only), the RAMs at fixed places, then the
// rest (state()). A save into a buffer this board saved before, since its last reset, copies only the
// device memories' pages written since; the RAMs the i960's recompiled code writes directly, and
// buffer RAM (the TGP and the geometrizer's port write it too), always whole.
// ---------------------------------------------------------------------------------------------

namespace {

struct state_header { uint32_t magic, version, id, epoch, gen, zero; };
static_assert(sizeof(state_header) == model2a::STATE_HEADER);

const char *const PAGED_NAMES[] = { "tile ram", "char ram", "palette", "colour tables", "framebuffer a", "framebuffer b",
	"texture ram 0", "texture ram 1", "luma ram" };

template <class IO> void attos(IO &io, vf2::uint128_t &t)   // (the scheduler's times, as two 64-bit halves)
{
	uint64_t lo = uint64_t(t), hi = uint64_t(t >> 64);
	io.pod(lo);
	io.pod(hi);
	t = vf2::uint128_t(hi) << 64 | lo;
}

template <class IO, class E> void enum32(IO &io, E &e, int32_t last)   // (one of 0 to last: else the state's refused)
{
	int32_t v = int32_t(e);
	io.pod(v);
	if (io.check(v >= 0 && v <= last)) e = E(v);
}

} // namespace

u8 *model2a::paged_base(int r)
{
	switch (r) {
	case P_TILE: return reinterpret_cast<u8 *>(m_tiles.tile_ram.data());
	case P_CHAR: return reinterpret_cast<u8 *>(m_tiles.char_ram.data());
	case P_PAL: return reinterpret_cast<u8 *>(m_palram.data());
	case P_XLAT: return reinterpret_cast<u8 *>(m_colorxlat.data());
	case P_FB_A: return reinterpret_cast<u8 *>(m_fbvram_a.data());
	case P_FB_B: return reinterpret_cast<u8 *>(m_fbvram_b.data());
	case P_TEX0: return reinterpret_cast<u8 *>(m_tex0.data());
	case P_TEX1: return reinterpret_cast<u8 *>(m_tex1.data());
	default: return m_lumaram.data();
	}
}

size_t model2a::paged_bytes(int r) const
{
	switch (r) {
	case P_TILE: return m_tiles.tile_ram.size() * 2;
	case P_CHAR: return m_tiles.char_ram.size() * 2;
	case P_PAL: return m_palram.size() * 2;
	case P_XLAT: return m_colorxlat.size() * 2;
	case P_FB_A: return m_fbvram_a.size() * 2;
	case P_FB_B: return m_fbvram_b.size() * 2;
	case P_TEX0: return m_tex0.size() * 4;
	case P_TEX1: return m_tex1.size() * 4;
	default: return m_lumaram.size();
	}
}

void model2a::setup_pages()
{
	for (int r = 0; r < PAGED; r++) m_page_gen[r].assign((paged_bytes(r) + PAGE - 1) / PAGE, 0);
	std::random_device rd;
	m_snap_id = rd() ^ uint32_t(uintptr_t(this));
	m_snap_epoch++;
}

size_t model2a::fixed_state_size() const
{
	size_t n = STATE_HEADER + m_ram200.size() + m_workram.size() + m_bufferram.size() + m_cpuctl.size() +
		m_m2comm_share.size() + m_backup.size();
	for (int r = 0; r < PAGED; r++) n += paged_bytes(r);
	return n;
}

// The state's small parts, after the memories
template <class IO> void model2a::state(IO &io)
{
	io.section("i960");
	m_cpu.state(io);
	io.section("tgp");
	m_tgp->cpu.state(io);
	io.pod(m_tgp->prog);
    io.bytes(m_tgp->ram,0x100*sizeof(u32));
    io.bytes(m_tgp->ram+0x200,0x200*sizeof(u32));
	io.pod(m_tgp->bank_reg); io.pod(m_tgp->sincos_base); io.pod(m_tgp->inv_base); io.pod(m_tgp->isqrt_base); io.pod(m_tgp->atan_base);
	io.section("scheduler");
	for (timer &t : m_timer) {
		attos(io, t.expire); attos(io, t.start); attos(io, t.period);
		io.pod(t.seq); io.pod(t.enabled); io.pod(t.param);
	}
	io.pod(m_timer_seq);
	for (cpu_sched &c : m_sched) {   // (period, divshift, divisor: the clocks, set once)
		attos(io, c.local);
		io.pod(c.cycles); io.pod(c.suspend); io.pod(c.nextsuspend); io.pod(c.eat); io.pod(c.nexteat);
	}
	io.pod(m_order); io.pod(m_suspend_changes);
	attos(io, m_base);
	for (std::vector<int> &q : m_line_queue) io.vec(q);
	io.pod(m_frame_number); io.pod(m_tgp_cycle_base);
	io.section("fifos");
	for (gen_fifo *f : { &m_fifo_in, &m_fifo_out }) {
		f->values.state(io); io.deq(f->extra);
		io.pod(f->empty_triggered); io.pod(f->full_triggered);
	}
	io.section("board");
	io.pod(m_intreq); io.pod(m_intena); io.pod(m_timervals); io.pod(m_timerorig); io.pod(m_timerrun);
	io.pod(m_coproctl); io.pod(m_coprocnt); io.pod(m_geoctl); io.pod(m_geocnt); io.pod(m_geo_write_start);
	io.pod(m_videocontrol); io.pod(m_framenum);
	io.pod(m_render_test_mode); io.pod(m_render_mode); io.pod(m_render_unk);
	{
		int16_t xoffset, yoffset;
		u8 zclip;
		m_video->get_regs(xoffset, yoffset, zclip);
		io.pod(m_video->m_geo_read_start_address); io.pod(xoffset); io.pod(yoffset); io.pod(zclip);
		if constexpr (IO::loading) m_video->set_regs(xoffset, yoffset, zclip);
	}
	io.pod(m_in0); io.pod(m_in1); io.pod(m_in2); io.pod(m_sw);
	io.pod(m_io_port); io.pod(m_io_config); io.pod(m_io_mode); io.pod(m_io_analog); io.pod(m_ctrlmode);
	io.section("eeprom");
	eeprom93c46 &e = m_eeprom;
	io.pod(e.cells); io.pod(e.cs); io.pod(e.clk); io.pod(e.di);
	enum32(io, e.state, eeprom93c46::WAIT_FOR_COMPLETION);
	io.pod(e.locked); io.pod(e.bits); io.pod(e.cmd_addr); io.pod(e.shift); io.pod(e.address);
	enum32(io, e.command, eeprom93c46::ERASEALL);
	io.pod(e.completion); io.pod(e.cs_rise);
	io.section("uart");
	uart8251 &u = m_uart;
	enum32(io, u.flags, uart8251::NEXT_COMMAND);
	io.pod(u.status); io.pod(u.command); io.pod(u.mode); io.pod(u.tx_data); io.pod(u.br_factor); io.pod(u.delayed_tx_en);
	io.pod(u.tx_bits_left); io.pod(u.tx_shift); io.pod(u.txc_count); io.pod(u.txc);
	io.pod(m_link_fast); io.pod(m_link_mode_edge);
	attos(io, m_link_t0);
	io.pod(m_link_delivered); io.pod(m_link_in_step); io.pod(m_link_byte);
	io.pod(m_m2comm_cn); io.pod(m_m2comm_fg);
	if (random_fighters_on()) {   // (online: on on both sides or neither, from the host's blob)
		io.section("random fighters");
		random_state(io);
	}
	io.section("end");
	if constexpr (IO::loading) io.check(state_sane());
}

// A loaded state's values that are used as an index, a count, a divisor, a shift or a time to run to: as the board
// makes them (a state from another process, a watcher's snapshot from a match's host, may hold anything). The board's
// own states always are; the checks are a few dozen comparisons a load
bool model2a::state_sane() const
{
	bool seen[N_CPUS] = {};
	for (const int c : m_order) {
		if (c < 0 || c >= N_CPUS || seen[c]) return false;
		seen[c] = true;
	}
	for (const std::vector<int> &q : m_line_queue) {
		if (q.size() > 64) return false;
		for (const int st : q) if (st != CLEAR_LINE && st != ASSERT_LINE) return false;
	}
	for (const gen_fifo *f : { &m_fifo_in, &m_fifo_out })
		if (f->values.size() > 8 || f->extra.size() > 1024 || (!f->extra.empty() && f->values.size() < 8)) return false;
	// the times: the scheduler runs each CPU up to the first timer, so none far from now (a frame is 17 ms; the
	// longest a timer waits, the SCSP's, under a second), and the frame's own timer always set
	const atime most = atime(8) * AS_PER_SEC;
	if (m_base > (atime(1) << 100)) return false;
	for (const timer &t : m_timer)   // (a period a one-shot's 0, else the SCSP's or the sound update's: never under a microsecond,
		// or the timer fires for ever without the CPUs running; found by fuzzing)
		if (t.enabled && (t.expire > m_base + most || t.period > most || (t.period && t.period < AS_PER_SEC / 1000000))) return false;
	if (!m_timer[T_VBLANK_START].enabled) return false;
	for (const cpu_sched &c : m_sched)   // (and each CPU's time its whole cycles, as it only ever moves: else a slice short of
		// a cycle can't be run and the scheduler goes round for ever; found by fuzzing)
		if (c.local > m_base + most || c.local + most < m_base || c.local != atime(c.cycles) * c.period) return false;
	const eeprom93c46 &e = m_eeprom;
	if (e.address >= std::size(e.cells) || (e.cs | e.clk | e.di) & ~1 || e.bits < 0 || e.bits > 0x10000) return false;
	const uart8251 &u = m_uart;
	if ((u.br_factor != 1 && u.br_factor != 16 && u.br_factor != 64) || u.txc_count < 0 || u.txc_count >= u.br_factor ||
	    u.tx_bits_left < 0 || u.tx_bits_left > 10 || (u.txc & ~1))
		return false;
	return true;
}

void model2a::save_state(std::vector<uint8_t> &out, std::vector<m2::StateSection> *sections)
{
	advance_aux_clocks(m_base);
	render_wait();   // (a picture being drawn alongside may have taken the 3D's RAMs back for a moment)
	if (sections) sections->clear();
	const size_t fixed = fixed_state_size();
	state_header h{};
	if (out.size() >= fixed) memcpy(&h, out.data(), sizeof h);
	const bool since = out.size() >= fixed && h.magic == STATE_MAGIC && h.version == STATE_VERSION && h.id == m_snap_id && h.epoch == m_snap_epoch;
	const uint32_t prev = since ? h.gen : 0;
	out.resize(fixed);   // (no allocation once the buffer has had a state)
	h = { STATE_MAGIC, STATE_VERSION, m_snap_id, m_snap_epoch, m_snap_gen, 0 };
	memcpy(out.data(), &h, sizeof h);
	size_t at = STATE_HEADER;
	auto whole = [&](const char *name, const void *src, size_t n) {
		if (sections) sections->push_back({ name, at });
		memcpy(out.data() + at, src, n);
		at += n;
	};
	whole("ram 200000", m_ram200.data(), m_ram200.size());
	whole("work ram", m_workram.data(), m_workram.size());
	whole("buffer ram", m_bufferram.data(), m_bufferram.size());
	whole("cpu control", m_cpuctl.data(), m_cpuctl.size());
	whole("m2comm", m_m2comm_share.data(), m_m2comm_share.size());
	whole("backup ram", m_backup.data(), m_backup.size());
	for (int r = 0; r < PAGED; r++) {
		const u8 *base = paged_base(r);
		const size_t bytes = paged_bytes(r);
		const std::vector<uint32_t> &gen = m_page_gen[r];
		if (sections) sections->push_back({ PAGED_NAMES[r], at });
		if (!since) memcpy(out.data() + at, base, bytes);
		else
			for (size_t q = 0; q < gen.size(); q++)
				if (gen[q] > prev) memcpy(out.data() + at + q * PAGE, base + q * PAGE, std::min(PAGE, bytes - q * PAGE));
		at += bytes;
	}
	m2::StateWriter w(out, sections, true);
	state(w);
	// (the geometrizer's and rasterizer's state, the picture's alone, kept by the board under this save, for a load of it:
	// model2_video::keep_geo. Only where there are pictures)
	if (m_draw_3d || m_video->smooth()) m_video->keep_geo(uint64_t(m_snap_epoch) << 32 | m_snap_gen);
	m_snap_gen++;   // (writes from now on are after this save)
}

bool model2a::load_state(const uint8_t *p, size_t n)
{
	const size_t fixed = fixed_state_size();
	state_header h{};
	if (n < fixed) return false;
	memcpy(&h, p, sizeof h);
	if (h.magic != STATE_MAGIC || h.version != STATE_VERSION) return false;
	render_wait();
	keep_2d();   // (a picture of the frame now still held keeps the 2D it was drawn from)
	// a state this board saved since its last reset: only the pages written since it was saved differ
	const bool since = h.id == m_snap_id && h.epoch == m_snap_epoch;
	size_t at = STATE_HEADER;
	auto whole = [&](void *dst, size_t m) { memcpy(dst, p + at, m); at += m; };
	whole(m_ram200.data(), m_ram200.size());
	whole(m_workram.data(), m_workram.size());
	whole(m_bufferram.data(), m_bufferram.size());
	m_video->invalidate_geometry_pages();
	whole(m_cpuctl.data(), m_cpuctl.size());
	whole(m_m2comm_share.data(), m_m2comm_share.size());
	whole(m_backup.data(), m_backup.size());
	for (int r = 0; r < PAGED; r++) {
		u8 *base = paged_base(r);
		const size_t bytes = paged_bytes(r);
		std::vector<uint32_t> &gen = m_page_gen[r];
		bool changed = false;
		for (size_t q = 0; q < gen.size(); q++) {
			const size_t len = std::min(PAGE, bytes - q * PAGE);
			u8 *dst = base + q * PAGE;
			const uint8_t *src = p + at + q * PAGE;
			if (since ? gen[q] <= h.gen : !memcmp(dst, src, len)) continue;
			memcpy(dst, src, len);
			gen[q] = m_snap_gen;   // (changed now: the next save into any buffer copies it)
			changed = true;
			if (r == P_TILE || r == P_CHAR) m_tiles.restored(r == P_TILE, offs_t(q * PAGE / 2), len / 2);
		}
		at += bytes;
		if (!changed) continue;
		// what follows from them: the pens, the GPU's copies of the 3D's RAMs
		if (r == P_PAL || r == P_XLAT) m_palette_dirty = true;
		if (r == P_XLAT) note_gpu_ram(RAM_XLAT);
		if (r == P_TEX0) note_gpu_ram(RAM_TEX0);
		if (r == P_TEX1) note_gpu_ram(RAM_TEX1);
		if (r == P_LUMA) note_gpu_ram(RAM_LUMA);
	}
	m2::StateReader rd(p + fixed, n - fixed);
	state(rd);
	// The board's held/running state is part of the scheduler snapshot, and every
	// checkpoint was saved with the board held (set_sound(false)), so it carries
	// SUSPEND_HALT for the 68000. Loading one with sound on must start the 68000:
	// otherwise the sound board stays silent and it costs nothing to "run".
	if (m_sound_on) {
		auto &s68 = m_sched[CPU_68K];
		s68.suspend &= ~SUSPEND_HALT;
		s68.nextsuspend &= ~SUSPEND_HALT;
	}
	m_cooperative_ready=false;
	++m_tgp->program_revision_id;
	m_head = -1;
	rebuild_timer_queue();
	// the geometrizer and rasterizer as they were at the save (kept by this board), not as the frames since left them
	if (since) m_video->restore_geo(uint64_t(h.epoch) << 32 | h.gen);
	// smooth motion's record of the 3D's RAMs changing: the frames from the one restored on are to run again
	if (!m_ram_log.empty()) {
		auto it = m_ram_log.begin();
		while (it != m_ram_log.end() && it->frame < m_frame_number) ++it;
		m_ram_log.erase(it, m_ram_log.end());
	}
	// smooth motion's backdrop: the camera's heading and pitch the next frame scrolls by, from the RAM restored (they
	// were the last frame run's: after a rollback, a frame of the timeline taken back's, the backdrop's place for the
	// first frame run again wrong), and the frames from the one restored on recorded again
	m_next_heading = u16(m_workram[0x3012] | m_workram[0x3013] << 8);
	m_next_pitch = int16_t(m_workram[0x3010] | m_workram[0x3011] << 8);
	for (auto &in : m_scroll_in) if (in.frame >= int64_t(m_frame_number)) in = scroll_input{};
	for (auto &sm : m_scroll_hist) if (sm.frame >= int64_t(m_frame_number)) sm = scroll_sample{};
	// The board's clock comes back with the state but its sample history does not:
	// start the stream at the restored time, or the first update would make every
	// sample since power-on in one block (1.8M samples on the fight checkpoint).
	if (m_scsp) m_scsp->start_stream_now();
	return rd.done();
}

// ---------------------------------------------------------------------------------------------
// MAME's scheduler: timeslices up to the next timer; in each, the CPUs run in turn (the running
// ones first, the halted ones "eating" their cycles), and a CPU that stops early (a timer it
// armed for now, or a synchronised input line) brings the slice's end down for the ones after
// it. Then the timers due fire, in order.
// ---------------------------------------------------------------------------------------------

static const bool g_sched_log = getenv("VF2_SCHED_LOG") != nullptr;
static const bool g_skip_inert_handoffs=std::getenv("VF2_SKIP_INERT_HANDOFFS")!=nullptr;
#if defined(VF2_SCHED_DIAGNOSTICS)
#define SCHED_COUNT(...) do { __VA_ARGS__; } while(false)
#else
#define SCHED_COUNT(...) do {} while(false)
#endif
static const bool g_reference_division = getenv("VF2_REFERENCE_DIVISION") != nullptr;
static const bool g_frame_budget_division=getenv("VF2_FRAME_BUDGET_DIVISION")!=nullptr;
static const bool g_i960_timer_reciprocal = getenv("VF2_I960_TIMER_RECIPROCAL") != nullptr;
static const bool g_sync_timer_fast = getenv("VF2_SYNC_TIMER_FAST") != nullptr;
static const bool g_fast_halt_delivery = getenv("VF2_FAST_HALT_DELIVERY") != nullptr;
static const bool g_fuse_fifo_halt = getenv("VF2_FUSE_FIFO_HALT") != nullptr;
static const bool g_fixed_fifo_store = getenv("VF2_I960_FIXED_FIFO_STORE") != nullptr;
static const bool g_fixed_fifo_load = getenv("VF2_I960_FIXED_FIFO_LOAD") != nullptr;
static const u16 g_fifo_port_flags=(g_fixed_fifo_store?VF2_FIXED_STORE_PORT:0)|
                                  (g_fixed_fifo_load?VF2_FIXED_LOAD_PORT:0);
static const bool g_reference_timers = getenv("VF2_REFERENCE_TIMERS") != nullptr;
static const bool g_modular_clock = getenv("VF2_MODULAR_CLOCK") != nullptr;
static const bool g_cache_2d_pens = getenv("VF2_CACHE_2D_PENS") != nullptr;
static const bool g_scheduler_gaps = getenv("VF2_SCHEDULER_GAPS") != nullptr;
static const bool g_reference_duration = getenv("VF2_REFERENCE_DURATION") != nullptr;
static const bool g_lazy_aux_clocks = getenv("VF2_LAZY_AUX_CLOCKS") != nullptr;
static const bool g_cooperative_fifo = getenv("VF2_COOPERATIVE_FIFO") != nullptr;
static const bool g_cooperative_timed = getenv("VF2_COOPERATIVE_TIMED") != nullptr;
static const unsigned g_cooperative_batch = unsigned(std::clamp(getenv("VF2_COOPERATIVE_BATCH")?atoi(getenv("VF2_COOPERATIVE_BATCH")):1,1,8));
static bool sched_log_on(double ns)
{
	static const double lo = getenv("VF2_SCHED_LOG") ? atof(getenv("VF2_SCHED_LOG")) : -1;
	static const double hi = getenv("VF2_SCHED_LOG_END") ? atof(getenv("VF2_SCHED_LOG_END")) : 1e300;
	return lo >= 0 && ns >= lo && ns <= hi;
}
// Keep optional formatting and debugger-hook construction out of the hot
// scheduler's stack frame and register allocation, even when tracing is off.
__attribute__((noinline,cold)) static void sched_log_adjust(const vf2::uint128_t &start,int id,const vf2::uint128_t &expire,int cpu){
    const double ns=double(start)/1e9;
    if(sched_log_on(ns))std::fprintf(stderr,"[%.3f ns] adjust timer %d to %.3f (cpu %d)\n",ns,id,double(expire)/1e9,cpu);
}
__attribute__((noinline,cold)) static void sched_log_cpu(const vf2::uint128_t &local,int id,bool suspended,int ran,int cycles,const vf2::uint128_t &target){
    const double ns=double(local)/1e9;
    if(sched_log_on(ns))std::fprintf(stderr,"[%.3f ns] cpu %d %s %d of %d cycles to %.3f (target %.3f)\n",ns,id,suspended?"ate":"ran",ran,cycles,ns,double(target)/1e9);
}
__attribute__((noinline,cold)) static void sched_log_fire(const vf2::uint128_t &expire,int id){
    const double ns=double(expire)/1e9;
    if(sched_log_on(ns))std::fprintf(stderr,"[%.3f ns] fire timer %d\n",ns,id);
}

model2a::atime model2a::time() const
{
	if (m_in_callback) return m_callback_time;
	if (m_executing == CPU_I960) {const auto &c=m_sched[CPU_I960];return c.local+(g_reference_duration?atime(m_cpu.cycles_used())*c.period:c.elapsed(m_cpu.cycles_used()));}
	if (m_executing == CPU_TGP) {const auto &c=m_sched[CPU_TGP];return c.local+(g_reference_duration?atime(m_tgp->cpu.cycles_used())*c.period:c.elapsed(m_tgp->cpu.cycles_used()));}
	if (m_executing == CPU_68K) {const auto &c=m_sched[CPU_68K];return c.local+(g_reference_duration?atime(m_68k->cycles_used())*c.period:c.elapsed(m_68k->cycles_used()));}
	return m_base;
}

void model2a::rebuild_timer_queue(){
    static_assert(N_TIMERS<128);
    m_timer_first=-1;
    std::fill_n(m_timer_prev,N_TIMERS,int8_t(-1));std::fill_n(m_timer_next,N_TIMERS,int8_t(-1));
    if(!g_reference_timers)for(int i=0;i<N_TIMERS;i++)if(m_timer[i].enabled)timer_enqueue(i);
}
void model2a::timer_unlink(int id){
    int before=m_timer_prev[id],after=m_timer_next[id];
    if(before<0)m_timer_first=after;else m_timer_next[before]=after;
    if(after>=0)m_timer_prev[after]=before;
    m_timer_prev[id]=m_timer_next[id]=-1;m_head=m_timer_first;
}
void model2a::timer_enqueue(int id){
    const timer &t=m_timer[id];int before=-1,after=m_timer_first;
    while(after>=0){
        const timer &other=m_timer[after];
        if(t.expire<other.expire||(t.expire==other.expire&&t.seq<other.seq))break;
        before=after;after=m_timer_next[after];
    }
    m_timer_prev[id]=before;m_timer_next[id]=after;
    if(before<0)m_timer_first=id;else m_timer_next[before]=id;
    if(after>=0)m_timer_prev[after]=id;
    m_head=m_timer_first;
}
int model2a::timer_head() const
{
    if(m_head>=0)return m_head;
    if(!g_reference_timers){m_head=m_timer_first;return m_head;}
    SCHED_COUNT(++m_scheduler_diagnostics.head_scans);
    int head=-1;
    for(int i=0;i<N_TIMERS;i++){
        const timer &t=m_timer[i];if(!t.enabled)continue;
        if(head<0||t.expire<m_timer[head].expire||(t.expire==m_timer[head].expire&&t.seq<m_timer[head].seq))head=i;
    }
    m_head=head;return head;
}

void model2a::timer_disable(int id)
{
	if(!g_reference_timers&&m_timer[id].enabled)timer_unlink(id);
	m_timer[id].enabled = false;
	if (m_head == id) m_head = -1;
	if (id == m_callback_timer) m_callback_modified = true;
}

void model2a::abort_timeslice()
{
	if (m_executing == CPU_I960) m_cpu.abort_timeslice();
	else if (m_executing == CPU_TGP) m_tgp->cpu.abort_timeslice();
	else if (m_executing == CPU_68K) m_68k->abort_timeslice();
}

// emu_timer::adjust: a timer that becomes the first to fire ends the running CPU's slice
void model2a::timer_adjust(int id, atime delay, u32 param, atime period)
{
	SCHED_COUNT(++m_scheduler_diagnostics.adjusted[id]);
	timer &t = m_timer[id];
	if(!g_reference_timers&&t.enabled)timer_unlink(id);
	if (id == m_callback_timer) m_callback_modified = true;
	t.param = param;
	t.period = period;
	t.enabled = true;
	t.start = time();
	t.expire = t.start + delay;
	t.seq = ++m_timer_seq;
	// keep the cached head: this one is the new head if it comes first, else unknown if it was
	if(!g_reference_timers)timer_enqueue(id);
	else if (m_head == id) m_head = -1;
	else if (m_head >= 0 && t.expire < m_timer[m_head].expire) m_head = id;
	if(g_sched_log)sched_log_adjust(t.start,id,t.expire,m_executing);
	if (timer_head() == id) abort_timeslice();
}

// FIFO and input-line synchronization schedule zero-delay one-shots. The
// common strictly-earlier insertion needs neither a queue walk nor a
// second head lookup. Equal timestamps retain the sequence-number order.
void model2a::timer_sync(int id)
{
    SCHED_COUNT(++m_scheduler_diagnostics.sync_calls);
    if(!g_sync_timer_fast||g_sched_log||g_reference_timers){
        timer_adjust(id,0);
        SCHED_COUNT(if(m_executing>=0&&timer_head()==id)++m_scheduler_diagnostics.sync_abort[m_executing]);
        return;
    }

    SCHED_COUNT(++m_scheduler_diagnostics.adjusted[id]);
    timer &t=m_timer[id];
    if(t.enabled)timer_unlink(id);
    if(id==m_callback_timer)m_callback_modified=true;
    t.enabled=true;t.period=0;t.param=0;
    t.start=t.expire=time();
    t.seq=++m_timer_seq;

    const int head=m_timer_first;
    if(head<0||t.expire<m_timer[head].expire){
        m_timer_prev[id]=-1;m_timer_next[id]=head;
        if(head>=0)m_timer_prev[head]=id;
        m_head=m_timer_first=id;
        SCHED_COUNT(if(m_executing>=0)++m_scheduler_diagnostics.sync_abort[m_executing]);
        abort_timeslice();
        return;
    }

    timer_enqueue(id);
    // An equal time can still sort first after loading a wrapped sequence.
    if(m_timer_first==id){
        SCHED_COUNT(if(m_executing>=0)++m_scheduler_diagnostics.sync_abort[m_executing]);
        abort_timeslice();
    }
}

// device_input::set_state_synced: queued, applied when the synchronize() timer fires
void model2a::set_input_line(int line, int state)
{
	m_line_queue[line].push_back(state);
	if (m_line_queue[line].size() == 1) timer_sync(int(T_LINE0) + line);
}

void model2a::apply_line(int line)
{
    auto &queue=m_line_queue[line];
    SCHED_COUNT(if(queue.size()==1)++m_scheduler_diagnostics.single_line;
                else if(!queue.empty())++m_scheduler_diagnostics.batched_line);
    if(g_sync_timer_fast&&queue.size()==1){
        const int state=queue.front();
        queue.clear();
        apply_line_state(line,state);
        return;
    }

    std::vector<int> pending;
    pending.swap(queue);
    for(int state:pending)apply_line_state(line,state);
    // A callback may have queued another event while applying this batch.
    if(queue.empty()){
        pending.clear();
        pending.swap(queue);
    }
}

void model2a::apply_line_state(int line,int state)
{
	switch (line) {
	case L_IRQ0: case L_IRQ1: case L_IRQ2: case L_IRQ3:
		m_cpu.set_input_line(line - L_IRQ0, state);
		break;
	case L_I960_HALT:
		if (state == ASSERT_LINE) suspend(CPU_I960, SUSPEND_HALT); else resume(CPU_I960, SUSPEND_HALT);
		break;
	case L_68K_IRQ0 + 0: case L_68K_IRQ0 + 1: case L_68K_IRQ0 + 2: case L_68K_IRQ0 + 3:
	case L_68K_IRQ0 + 4: case L_68K_IRQ0 + 5: case L_68K_IRQ0 + 6: case L_68K_IRQ0 + 7: {
		static const bool log = getenv("VF2_SND_LOG") != nullptr;
		if (log) fprintf(stderr, "68k line %d %d at %llu (pc %06x, frame %llu)\n", line - L_68K_IRQ0, state, (unsigned long long)(time() / m_sched[CPU_68K].period), m_68k->pc(), (unsigned long long)m_frame_number);
		m_68k->set_input_line(line - L_68K_IRQ0, state);
		break;
	}
	case L_TGP_HALT:
		if (state == ASSERT_LINE) suspend(CPU_TGP, SUSPEND_HALT); else resume(CPU_TGP, SUSPEND_HALT);
		break;
	case L_TGP_RESET:
		if (state == ASSERT_LINE) suspend(CPU_TGP, SUSPEND_RESET);
		else if (m_sched[CPU_TGP].nextsuspend & SUSPEND_RESET) {
			// device_t::reset(): the cycle count restarts, every suspension is lifted, the
			// queued input line events are dropped
			m_tgp_cycle_base = m_sched[CPU_TGP].cycles;
			resume(CPU_TGP, ~0u);
			m_tgp->cpu.reset();
			m_line_queue[L_TGP_HALT].clear();
			timer_disable(int(T_LINE0) + L_TGP_HALT);
			resume(CPU_TGP, SUSPEND_RESET);
		}
		break;
	}
}

void model2a::suspend(int cpu, u32 reason)
{
	m_sched[cpu].nextsuspend |= reason;
	m_sched[cpu].nexteat = true;
	m_suspend_changes = true;
	if (m_executing == cpu) abort_timeslice();
}

void model2a::resume(int cpu, u32 reason)
{
	m_sched[cpu].nextsuspend &= ~reason;
	m_suspend_changes = true;
	if (m_executing == cpu) abort_timeslice();
}

void model2a::apply_suspend_changes()
{
	if (g_fast_halt_delivery && !g_reference_timers && !g_sched_log) {
		static constexpr auto orders = [] {
			std::array<std::array<int, N_CPUS>, 1u << N_CPUS> table{};
			for (unsigned mask = 0; mask < table.size(); ++mask) {
				int position = 0;
				for (unsigned suspended : { 0u, 1u })
					for (int cpu = 0; cpu < N_CPUS; ++cpu)
						if (((mask >> cpu) & 1u) == suspended)
							table[mask][position++] = cpu;
			}
			return table;
		}();
		unsigned mask = 0;
		u32 changed = 0;
		for (int cpu = 0; cpu < N_CPUS; ++cpu) {
			cpu_sched &c = m_sched[cpu];
			changed |= c.suspend ^ c.nextsuspend;
			c.suspend = c.nextsuspend;
			c.eat = c.nexteat;
			mask |= unsigned(c.suspend != 0) << cpu;
		}
		m_suspend_changes = false;
		if (changed)
			std::copy(orders[mask].begin(), orders[mask].end(), m_order);
		return;
	}

	bool changed = false;
	for (auto &c : m_sched) {
		changed |= (c.suspend ^ c.nextsuspend) != 0;
		c.suspend = c.nextsuspend;
		c.eat = c.nexteat;
	}
	m_suspend_changes = false;
	if (changed) {   // rebuild_execute_list: the running CPUs first, then the suspended ones
		int n = 0;
		for (int i = 0; i < N_CPUS; i++) if (!m_sched[i].suspend) m_order[n++] = i;
		for (int i = 0; i < N_CPUS; i++) if (m_sched[i].suspend) m_order[n++] = i;
	}
}

__attribute__((noinline,cold)) void model2a::sync_instruction_hook(int id)
{
	if(id==CPU_I960)m_cpu.on_instruction=on_instruction?std::function<void(offs_t)>([this](offs_t pc){if(on_instruction)on_instruction(pc);}):nullptr;
	else if(id==CPU_68K)m_68k->on_instruction=on_68k_instruction?std::function<void(offs_t)>([this](offs_t pc){if(on_68k_instruction)on_68k_instruction(pc&0xffffff);}):nullptr;
}

void model2a::timeslice()
{
    if(m_modular_frame){
        const bool frame_division=g_frame_budget_division&&!g_reference_division;
        // With the sound board live the 68000 must run in the same pass, so the
        // modular clock keeps its uint64 arithmetic but loses LazyAux and the
        // inert-handoff proof (which assumes only three non-deferred clocks).
        if (m_sound_on) {
            if (frame_division) timeslice_impl<false,true,true,true>();
            else timeslice_impl<false,true,true>();
        } else if (g_skip_inert_handoffs) {
            if (frame_division) timeslice_impl<true,true,true,true,true>();
            else timeslice_impl<true,true,true,false,true>();
        } else if (frame_division) timeslice_impl<true,true,true,true>();
        else timeslice_impl<true,true,true>();
        return;
    }
	if(g_cooperative_fifo&&!m_cooperative_ready)prepare_cooperative_fifo();
	// Cost probe only: take the lazy-aux path even with the board on, so a held
	// 68000 is not visited for every synchronization. The board is independent
	// of the i960, so the game still runs.
	static const bool force_lazy_aux=std::getenv("VF2_FORCE_LAZY_AUX")!=nullptr;
	if((g_lazy_aux_clocks&&!m_sound_on&&!m_sound_only)||(force_lazy_aux&&!m_sound_only)){
        if(g_scheduler_gaps)timeslice_impl<true,true,false>();else timeslice_impl<true,false,false>();
    }else{
        if(g_scheduler_gaps)timeslice_impl<false,true,false>();else timeslice_impl<false,false,false>();
    }
}

void model2a::advance_aux_clocks(const atime &target)
{
	if(!g_lazy_aux_clocks||m_sound_on||m_sound_only)return;
	cpu_sched &c=m_sched[CPU_68K];
	if(!(c.suspend&SUSPEND_HALT)||!c.eat||target<c.local)return;
	SCHED_COUNT(++m_scheduler_diagnostics.visits[CPU_68K]);
	const atime delta=target-c.local;if(delta<c.period)return;
	// A halted processor has no side effects. Materialize its clock at observable
	// boundaries instead of visiting it for every FIFO/timer synchronization.
	// The full period is necessary for these larger gaps: the scheduler's shifted
	// divisor approximation can round an extra cycle near a period boundary.
	const uint64_t cycles=(delta>>64)?uint64_t(delta/c.period):uint64_t(delta)/uint64_t(c.period);
	SCHED_COUNT(++m_scheduler_diagnostics.dispatches[CPU_68K];m_scheduler_diagnostics.budget[CPU_68K]+=cycles;m_scheduler_diagnostics.advanced[CPU_68K]+=cycles);
	c.local+=atime(cycles)*c.period;c.cycles+=cycles;
}

template<bool LazyAux,bool NarrowGap,bool Modular,bool FrameDivision,bool SkipInert> void model2a::timeslice_impl()
{
    using clock_type=std::conditional_t<Modular,uint64_t,atime>;
    auto clock=[](atime t)->clock_type {if constexpr(Modular)return uint64_t(t);else return t;};
    auto less=[](clock_type a,clock_type b){if constexpr(Modular)return vf2::frame_clock::less(a,b);else return a<b;};
    auto later=[&](clock_type a,clock_type b){return less(a,b)?b:a;};
	SCHED_COUNT(++m_scheduler_diagnostics.slices);
	int head = timer_head();
    bool new_head=false;
	while (less(clock(m_base),clock(m_timer[head].expire))) {
		SCHED_COUNT(++m_scheduler_diagnostics.passes);
		const atime initial_target=m_timer[head].expire;
        clock_type target=clock(initial_target);
        auto absolute_target=[&]()->atime{if constexpr(Modular)return vf2::frame_clock::restore(initial_target,target);else return target;};   // (MAME's 1/60 s quantum never comes first here)
		if (m_suspend_changes) apply_suspend_changes();
        if constexpr(SkipInert){
            // A guest may have inserted an earlier event. If all three
            // non-deferred clocks are short of a cycle at that deadline,
            // the original pass would only advance base to the event.
            // Ignore suspension: this proof also holds for eating clocks.
            const auto short_of_cycle=[&](int id){
                const auto &c=m_sched[id];
                return std::bit_cast<int64_t>(target-uint64_t(c.local))<int64_t(uint64_t(c.period));
            };
            if(new_head&&short_of_cycle(CPU_I960)&&short_of_cycle(CPU_TGP)&&short_of_cycle(CPU_Z80)){
                m_base=initial_target;
                break;
            }
        }
		for (int k = 0; k < N_CPUS; k++) {
			const int id = m_order[k];
			if constexpr(!Modular)if(g_cooperative_fifo&&id==CPU_TGP)continue;
			if constexpr(LazyAux)if(id==CPU_68K)continue;
			SCHED_COUNT(++m_scheduler_diagnostics.visits[id]);
			cpu_sched &c = m_sched[id];
			if constexpr(!Modular)if (m_sound_only && id != CPU_68K) { c.local = std::max(c.local, target); continue; }   // (a sound board alone)
			if (!(c.suspend == 0 || c.eat)) continue;
            if constexpr(!NarrowGap)if(less(target,clock(c.local)))continue;
			if constexpr(!Modular)if(g_cooperative_fifo&&g_cooperative_timed&&id==CPU_I960&&m_cooperative_cpu_ready>c.local){
                if constexpr(NarrowGap)if(target<c.local)continue;
                const uint64_t available=div_atime(target-c.local,c.period);
                if(!available)continue;
                const uint64_t needed=div_atime(m_cooperative_cpu_ready-c.local+c.period-1,c.period);
                const uint64_t idle=std::min(available,needed);
                c.local+=atime(idle)*c.period;c.cycles+=idle;m_cooperative_wait_cycles+=idle;
                if(c.local<target)target=std::max(c.local,m_base);
                continue;
            }
			clock_type delta;
            if constexpr(Modular){
                const uint64_t local=uint64_t(c.local);
                if(less(target,local))continue;
                delta=target-local;
                if(delta<uint64_t(c.period))continue;
            }else if constexpr(NarrowGap){
                if(!vf2::scheduler_gap(target,c.local,uint64_t(c.period),delta))continue;
            }else{delta=target-c.local;if(delta<c.period)continue;}
			const uint64_t dividend=uint64_t(delta)>>c.divshift;
			const int cycles=int(u32(FrameDivision?c.divide_cycles.divide_frame(dividend):
                g_reference_division?dividend/c.divisor:c.divide_cycles(dividend)));
			SCHED_COUNT(++m_scheduler_diagnostics.dispatches[id];m_scheduler_diagnostics.budget[id]+=uint64_t(cycles));
			int ran = cycles;
			if (c.suspend == 0) {
				SCHED_COUNT(++m_scheduler_diagnostics.active[id]);
				if (id == CPU_Z80)
					ran = cycles + (cycles & 1);   // NOPs: two steps of 2 cycles
				else {
					m_executing = id;
					// the instruction hooks put on the CPUs only while one is set (each costs a call an instruction)
					if constexpr(!Modular)if (id == CPU_I960 && bool(m_cpu.on_instruction) != bool(on_instruction))
						sync_instruction_hook(id);
					if constexpr(!Modular)if (id == CPU_68K && bool(m_68k->on_instruction) != bool(on_68k_instruction))
						sync_instruction_hook(id);
					if (id == CPU_68K) {
						static const bool sound_prof=getenv("VF2_SOUND_PROFILE")!=nullptr;
						const uint64_t snd_t0=sound_prof?now_ns():0;
						m_68k->set_total_base(c.cycles);
						ran = m_68k->run(cycles);
						if(sound_prof)m_sound_68k_ns+=now_ns()-snd_t0;
					} else
						ran = id == CPU_I960 ? m_cpu.run(cycles) : m_tgp->run(cycles);
					// Complete partial command batches before timers can parse a frame.
					if constexpr(!Modular)if(g_cooperative_fifo&&g_cooperative_batch>1&&id==CPU_I960&&!m_fifo_in.values.empty())service_cooperative_tgp();
					m_executing = -1;
					if (id == CPU_TGP) m_tgp_instructions += uint64_t(ran);
				}
			}
			if constexpr(Modular)vf2::frame_clock::advance(c.local,uint64_t(ran)*uint64_t(c.period));
            else c.local += g_reference_duration?atime(ran)*c.period:c.elapsed(ran);
			SCHED_COUNT(m_scheduler_diagnostics.advanced[id]+=uint64_t(ran);m_scheduler_diagnostics.early[id]+=ran<cycles);
			c.cycles += uint64_t(ran);
			if(g_sched_log)sched_log_cpu(c.local,id,c.suspend!=0,ran,cycles,absolute_target());
			if (less(clock(c.local),target)) target=later(clock(c.local),clock(m_base));
		}
		m_base = absolute_target();
        const int next=timer_head();
        if constexpr(SkipInert)new_head=next!=head;
		head = next;
	}
	// execute_timers
	while ((head = timer_head()) >= 0 && !less(clock(m_base),clock(m_timer[head].expire))) {
		timer &t = m_timer[head];
		const atime period = t.period;
		constexpr int i960_halt = int(T_LINE0) + L_I960_HALT;
		constexpr int tgp_halt = int(T_LINE0) + L_TGP_HALT;
		if (g_fast_halt_delivery && g_sync_timer_fast && !g_reference_timers && !g_sched_log &&
			!period && (head == i960_halt || head == tgp_halt)) {
			auto &queue = m_line_queue[head - T_LINE0];
			if (queue.size() == 1) {
				// HALT delivery only updates the processor's pending suspension.
				// Dequeue at the ordinary callback position to keep equal-time order.
				const int state = queue.front();
				queue.clear();
				timer_disable(head);
				m_callback_time = t.expire;
				m_callback_modified = false;
				SCHED_COUNT(++m_scheduler_diagnostics.fired[head]; ++m_scheduler_diagnostics.single_line);
				const int cpu = head == i960_halt ? CPU_I960 : CPU_TGP;
				if (state == ASSERT_LINE) suspend(cpu, SUSPEND_HALT);
				else resume(cpu, SUSPEND_HALT);
				continue;
			}
		}
		if (!period) timer_disable(head);   // a one-shot timer
		m_in_callback = true;
		m_callback_time = t.expire;
		m_callback_timer = head;
		m_callback_modified = false;
		SCHED_COUNT(++m_scheduler_diagnostics.fired[head]);
		if(head!=T_UART&&g_sched_log)sched_log_fire(t.expire,head);
		timer_fired(head);
		m_in_callback = false;
		m_callback_timer = -1;
		// a periodic one goes again a period on, unless its callback changed it
		if (period && !m_callback_modified) {
			if(!g_reference_timers)timer_unlink(head);
			t.start = t.expire;
			t.expire += period;
			t.seq = ++m_timer_seq;
			if(!g_reference_timers)timer_enqueue(head);else m_head = -1;
		}
	}
}

void model2a::timer_fired(int id)
{
	if (id >= T_LINE0) { apply_line(id - int(T_LINE0)); return; }
	if (id >= T_SCSP0 && id < int(T_SCSP0) + int(scsp_device::N_TIMERS)) { m_scsp->timer_fired(id - T_SCSP0); return; }
	switch (id) {
	case T_SOUND_UPDATE: sound_update_tick(); break;
	case T_UART:
		if (m_sound_only) snd_link_byte();
		else if (m_link_fast) link_event();
		else uart_clock_tick();
		break;
	case T_VBLANK_START: {   // screen_vblank(1)
		timer_adjust(T_VBLANK_START, atime(CYCLES_PER_FRAME) * m_sched[CPU_I960].period);
		timer_adjust(T_VBLANK_END, atime(VBLANK_CYCLES) * m_sched[CPU_I960].period);
		m_framenum = u32(m_frame_number);
		// if 60 Hz mode or frame number is even, trigger geometrizer to start new frame
		if ((m_videocontrol & 1) == 0 || (m_framenum & 1) == 0) {
			m_video->m_geo_frame = m_frame_number;   // (smooth motion's)
			m_video->geo_parse();
		}
		const u32 line = 1 << 0;
		if (m_intena & line) {
			m_intreq |= line;
			irq_update();
		}
		break;
	}
	case T_VBLANK_END:   // MAME draws the screen here, then counts the frame
		if(m_modular_frame&&m_vblank_tile_hook)(*m_vblank_tile_hook)();
		m_video->geometry_wait();
		render_wait();   // (a picture still being drawn: its polygons, before the next frame's)
		if (m_draw_3d) m_video->frame_end(m_render_test_mode, m_frame_number);
		m_frame_number++;
		break;
	case T_TIMER0: case T_TIMER1: case T_TIMER2: case T_TIMER3: timer_fire(id - T_TIMER0); break;
	case T_IRQ_DELAY: {   // irq_mask_delayed_update
		m_intena = m_timer[id].param;
		const u32 line = 1 << 10;
		if ((m_uart.txrdy() || m_uart.rxrdy()) && (m_intena & line)) {
			m_intreq |= line;
			irq_update();
		}
		if (m_link_fast) link_schedule();
		break;
	}
	case T_FIN_EMPTY: fifo_sync_empty(false); break;
	case T_FIN_FULL: fifo_sync_full(false); break;
	case T_FOUT_EMPTY: fifo_sync_empty(true); break;
	case T_FOUT_FULL: fifo_sync_full(true); break;
	}
}

void model2a::irq_update()
{
	set_input_line(L_IRQ0, (m_intreq & 0x001) ? ASSERT_LINE : CLEAR_LINE);   // vblank
	set_input_line(L_IRQ1, (m_intreq & 0x002) ? ASSERT_LINE : CLEAR_LINE);
	set_input_line(L_IRQ2, (m_intreq & 0x3fc) ? ASSERT_LINE : CLEAR_LINE);   // timers
	set_input_line(L_IRQ3, (m_intreq & 0xc00) ? ASSERT_LINE : CLEAR_LINE);   // sound (UART)
}

void model2a::timer_fire(int n)
{
	if (!m_timerrun[n]) return;
	const u32 line = 1 << (n + 2);
	if (m_intena & line) {
		m_intreq |= line;
		irq_update();
	}
	m_timervals[n] = 0xfffff;
	m_timerrun[n] = false;
}

void model2a::run_until(uint64_t cycle)
{
	const atime t = atime(cycle) * m_sched[CPU_I960].period;
	while (m_base < t) timeslice();
	advance_aux_clocks(m_base);
}

// A frame's first vblank bounds every selected head deadline. Active CPUs
// and expired timers start within one second of base; execution can advance
// less than another second plus a vblank. All compared gaps stay far below
// the signed low-limb half-range. Long future timers remain ordered in 128 bits.
bool model2a::modular_frame_ready()const
{
    // Cost probe only: the board's presence disables this fast path. Take it
    // away from the sound-off arm instead, to see whether the tail follows the
    // path rather than the board.
    static const bool no_modular=std::getenv("VF2_NO_MODULAR")!=nullptr;
    if(no_modular)return false;
    // The modular path was gated on the board being held because it skipped the
    // 68000. timeslice() can run the 68000 in modular passes, so allow it back
    // (opt-in until qualified on all three scenes).
    static const bool modular_sound=std::getenv("VF2_MODULAR_SOUND")!=nullptr;
    if(!g_modular_clock||!g_lazy_aux_clocks||g_reference_duration||(m_sound_on&&!modular_sound)||m_sound_only||g_cooperative_fifo||g_sched_log)return false;
    if(on_instruction||on_68k_instruction||m_cpu.on_instruction||m_68k->on_instruction||m_tgp->cpu.on_instruction||on_copro||on_milestone||on_uart_tx||on_68k_scsp_write)return false;
    const atime window=AS_PER_SEC;
    if(m_base>(atime(1)<<100))return false;
    const timer &v=m_timer[T_VBLANK_START];
    if(!v.enabled||v.expire<m_base||v.expire-m_base>window)return false;
    for(int id:{CPU_I960,CPU_TGP,CPU_Z80}){
        const auto &c=m_sched[id];
        if(!c.period||c.period>AS_PER_SEC/1000000||!vf2::frame_clock::near(c.local,m_base,uint64_t(window)))return false;
    }
    for(const auto &t:m_timer)if(t.enabled&&t.expire<m_base&&m_base-t.expire>window)return false;
    return true;
}

// Runs to the end of the frame's vblank, where MAME draws the screen.
void model2a::run_frame()
{
	const uint64_t f = m_frame_number;
	if (!m_ram_log.empty() && m_ram_log.front().frame + 64 < f) {   // (the lists kept keep the last frames' changes: a held picture's too)
		ram_guard();   // (a picture being drawn alongside may be reading the log: taking its RAMs back)
		auto it = m_ram_log.begin();
		while (it != m_ram_log.end() && it->frame + 64 < f) ++it;
		m_ram_log.erase(m_ram_log.begin(), it);
	}
	keep_2d();
	// (Cheats' random fighters at the select: a random player's controls let go, its cursor and pick pressed for it
	// (random_fighter.cpp's select_inputs), this frame's only: the inputs as given after it)
	const u8 in_were[2] = { m_in1, m_in2 }, sw_was = m_sw;
	if (m_random) random_select_inputs();
	if (m_debug_mode) {   // (Arcade settings > Debug mode: the secret code's flag, and switch 1 on, this frame's only)
		write_dword(0x508000, read_dword(0x508000) | 0x4000);
		m_sw = u8(m_sw & ~1);
	}
	m_modular_frame=modular_frame_ready();if(m_modular_frame)++m_modular_frames;
    while (m_frame_number == f) timeslice();
    m_modular_frame=false;
	advance_aux_clocks(m_base);
	m_in1 = in_were[0]; m_in2 = in_were[1]; m_sw = sw_was;
	take_audio();
	m_scroll_hist[f % SCROLL_HIST] = { int64_t(f), m_tiles.tile_r(0x5002), m_tiles.tile_r(0x5006) };   // (smooth motion's backdrop)
	record_scroll_input(f);
	if (m_random) random_frame();   // (Cheats' random fighters: random_fighter.cpp)
	if (m_random || !m_rank_names.empty()) ranking_frame();   // (their names in the ranking: drawing only)
}

// ---------------------------------------------------------------------------------------------
// The FIFOs between the i960 and the TGP (generic_fifo_u32_device, 8 deep). On an empty pop the
// reader retries the instruction and, if a sync later finds the FIFO still empty, is halted
// until something is pushed; a push into a full FIFO halts the writer the same way.
//   copro_fifo_in:  i960 -> TGP     copro_fifo_out: TGP -> i960
// ---------------------------------------------------------------------------------------------

void model2a::prepare_cooperative_fifo()
{
    for(int id:{T_FIN_EMPTY,T_FIN_FULL,T_FOUT_EMPTY,T_FOUT_FULL})timer_disable(id);
    for(int line:{L_I960_HALT,L_TGP_HALT}){
        m_line_queue[line].clear();timer_disable(int(T_LINE0)+line);
    }
    for(int id:{CPU_I960,CPU_TGP}){m_sched[id].suspend&=~SUSPEND_HALT;m_sched[id].nextsuspend&=~SUSPEND_HALT;}
    m_suspend_changes=true;
    m_fifo_in.empty_triggered=m_fifo_in.full_triggered=false;
    m_fifo_out.empty_triggered=m_fifo_out.full_triggered=false;
    m_cooperative_ready=true;m_cooperative_pumping=m_cooperative_wait_input=false;
    m_cooperative_cpu_ready=0;m_cooperative_output_ready.clear();
    if(g_cooperative_timed)for(size_t i=0;i<m_fifo_out.values.size()+m_fifo_out.extra.size();++i)m_cooperative_output_ready.push_back(m_base);
}

bool model2a::cooperative_wait_until(const atime &ready)
{
    const atime now=time();if(ready<=now)return true;
    const auto &c=m_sched[CPU_I960];
    const uint64_t needed=div_atime(ready-now+c.period-1,c.period);
    const int available=std::max(0,m_cpu.remaining_cycles());
    const int idle=int(std::min(needed,uint64_t(available)));
    m_cpu.wait_cycles(idle);m_cooperative_wait_cycles+=uint64_t(idle);
    if(uint64_t(idle)==needed)return true;
    m_cooperative_cpu_ready=ready;return false;
}

void model2a::service_cooperative_tgp()
{
    if(m_cooperative_pumping||m_executing!=CPU_I960||m_fifo_out.is_full()||(m_coproctl&0x80000000))return;
    auto &c=m_sched[CPU_TGP];
    if((c.suspend|c.nextsuspend)&SUSPEND_RESET)return;
    if(m_cooperative_wait_input&&m_fifo_in.values.empty())return;
    c.local=std::max(c.local,time());
    const int saved=m_executing;m_executing=CPU_TGP;m_cooperative_pumping=true;
    for(unsigned batch=0;batch<16;++batch){
        m_cooperative_wait_input=false;
        const int ran=m_tgp->run(65536);
        c.local+=c.elapsed(ran);c.cycles+=uint64_t(ran);m_tgp_instructions+=uint64_t(ran);
        ++m_cooperative_calls;m_cooperative_cycles+=uint64_t(ran);
        if(m_cooperative_wait_input||m_fifo_out.is_full())break;
        if(batch==15){std::fprintf(stderr,"Cooperative DSP budget exhausted at PC %04x\n",m_tgp->cpu.pc());std::abort();}
    }
    m_cooperative_pumping=false;m_executing=saved;
}

void model2a::report_cooperative_fifo(FILE *out)const
{
    if(g_cooperative_fifo)std::fprintf(out,"Cooperative FIFO: %llu DSP services, %llu cycles, max queued input %llu words, %llu i960 virtual wait cycles, input batch threshold %u\n",(unsigned long long)m_cooperative_calls,(unsigned long long)m_cooperative_cycles,(unsigned long long)m_cooperative_max_input,(unsigned long long)m_cooperative_wait_cycles,g_cooperative_batch);
}

u32 model2a::fifo_pop(bool out)
{
	gen_fifo &f = out ? m_fifo_out : m_fifo_in;
	if(g_cooperative_fifo){
        if(out)m_cooperative_read_waited=false;
        if(out&&f.values.empty())service_cooperative_tgp();
        if(f.values.empty()){
            if(!out){m_cooperative_wait_input=true;m_tgp->cpu.stall();m_tgp->cpu.abort_timeslice();return 0;}
            std::fprintf(stderr,"Cooperative output starvation at i960 PC %08x DSP PC %04x input %zu\n",m_cpu.pip(),m_tgp->cpu.pc(),m_fifo_in.values.size()+m_fifo_in.extra.size());std::abort();
        }
        if(out&&g_cooperative_timed&&!cooperative_wait_until(m_cooperative_output_ready.front())){
            m_cooperative_read_waited=true;m_cpu.i960_stall();return 0;
        }
        const u32 value=f.values.front();f.values.pop_front();
        if(out&&g_cooperative_timed)m_cooperative_output_ready.pop_front();
        if(!f.extra.empty()){f.values.push_back(f.extra.front());f.extra.pop_front();}
        return value;
    }
	if (f.values.empty()) {
		timer_sync(out ? T_FOUT_EMPTY : T_FIN_EMPTY);
		if (out) m_cpu.i960_stall(); else m_tgp->cpu.stall();   // on_fifo_empty_pre_sync
		return 0;
	}
	const u32 v = f.values.front();
	f.values.pop_front();
	if (!f.extra.empty()) {
		f.values.push_back(f.extra.front());
		f.extra.pop_front();
		if (f.extra.empty() && f.full_triggered) {
			f.full_triggered = false;
			set_input_line(out ? L_TGP_HALT : L_I960_HALT, CLEAR_LINE);   // on_fifo_unfull
		}
	}
	return v;
}

void model2a::fifo_push(bool out, u32 v)
{
	gen_fifo &f = out ? m_fifo_out : m_fifo_in;
	if(g_cooperative_fifo){
        if(out&&g_cooperative_timed)m_cooperative_output_ready.push_back(time());
        if(f.is_full()||!f.extra.empty())f.extra.push_back(v);else f.values.push_back(v);
        if(out){if(f.is_full())m_tgp->cpu.abort_timeslice();}
        else{
            m_cooperative_max_input=std::max(m_cooperative_max_input,uint64_t(f.values.size()+f.extra.size()));
            if(f.extra.size()>65536){std::fprintf(stderr,"Cooperative input queue limit\n");std::abort();}
            if(f.values.size()+f.extra.size()>=g_cooperative_batch)service_cooperative_tgp();
        }
        return;
    }
	if (!f.extra.empty())
		f.extra.push_back(v);
	else if (f.is_full()) {
		f.extra.push_back(v);
		timer_sync(out ? T_FOUT_FULL : T_FIN_FULL);
	} else {
		f.values.push_back(v);
		if (f.empty_triggered) {
			f.empty_triggered = false;
			set_input_line(out ? L_I960_HALT : L_TGP_HALT, CLEAR_LINE);   // on_fifo_unempty
		}
	}
}

void model2a::fifo_clear(bool out)
{
	gen_fifo &f = out ? m_fifo_out : m_fifo_in;
	if(g_cooperative_fifo&&out)m_cooperative_output_ready.clear();
	f.values.clear();
	f.extra.clear();
	timer_disable(out ? T_FOUT_EMPTY : T_FIN_EMPTY);
	timer_disable(out ? T_FOUT_FULL : T_FIN_FULL);
	if (f.full_triggered) {
		f.full_triggered = false;
		set_input_line(out ? L_TGP_HALT : L_I960_HALT, CLEAR_LINE);
	}
}

void model2a::assert_fifo_halt(int line)
{
	// This is the last action of both FIFO synchronization callbacks.
	const int id=int(T_LINE0)+line;
	auto &halt=m_timer[id];
	const int head=m_timer_first;
	if (g_fuse_fifo_halt && g_fast_halt_delivery && g_sync_timer_fast &&
		!g_reference_timers && !g_sched_log && m_in_callback &&
		m_callback_timer>=0 && !m_timer[m_callback_timer].enabled &&
		m_executing<0 && !halt.enabled && m_line_queue[line].empty() &&
		(head<0 || m_callback_time<m_timer[head].expire)) {
		// The queued HALT would be the next event, at the FIFO callback's
		// time. Preserve its serialized timer fields, then assert the line
		// without inserting and immediately unlinking a timer.
		halt.start=halt.expire=m_callback_time;
		halt.period=0;
		halt.param=0;
		halt.seq=++m_timer_seq;
		SCHED_COUNT(++m_scheduler_diagnostics.sync_calls; ++m_scheduler_diagnostics.adjusted[id];
		            ++m_scheduler_diagnostics.fired[id]; ++m_scheduler_diagnostics.single_line);
		suspend(line==L_I960_HALT?CPU_I960:CPU_TGP,SUSPEND_HALT);
		return;
	}
	set_input_line(line,ASSERT_LINE);
}

void model2a::fifo_sync_empty(bool out)
{
	gen_fifo &f = out ? m_fifo_out : m_fifo_in;
	if (f.values.empty() && !f.empty_triggered) {
		f.empty_triggered = true;
		assert_fifo_halt(out ? L_I960_HALT : L_TGP_HALT);   // on_fifo_empty_post_sync
	}
}

void model2a::fifo_sync_full(bool out)
{
	gen_fifo &f = out ? m_fifo_out : m_fifo_in;
	while (!f.extra.empty() && !f.is_full()) {
		f.values.push_back(f.extra.front());
		f.extra.pop_front();
	}
	if (!f.extra.empty() && !f.full_triggered) {
		f.full_triggered = true;
		assert_fifo_halt(out ? L_TGP_HALT : L_I960_HALT);   // on_fifo_full_post_sync
	}
}

// ---------------------------------------------------------------------------------------------
// The i960's memory map: model2a_crx_mem (with model2_tgp_mem and model2_base_mem)
// ---------------------------------------------------------------------------------------------

namespace { constexpr u16 BURST = i960_cpu_device::BURST; }

u16 model2a::flags(offs_t a)
{
	if (g_fifo_port_flags && (a & 3u) == 0 && a - 0x00884000u < 0x4000u)
		return g_fifo_port_flags;
	// the regions MAME maps with .flags(i960_cpu_device::BURST): ldl/ldt/ldq step through them,
	// and repeat the same address everywhere else (the FIFOs)
	if (a < 0x00240000) return BURST;                               // ROM, RAM
	if (a >= 0x00500000 && a < 0x00600000) return BURST;           // work RAM
	if (a >= 0x00804000 && a < 0x00808000) return BURST;           // geo_prg
	if (a >= 0x00880000 && a < 0x00884000) return BURST;           // copro function port
	if (a >= 0x00900000 && a < 0x00980000) return BURST;           // buffer RAM
	if ((a & ~0x110000u) >= 0x01000000 && (a & ~0x110000u) < 0x01010000) return BURST;   // tile RAM
	if ((a & ~0x100000u) >= 0x01020000 && (a & ~0x100000u) < 0x01020004) return BURST;
	if ((a & ~0x100000u) >= 0x01080000 && (a & ~0x100000u) < 0x01100000) return BURST;   // char RAM
	if (a >= 0x01800000 && a < 0x01804000) return BURST;           // palette
	if (a >= 0x01810000 && a < 0x0181c000) return BURST;           // colour translation
	if ((a & ~0x10000u) >= 0x01a00000 && (a & ~0x10000u) < 0x01a04000) return BURST;     // M2COMM
	if (a >= 0x01d00000 && a < 0x01d04000) return BURST;           // backup RAM
	if (a >= 0x02000000 && a < 0x04000000) return BURST;           // data ROMs
	if (a >= 0x06000000 && a < 0x07000000) return BURST;
	if (a >= 0x11600000 && a < 0x11700000) return BURST;           // framebuffers
	if (a >= 0x12000000 && a < 0x12820000) return BURST;           // texture and luma RAM
	return 0;
}

// Plain memory: P points at the byte (ROM set for the read-only ones); devices leave it null.
#define MEM_CASES(a, P, ROM, TrackWrite)                                                                   \
	if (a < 0x00200000) { ROM = true; P = (u8 *)&m_rom[a]; }                                   \
	else if (a < 0x00240000) P = &m_ram200[a - 0x00200000];                                    \
	else if (a >= 0x00500000 && a < 0x00600000) P = &m_workram[a - 0x00500000];                \
	else if (a >= 0x00900000 && a < 0x00980000) {                                           \
        if(g_cooperative_fifo&&g_cooperative_batch>1&&!m_fifo_in.values.empty())service_cooperative_tgp(); \
        if constexpr(TrackWrite)if(g_track_geometry_pages)m_video->mark_geometry_page(a&0x1ffff); \
        P = &m_bufferram[a & 0x1ffff]; }                                                   \
	else if (a >= 0x00e00000 && a < 0x00e00038) P = &m_cpuctl[a - 0x00e00000];                 \
	else if ((a & ~0x10000u) >= 0x01a00000 && (a & ~0x10000u) < 0x01a04000) P = &m_m2comm_share[a & 0x3fff]; \
	else if (a >= 0x01d00000 && a < 0x01d04000) P = &m_backup[a - 0x01d00000];                 \
	else if (a >= 0x02000000 && a < 0x04000000) { ROM = true; P = (u8 *)&m_data[a - 0x02000000]; } \
	else if (a >= 0x06000000 && a < 0x07000000) { ROM = true; P = (u8 *)&m_data[a - 0x06000000 + 0x1000000]; }

// The recompiled code's direct view of plain memory: each 4 KB page below 0x08000000 that is
// all one plain region (no device, no side effects), as MEM_CASES maps it. Every such page is one
// MAME maps with BURST (flags()).
void model2a::map_plain_pages(i960rec::runtime &r)
{
	r.rpage.assign(i960rec::runtime::PAGES, nullptr);
	r.wpage.assign(i960rec::runtime::PAGES, nullptr);
	for (u32 pg = 0; pg < i960rec::runtime::PAGES; pg++) {
		const u32 a = pg << 12, z = a + 0xfff;
		// Deferred DSP commands must complete before either side of an i960
		// shared-buffer access. Route these pages through the memory helpers.
		if(g_cooperative_fifo&&g_cooperative_batch>1&&a>=0x00900000&&a<0x00980000)continue;
		u8 *p = nullptr, *q = nullptr; bool rom = false, rom2 = false;
		MEM_CASES(a, p, rom, false)
		MEM_CASES(z, q, rom2, false)
		if (!p || q != p + 0xfff || rom != rom2 || flags(a) != BURST || flags(z) != BURST) continue;
		r.rpage[pg] = p;
        // Writes through a mapped buffer page would bypass dirty tracking.
        if (!rom && (!g_track_geometry_pages || a-0x00900000u>=0x80000u))
            r.wpage[pg]=p;
	}
	r.timer_poll_context=this; r.collapse_timer_poll=collapse_i960_timer_poll;
}
u8 *model2a::direct_register_frame(offs_t a)
{
	const u32 offset = a - 0x00500000;
	if (offset <= 0x000fffc0)
		return m_workram.data() + offset;
	return nullptr;
}

u32 model2a::read_dword(offs_t a)
{
	a &= ~3u;
	if (g_fixed_fifo_load && a - 0x00884000u < 0x4000u)
		return copro_fifo_r();
	u8 *p = nullptr; bool rom = false;
	MEM_CASES(a, p, rom, false)
	if (p) { u32 v; memcpy(&v, p, 4); return v; }
	return dev_read(a, 0xffffffff);
}

u16 model2a::read_word(offs_t a)
{
	a &= ~1u;
	u8 *p = nullptr; bool rom = false;
	MEM_CASES(a, p, rom, false)
	if (p) { u16 v; memcpy(&v, p, 2); return v; }
	const int sh = (a & 2) * 8;
	return u16(dev_read(a & ~3u, 0xffffu << sh) >> sh);
}

u8 model2a::read_byte(offs_t a)
{
	u8 *p = nullptr; bool rom = false;
	MEM_CASES(a, p, rom, false)
	if (p) return *p;
	const int sh = (a & 3) * 8;
	return u8(dev_read(a & ~3u, 0xffu << sh) >> sh);
}

void model2a::write_dword(offs_t a, u32 v)
{
	a &= ~3u;
	if (g_fixed_fifo_store && a - 0x00884000u < 0x4000u) {
		if (on_copro) on_copro('w', a - 0x00880000, v);
		copro_fifo_w(v);
		return;
	}
	u8 *p = nullptr; bool rom = false;
	MEM_CASES(a, p, rom, true)
	if (p) { if (!rom) memcpy(p, &v, 4); return; }
	dev_write(a, v, 0xffffffff);
}

void model2a::write_word(offs_t a, u16 v)
{
	a &= ~1u;
	u8 *p = nullptr; bool rom = false;
	MEM_CASES(a, p, rom, true)
	if (p) { if (!rom) memcpy(p, &v, 2); return; }
	const int sh = (a & 2) * 8;
	dev_write(a & ~3u, u32(v) << sh, 0xffffu << sh);
}

void model2a::write_byte(offs_t a, u8 v)
{
	u8 *p = nullptr; bool rom = false;
	MEM_CASES(a, p, rom, true)
	if (p) { if (!rom) *p = v; return; }
	const int sh = (a & 3) * 8;
	dev_write(a & ~3u, u32(v) << sh, 0xffu << sh);
}

void model2a::unmapped(u32 a, bool write)
{
	int &n = m_unmapped[a | (write ? 1 : 0)];
	if (n++ == 0 && getenv("VF2_LOG_UNMAPPED"))
		fprintf(stderr, "unmapped %s %08x (pc %08x)\n", write ? "write" : "read", a, m_cpu.pip());
}

// 16-bit devices on the 32-bit bus: MAME calls the handler once per half the access covers
template <typename R> static u32 read16_halves(u32 mem_mask, R r)
{
	u32 v = 0;
	if (mem_mask & 0x0000ffff) v |= r(0, u16(mem_mask));
	if (mem_mask & 0xffff0000) v |= u32(r(1, u16(mem_mask >> 16))) << 16;
	return v;
}
template <typename W> static void write16_halves(u32 data, u32 mem_mask, W w)
{
	if (mem_mask & 0x0000ffff) w(0, u16(data), u16(mem_mask));
	if (mem_mask & 0xffff0000) w(1, u16(data >> 16), u16(mem_mask >> 16));
}

u32 model2a::dev_read(u32 a, u32 mem_mask)
{
	// geometrizer
	if (a >= 0x00800000 && a < 0x00804000) {
		if (a - 0x00800000 == 0x2008) return m_geo_write_start;
		if (a - 0x00800000 == 0x3008) return m_video->m_geo_read_start_address;
		return 0;
	}
	if (a >= 0x00804000 && a < 0x00808000) return 0xffffffff;      // geo_prg_r
	if (a >= 0x00884000 && a < 0x00888000) return copro_fifo_r();
	if (a >= 0x00980000 && a < 0x00980040) {
		switch (a & 0x3f) {
		case 0x00: return m_coproctl;
		case 0x04:   // fifo_control_r: the TGP's output FIFO is empty
			if(g_cooperative_fifo&&m_fifo_out.values.empty())service_cooperative_tgp();
			if(g_cooperative_fifo&&g_cooperative_timed&&!m_fifo_out.values.empty()&&!cooperative_wait_until(m_cooperative_output_ready.front()))return 1;
			return m_fifo_out.values.empty() ? 1 : 0;
		case 0x0c: {
			u8 framenum = !m_render_mode ? (m_framenum & 2) << 1 : (m_framenum & 1) << 2;
			return framenum | (m_videocontrol & 3);
		}
		case 0x30: case 0x34: case 0x38: case 0x3c: {   // tgpid_r, one byte per lane
			static const u8 ID[] = { 0, 'T', 'A', 'H', 0, 'A', 'K', 'O', 0, 'Z', 'A', 'K', 0, 'M', 'T', 'K' };
			u32 v = 0;
			for (int b = 0; b < 4; b++) if (mem_mask & (0xffu << (8 * b))) v |= u32(ID[(a & 0xf) + b]) << (8 * b);
			return v;
		}
		}
		unmapped(a, false);
		return 0;
	}
	if (a == 0x00e80000) return m_intreq;
	if (a == 0x00e80004) return m_intena;
	if (a >= 0x00f00000 && a < 0x00f00010) {
		int n = (a >> 2) & 3;
		if (m_timerrun[n]) {
            const auto &clock=m_sched[CPU_I960];
            const auto elapsed=time()-m_timer[T_TIMER0+n].start;
            uint64_t ticks;
            if(g_i960_timer_reciprocal){
                // The fixed 25 MHz period factors exactly into a power of
                // two and the scheduler's cached 32-bit divisor.
                const auto dividend=elapsed>>clock.divshift;
                ticks=(dividend>>64)==0?clock.divide_cycles(uint64_t(dividend)):div_atime(elapsed,clock.period);
            }else ticks=div_atime(elapsed,clock.period);
            m_timervals[n]=m_timerorig[n]-u32(ticks);
        }
		return m_timervals[n];
	}
	const u32 t = a & ~0x110000u;
	if (t >= 0x01000000 && t < 0x01010000) {
		const offs_t o = (t - 0x01000000) >> 1;
		return read16_halves(mem_mask, [&](int h, u16) { return m_tiles.tile_r(o + h); });
	}
	const u32 c = a & ~0x100000u;
	if (c >= 0x01080000 && c < 0x01100000) {
		const offs_t o = (c - 0x01080000) >> 1;
		return read16_halves(mem_mask, [&](int h, u16) { return m_tiles.char_r(o + h); });
	}
	if (a >= 0x01800000 && a < 0x01804000) {
		const offs_t o = (a - 0x01800000) >> 1;
		ram_guard();
		return read16_halves(mem_mask, [&](int h, u16) { return m_palram[o + h]; });
	}
	if (a >= 0x01810000 && a < 0x0181c000) {
		const offs_t o = (a - 0x01810000) >> 1;
		ram_guard();
		return read16_halves(mem_mask, [&](int h, u16) { return m_colorxlat[o + h]; });
	}
	if ((a & ~0x10000u) == 0x01a04000) {   // M2COMM cn (lane 0) and fg (lane 2)
		u32 v = 0;
		if (mem_mask & 0x000000ff) v |= u32(m_m2comm_cn | 0xfe);
		if (mem_mask & 0x00ff0000) v |= u32(u8(m_m2comm_fg | 0x7e)) << 16;   // ~zfg << 7 with zfg 0: bit 7 set
		return v;
	}
	if (a >= 0x01c00000 && a < 0x01c00020) {   // 315-5649, umask32 0x00ff00ff
		const int o = int((a - 0x01c00000) >> 1);
		u32 v = 0;
		if (mem_mask & 0x000000ff) v |= io_read(o);
		if (mem_mask & 0x00ff0000) v |= u32(io_read(o + 1)) << 16;
		return v;
	}
	if (a == 0x01c80000) {   // i8251: data (lane 0), status (lane 2); VF2 never reads it
		u32 v = 0;
		if (mem_mask & 0x00ff0000) v |= u32(m_uart.status | 0x80) << 16;   // DSR
		return v;
	}
	if (a >= 0x10000000 && a < 0x10200000) return (m_render_unk << 14) | (m_render_mode << 2) | (m_render_test_mode << 0);
	if (a >= 0x10400000 && a < 0x10600000) return m_video->polygon_count_r();
	if (a == 0x10800000) return 0;
	if (a >= 0x11600000 && a < 0x11680000) {
		const offs_t o = (a - 0x11600000) >> 1;
		return read16_halves(mem_mask, [&](int h, u16) { return m_fbvram_a[o + h]; });
	}
	if (a >= 0x11680000 && a < 0x11700000) {
		const offs_t o = (a - 0x11680000) >> 1;
		return read16_halves(mem_mask, [&](int h, u16) { return m_fbvram_b[o + h]; });
	}
	if (a >= 0x12000000 && a < 0x12820000) ram_guard();
	if (a >= 0x12000000 && a < 0x12400000) return m_tex0[((a - 0x12000000) & 0x1fffff) >> 2];
	if (a >= 0x12400000 && a < 0x12800000) return m_tex1[((a - 0x12400000) & 0x1fffff) >> 2];
	if (a >= 0x12800000 && a < 0x12820000) return (mem_mask & 0xff) ? m_lumaram[(a - 0x12800000) >> 2] : 0;
	unmapped(a, false);
	return 0;
}

void model2a::dev_write(u32 a, u32 data, u32 mem_mask)
{
	if (a >= 0x00800000 && a < 0x00804000) {   // geo_w: the geometrizer's command port (no 3D yet)
		const u32 address = a - 0x00800000;
		if (address < 0x1000) {
			if (data & 0x80000000) {
				u32 r = (data & 0x800fffff) | (((address >> 4) & 0x3f) << 23);
				if(g_track_geometry_pages)m_video->mark_geometry_page(m_geo_write_start&0x1fffc);
				memcpy(&m_bufferram[(m_geo_write_start / 4 * 4) & 0x1fffc], &r, 4);
				m_geo_write_start += 4;
			} else if ((address & 0xf) == 0) {
				u32 r = (data & 0x000fffff) | (((address >> 4) & 0x3f) << 23);
				if ((address >> 4) & 0xc0) {
					u8 function = (address >> 4) & 0x3f;
					if (function == 1) r |= ((address >> 10) & 3) << 29;
				}
				if(g_track_geometry_pages)m_video->mark_geometry_page(m_geo_write_start&0x1fffc);
				memcpy(&m_bufferram[(m_geo_write_start / 4 * 4) & 0x1fffc], &r, 4);
				m_geo_write_start += 4;
			}
		} else if (address == 0x1008) m_geo_write_start = data & 0xfffff;
		else if (address == 0x3008) m_video->m_geo_read_start_address = data & 0xfffff;
		else fprintf(stderr, "geo_w: %08x = %08x\n", address, data);
		return;
	}
	if (a >= 0x00804000 && a < 0x00808000) {   // geo_prg_w
		if (m_geoctl & 0x80000000) m_geocnt++;
		else {
            const u32 offset=m_geo_write_start&0x1fffc;
            if(g_track_geometry_pages)m_video->mark_geometry_page(offset);
            memcpy(&m_bufferram[offset],&data,4);
            m_geo_write_start+=4;
        }
		return;
	}
	if (a >= 0x00880000 && a < 0x00888000 && on_copro) on_copro('w', a - 0x00880000, data);
	if (a >= 0x00880000 && a < 0x00884000) {   // copro_function_port_w
		const u32 offset = (a - 0x00880000) >> 2;
		u32 d = (data & 0x800fffff) | (((offset >> 2) & 0xff) << 23);
		fifo_push(false, d);
		return;
	}
	if (a >= 0x00884000 && a < 0x00888000) { copro_fifo_w(data); return; }
	if (a >= 0x00980000 && a < 0x00980040) {
		switch (a & 0x3f) {
		case 0x00: copro_ctl1_w(data, mem_mask); return;
		case 0x08:   // geo_ctl1_w
			if ((data ^ m_geoctl) == 0x80000000) { if (data & 0x80000000) m_geocnt = 0; }
			m_geoctl = data;
			return;
		case 0x0c: COMBINE_DATA(&m_videocontrol); return;
		}
		unmapped(a, true);
		return;
	}
	if (a == 0x00e80000) {   // irq_ack_w
		m_intreq &= data;
		irq_update();
		if (m_link_fast) link_schedule();   // (an idle transmitter's next step can ask again)
		return;
	}
	if (a == 0x00e80004) {   // irq_enable_w: the mask changes 80 ns later (vcop2 needs it)
		u32 temp = m_intena;
		COMBINE_DATA(&temp);
		timer_adjust(T_IRQ_DELAY, AS_PER_SEC / 1000000000 * 80, temp);
		return;
	}
	if (a >= 0x00f00000 && a < 0x00f00010) {   // timers_w: they count down at 25 MHz
		int n = (a >> 2) & 3;
		COMBINE_DATA(&m_timervals[n]);
		m_timerorig[n] = m_timervals[n];
		timer_adjust(T_TIMER0 + n, atime(m_timerorig[n]) * (AS_PER_SEC / 25000000));
		m_timerrun[n] = true;
		return;
	}
	const u32 t = a & ~0x110000u;
	if (t >= 0x01000000 && t < 0x01010000) {
		const offs_t o = (t - 0x01000000) >> 1;
		write16_halves(data, mem_mask, [&](int h, u16 d, u16 m) { touch(P_TILE, ((o + h) & 0x7fff) * 2); m_tiles.tile_w(o + h, d, m); });
		return;
	}
	if ((a & ~0x100000u) == 0x01020000 || (a & ~0x100000u) == 0x01070000) return;   // nopw
	if ((a & ~0x100000u) == 0x01040000) { if (mem_mask & 0xffff) m_video->horizontal_sync_w(u16(data)); return; }   // xhout
	if ((a & ~0x100000u) == 0x01060000) { if (mem_mask & 0xffff) m_video->vertical_sync_w(u16(data)); return; }     // xvout
	const u32 c = a & ~0x100000u;
	if (c >= 0x01080000 && c < 0x01100000) {
		const offs_t o = (c - 0x01080000) >> 1;
		write16_halves(data, mem_mask, [&](int h, u16 d, u16 m) { touch(P_CHAR, ((o + h) & 0x3ffff) * 2); m_tiles.char_w(o + h, d, m); });
		return;
	}
	if (a >= 0x01800000 && a < 0x01804000) {
		const offs_t o = (a - 0x01800000) >> 1;
		write16_halves(data, mem_mask, [&](int h, u16 d, u16 m) { palette_w(o + h, d, m); });
		return;
	}
	if (a >= 0x01810000 && a < 0x0181c000) {   // colorxlat_w
		const offs_t o = (a - 0x01810000) >> 1;
		ram_guard();
		write16_halves(data, mem_mask, [&](int h, u16 d, u16 m) {
			const offs_t offset = o + h;
			const u16 v = (m_colorxlat[offset] & ~m) | (d & m);
			if (v == m_colorxlat[offset]) return;   // (the same value: nothing to draw again)
			before_3d_ram_change(RAM_XLAT, offset, m_colorxlat[offset], v);
			touch(P_XLAT, offset * 2);
			m_colorxlat[offset] = v;
			if ((offset & 0xff) == 0x80 / 2) m_palette_dirty = true;
		});
		return;
	}
	if (a == 0x0181c000) { m_video->model2_3d_zclip_w(data); return; }
	if ((a & ~0x10000u) == 0x01a04000) {
		if (mem_mask & 0x000000ff) { m_m2comm_cn = data & 1; if (!m_m2comm_cn) m_m2comm_fg = 0; }
		if (mem_mask & 0x00ff0000) m_m2comm_fg = (data >> 16) & 1;
		return;
	}
	if (a >= 0x01c00000 && a < 0x01c00020) {
		const int o = int((a - 0x01c00000) >> 1);
		if (mem_mask & 0x000000ff) io_write(o, u8(data));
		if (mem_mask & 0x00ff0000) io_write(o + 1, u8(data >> 16));
		return;
	}
	if (a == 0x01c00040) return;   // nopw
	if (a == 0x01c80000) {
		if (mem_mask & 0x000000ff) uart_write(0, u8(data));
		if (mem_mask & 0x00ff0000) uart_write(1, u8(data >> 16));
		return;
	}
	if (a >= 0x10000000 && a < 0x10200000) {   // render_mode_w
		m_render_test_mode = BIT(data, 0);
		m_render_mode = BIT(data, 2);
		m_render_unk = BIT(data, 14);
		return;
	}
	if (a >= 0x11600000 && a < 0x11680000) {
		const offs_t o = (a - 0x11600000) >> 1;
		write16_halves(data, mem_mask, [&](int h, u16 d, u16 m) { touch(P_FB_A, (o + h) * 2); m_fbvram_a[o + h] = (m_fbvram_a[o + h] & ~m) | (d & m); });
		return;
	}
	if (a >= 0x11680000 && a < 0x11700000) {
		const offs_t o = (a - 0x11680000) >> 1;
		write16_halves(data, mem_mask, [&](int h, u16 d, u16 m) { touch(P_FB_B, (o + h) * 2); m_fbvram_b[o + h] = (m_fbvram_b[o + h] & ~m) | (d & m); });
		return;
	}
	if (a >= 0x12000000 && a < 0x12800000) {   // tex0_w / tex1_w: 16 bits per dword written
		ram_guard();
		std::vector<u32> &tex = a < 0x12400000 ? m_tex0 : m_tex1;
		const offs_t offset = ((a - (a < 0x12400000 ? 0x12000000 : 0x12400000)) & 0x1fffff) >> 2;
		u32 &t = tex[offset >> 1];
		const u32 v = (offset & 1) == 0 ? (t & 0xffff0000) | (data & 0xffff) : (t & 0x0000ffff) | ((data & 0xffff) << 16);
		if (v == t) return;   // (the same value: nothing changes, and a picture being drawn may be reading it)
		before_3d_ram_change(a < 0x12400000 ? RAM_TEX0 : RAM_TEX1, offset >> 1, t, v);   // (the 3D's colours: model2_video::defer_drawing)
		touch(a < 0x12400000 ? P_TEX0 : P_TEX1, (offset >> 1) * 4);
		t = v;
		return;
	}
	if (a >= 0x12800000 && a < 0x12820000) {
		if (!(mem_mask & 0xff)) return;
		ram_guard();
		u8 &l = m_lumaram[(a - 0x12800000) >> 2];
		if (u8(data) == l) return;   // (as above)
		before_3d_ram_change(RAM_LUMA, (a - 0x12800000) >> 2, l, u8(data));
		touch(P_LUMA, (a - 0x12800000) >> 2);
		l = u8(data);
		return;
	}
	unmapped(a, true);
}

// ---------------------------------------------------------------------------------------------
// Coprocessor control (model2_tgp_state)
// ---------------------------------------------------------------------------------------------

void model2a::copro_ctl1_w(u32 data, u32 mem_mask)
{
	if ((data ^ m_coproctl) == 0x80000000) {
		if (data & 0x80000000) {
			m_coprocnt = 0;
			copro_halt();
			milestone(string_format("TGP upload start (pc %08x)", m_cpu.pip()));
		} else {
			m_tgp_upload_words = m_coprocnt;
			copro_boot();
			milestone(string_format("TGP upload end: %d words, TGP started (pc %08x)", m_coprocnt, m_cpu.pip()));
		}
	}
	COMBINE_DATA(&m_coproctl);
}

void model2a::copro_halt()
{
	set_input_line(L_TGP_HALT, ASSERT_LINE);
}

void model2a::copro_boot()
{
	set_input_line(L_TGP_HALT, CLEAR_LINE);
	// pulse_input_line(INPUT_LINE_RESET, attotime::zero)
	set_input_line(L_TGP_RESET, ASSERT_LINE);
	set_input_line(L_TGP_RESET, CLEAR_LINE);
}

void model2a::copro_fifo_w(u32 data)
{
	if (m_coproctl & 0x80000000) {
		m_tgp->prog[m_coprocnt & 0xfff] = data;
		++m_tgp->program_revision_id;
		m_coprocnt++;
	} else
		fifo_push(false, data);
}

u32 model2a::copro_fifo_r()
{
	const bool was_empty = m_fifo_out.values.empty();
	const u32 v = fifo_pop(true);
	if (on_copro&&(g_cooperative_fifo?!m_cooperative_read_waited:!was_empty)) on_copro('r', 0x4000, v);
	return v;
}

// ---------------------------------------------------------------------------------------------
// Palette and the 2D layers (model2_state::palette_w, screen_update without the polygons)
// ---------------------------------------------------------------------------------------------

void model2a::set_pen(int i)
{
	u16 palcolor = m_palram[i];
	u8 r = u8(m_colorxlat[(0x0080 >> 1) + (((palcolor >> 0) & 0x1f) << 8)]);
	u8 g = u8(m_colorxlat[(0x4080 >> 1) + (((palcolor >> 5) & 0x1f) << 8)]);
	u8 b = u8(m_colorxlat[(0x8080 >> 1) + (((palcolor >> 10) & 0x1f) << 8)]);
	// with MAME's alpha: a black pen is 0xff000000, not the transparent 0 of the layer bitmap
	m_pens[i] = 0xff000000 | (u32(m_gamma[r]) << 16) | (u32(m_gamma[g]) << 8) | m_gamma[b];
}

void model2a::palette_w(offs_t offset, u16 data, u16 mem_mask)
{
	ram_guard();
	const u16 v = (m_palram[offset] & ~mem_mask) | (data & mem_mask);
	if (v == m_palram[offset]) return;   // (the same value: the pen is as it was; a picture being drawn may be reading it)
	if (offset >= 0x1000) before_3d_ram_change(RAM_PAL, offset, m_palram[offset], v);   // (the polygons' colours)
	touch(P_PAL, offset * 2);
	m_palram[offset] = v;
	set_pen(int(offset));
}

// MAME's screen_update: the screen filled with pen 0, the tile layers' first pass over it
// (transparent pen 0), the 3D, the second pass. Both passes are drawn first, at the arcade's
// resolution, then the screen is put together row by row on every core, each tile pixel an
// s x s block: the same pixels as drawing each step over the whole screen in turn.
const bitmap_rgb32 &model2a::render(bool polygons)
{
	render_into(&m_screen.pix(0), size_t(m_screen.width()), polygons);
	return m_screen;
}

void model2a::render_into(u32 *out, size_t stride, bool polygons)
{
	render_wait();
	draw_picture(out, stride, polygons, false);
}

void model2a::render_async(u32 *out, size_t stride)
{
	render_wait();
	draw_picture(out, stride, true, true);
}

void model2a::render_smooth(float alpha, u32 *out, size_t stride, bool async)
{
	render_wait();
	draw_picture(out, stride, true, async, std::clamp(alpha, 0.0f, 1.0f));
}

bool model2a::render_parts(float alpha, u32 *px3d, size_t stride3d, bool async, picture_parts &parts, const gpu3d_hook *gpu, bool gpu_only, bool also_cpu, const gpu2d_hook *tiles, vblank_tiles early_tiles)
{
	render_wait();
	if (m_video->shows_framebuffer()) return false;
	if (gpu && gpu_only && m_ext && !wide_frame()) return false;   // (a 4:3 screen in a wide picture: its sides go by the 3D)
	parts.gpu3d = false;
	draw_picture(px3d, stride3d, true, async, alpha < 0 ? -1.0f : std::clamp(alpha, 0.0f, 1.0f), &parts, gpu, also_cpu, tiles, early_tiles);
	return true;
}

// The backdrop's scroll inputs: the stage backdrop's routine (0x24540, every frame while a stage
// is up) takes the camera's heading and pitch (words 0x26 and 0x24 of the camera at [0x500814]),
// two frames late (they pass through 0x50300e/0x50300c and 0x503012/0x503010), and sets
//   hscroll = -((-heading & 0xffff) >> 4) & 0x1ff        = ceil(heading / 16)
//   vscroll = (-((pitch - 736) >> 4) & 0x3ff) + 0x2000 + 8 * [0x50303e]   (bit 15 kept)
// (in the line-scroll stages, 7, 9 and 10, the same hscroll in each line's table, rippled on the
// water's lines). After frame f, [0x503012] and [0x503010] are what frame f + 1 scrolls by. A
// frame is "exact" when what it set is the sum of what it took.
void model2a::record_scroll_input(uint64_t f)
{
	const std::vector<u8> &w = m_workram;
	scroll_input &in = m_scroll_in[f % SCROLL_HIST];
	in.frame = int64_t(f);
	in.heading = m_next_heading;
	in.pitch = m_next_pitch;
	const u16 h = m_tiles.tile_r(0x5002), v = m_tiles.tile_r(0x5006);
	const int hx = ((int(in.heading) + 15) >> 4) & 0x1ff;
	const int q = int(in.pitch) - 736;
	const int vx = (-(q >> 4) + 8 * w[0x303e]) & 0x1ff;
	bool h_ok = (h & 0x8000) == 0 && (h & 0x1ff) == hx;
	if (h & 0x8000)   // (line scroll: the lines away from the ripple have the plain scroll)
		for (int y = 0; y < HEIGHT && !h_ok; y++) h_ok = (m_tiles.tile_r(0x4400 + y) & 0x1ff) == hx;
	in.exact = w[0x3001] != 0 && h_ok && (v & 0x1ff) == vx;
	m_next_heading = u16(w[0x3012] | w[0x3013] << 8);
	m_next_pitch = int16_t(w[0x3010] | w[0x3011] << 8);
}

int model2a::backdrop_move(double t, double &dh, double &dv) const
{
	dh = dv = 0;
	const int64_t newest = int64_t(m_frame_number) - 1;
	if (newest < 1) return 0;
	const double alpha = std::clamp(t - double(newest - 1), 0.0, 1.0);
	const scroll_input &b = m_scroll_in[newest % SCROLL_HIST], &a = m_scroll_in[(newest - 1) % SCROLL_HIST];
	if (b.frame == newest && b.exact) {
		// the heading and pitch for the moment, between the two frames' (not across a cut)
		int dhd = ((int(b.heading) - int(a.heading) + 32768) & 0xffff) - 32768, dp = int(b.pitch) - int(a.pitch);
		if (a.frame != newest - 1 || !a.exact || std::abs(dhd) > 16 * 24 || std::abs(dp) > 16 * 24) dhd = dp = 0;
		const double heading = b.heading - (1 - alpha) * dhd, pitch = b.pitch - (1 - alpha) * dp;
		dh = heading / 16 - double((int(b.heading) + 15) >> 4);
		dv = -(pitch - 736) / 16 - double(-((int(b.pitch) - 736) >> 4));
		return 1;
	}
	// else a line through the scroll it set: it moves in whole pixels a frame, so a pan is a
	// staircase; the line gives a smooth place (as vf1-recomp's sky). A jump (a cut) ends the
	// samples used.
	const scroll_sample &now = m_scroll_hist[newest % SCROLL_HIST];
	if (now.frame != newest) return 0;
	auto fit = [&](bool horizontal) -> double {
		const int mask = 512;
		const int v_now = (horizontal ? now.h : now.v) & 0x1ff;
		double xs[SCROLL_HIST], ys[SCROLL_HIST];
		int n = 0, prev = v_now;
		double acc = v_now;
		for (int64_t f = newest; f > newest - SCROLL_HIST && f >= 0; f--) {
			const scroll_sample &sm = m_scroll_hist[f % SCROLL_HIST];
			if (sm.frame != f) break;
			const int val = (horizontal ? sm.h : sm.v) & 0x1ff;
			int d = val - prev;
			d = ((d + mask / 2) % mask + mask) % mask - mask / 2;
			if (std::abs(d) > 24) break;
			acc += d;
			prev = val;
			if (f < t - 4) break;
			xs[n] = double(f) - t;
			ys[n] = acc;
			n++;
		}
		if (n < 2) return v_now;
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		for (int i = 0; i < n; i++) { sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; }
		const double den = n * sxx - sx * sx;
		const double slope = den > 1e-9 ? (n * sxy - sx * sy) / den : 0;
		return (sy - slope * sx) / n;
	};
	dh = fit(true) - (now.h & 0x1ff);
	dv = fit(false) - (now.v & 0x1ff);
	if (std::fabs(dh) > 24 || std::fabs(dv) > 24) { dh = dv = 0; return 0; }
	return 2;
}

// The backdrop's move in screen pixels at the scale now: from the game's sum, rounded as the game
// rounds, but to the screen's pixels (at 1x the game's own scroll, at any frame's own moment);
// from the line, to the nearest
void model2a::bd_pixels(int mode, double dh, double dv, int &dx, int &dy) const
{
	dx = dy = 0;
	const double s = m_scale;
	if (mode == 1) {
		dx = int(BACKDROP_SX * std::ceil(dh * s));
		dy = int(BACKDROP_SY * std::ceil(dv * s));
	} else if (mode == 2) {
		dx = int(std::lround(BACKDROP_SX * dh * s));
		dy = int(std::lround(BACKDROP_SY * dv * s));
	}
}

void model2a::render_wait()
{
	if (!m_job_active) return;
	const auto t0 = std::chrono::steady_clock::now();
	m_job->wait();
	m_job_active = false;
	m_job_reverts = false;
	m_job_waited += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

// Before the RAM the 3D is coloured from changes (texture RAM, the palette from 0x1000, colorxlat,
// lumaram): a picture being drawn finished, the deferred ones that can still be shown drawn.
// (the GPU renderer's count of each RAM's changes: gpu3d_rams)
void model2a::note_gpu_ram(int kind,u32 offset)
{
	switch (kind) {
	case RAM_XLAT: m_video->note_ram(model2_video::GPU_XLAT); break;
	case RAM_TEX0: m_video->note_ram(model2_video::GPU_TEX0,offset); break;
	case RAM_TEX1: m_video->note_ram(model2_video::GPU_TEX1,offset); break;
	case RAM_LUMA: m_video->note_ram(model2_video::GPU_LUMA); break;
	}
}

void model2a::before_3d_ram_change(int kind, u32 offset, u32 old, u32 now)
{
	render_wait();
	note_gpu_ram(kind,offset);
	// (the 3D still to be shown drawn now, in the RAMs as they are, unless the lists and the
	// changes are kept: then each is drawn when shown, the changes since taken back)
	if (m_video->deferred() && (!m_video->smooth() || m_ram_log.size() >= 1000000)) {
		const auto t0 = std::chrono::steady_clock::now();
		if (!m_video->smooth()) m_video->draw_deferred();
		else   // (the changes kept are about to be dropped: the pictures still to be shown drawn first, each in its frame's colours)
			for (int k = 0; k < 3; k++) {
				const int p = m_video->shown_picture(k);
				if (!m_video->picture_deferred(p)) continue;
				const uint64_t f = m_video->picture_frame(p);
				const bool back = !m_ram_log.empty() && m_ram_log.back().frame > f;
				if (back) ram_log_apply(f, true);
				m_video->draw_layer(p);
				if (back) ram_log_apply(f, false);
				m_video->picture_drawn(p);
			}
		m_stats.deferred_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		m_stats.deferred_n++;
	}
	m_stats.ram_changes++;
	// smooth motion: kept, so that an earlier frame's 3D can be drawn again in its own colours
	if (m_video->smooth()) {
		if (m_ram_log.size() >= 1000000) { m_ram_log.clear(); m_video->m_ram_changed = m_frame_number; }   // (too much: no smooth pictures of the frames before)
		m_ram_log.push_back({ m_frame_number, u8(kind), offset, old, now });
	}
}

// The 3D's RAMs as they were when frame `frame` ended (undo), or as they are (redo): the changes
// made in the frames after it, taken back in reverse or made again in order.
void model2a::ram_log_apply(uint64_t frame, bool undo)
{
	auto put = [&](const ram_change &c, u32 v) {
		note_gpu_ram(c.kind,c.offset);
		switch (c.kind) {
		case RAM_XLAT:
            m_colorxlat[c.offset] = u16(v);
            if(g_cache_2d_pens&&(c.offset&0xff)==0x40)m_palette_dirty=true;
            break;
		case RAM_TEX0: m_tex0[c.offset] = v; break;
		case RAM_TEX1: m_tex1[c.offset] = v; break;
		case RAM_LUMA: m_lumaram[c.offset] = u8(v); break;
		case RAM_PAL:
            m_palram[c.offset] = u16(v);
            if(g_cache_2d_pens&&c.offset<0x1000)m_palette_dirty=true;
            break;
		}
	};
	if (undo) {
		for (auto it = m_ram_log.rbegin(); it != m_ram_log.rend() && it->frame > frame; ++it) put(*it, it->old);
	} else {
		auto it = m_ram_log.begin();
		while (it != m_ram_log.end() && it->frame <= frame) ++it;
		for (; it != m_ram_log.end(); ++it) put(*it, it->now);
	}
}

struct model2a::picture_desc
{
	std::shared_ptr<frame_2d> state;   // the 2D's frame (and its state, once kept)
	bool polygons = true, framebuffer = false;
	model2_video::moment mo;           // the 3D's lists and moment (b null: none)
	int bd_mode = 0;                   // the backdrop's move (backdrop_move)
	double bd_h = 0, bd_v = 0;
};

// Before the game runs on: the 2D state of the frame a picture still around was drawn from, kept
// with it (a picture held by scene sync, drawn again at another size, needs the 2D it showed).
// The tile graphics are copied again only when they've changed.
void model2a::keep_2d()
{
	if (!m_cur_2d) return;
	if (m_cur_2d.use_count() > 1 && !m_cur_2d->kept) {
		ram_guard();   // (a picture being drawn may have the palette and colorxlat taken back)
		frame_2d &f = *m_cur_2d;
		f.tile_ram = m_tiles.tile_ram;
		f.palram = m_palram;
		f.colorxlat = m_colorxlat;
		if (!m_char_copy || m_char_copy_version != m_tiles.char_version) {
			m_char_copy = std::make_shared<const std::vector<u16>>(m_tiles.char_ram);
			m_char_copy_version = m_tiles.char_version;
		}
		f.char_ram = m_char_copy;
		f.kept = true;
	}
	m_cur_2d.reset();
}

void model2a::swap_2d(frame_2d &f)
{
	std::swap(m_tiles.tile_ram, f.tile_ram);
	std::swap(m_palram, f.palram);
	std::swap(m_colorxlat, f.colorxlat);
	auto chars = std::make_shared<std::vector<u16>>(*f.char_ram);
	std::swap(m_tiles.char_ram, *chars);
	f.char_ram = chars;
	m_tiles.mark_dirty();
	m_palette_dirty = true;
}

// The picture `pic` again, at the scale and width now, from what was kept: nothing of the game
// runs. Its 2D drawn from its frame's state, its 3D from its lists at its moment in its frame's
// colours, the backdrop moved as it was.
void model2a::redraw_into(const picture_desc &pic, u32 *out, size_t stride)
{
	render_wait();
	const bool live = !pic.state || pic.state->frame == m_frame_number;
	if (pic.framebuffer || (!live && !pic.state->kept)) {   // (nothing kept to draw it from: as it is now)
		draw_picture(out, stride, pic.polygons, false);
		return;
	}
	const int s = m_scale, e = m_ext, w = WIDTH + 2 * e;
	frame_2d &st = *pic.state;
	if (!live) swap_2d(st);
	freecam_sky_move();
	draw_2d(pic.polygons, false);
	m_2d_key[0] = ~0ull;
	const u32 pen0 = m_pens[0];
	if (!live) swap_2d(st);   // (the live state back; its colours are worked out again at the next picture)
	const bool wide = m_2d_wide, smooth = m_2d_filter;
	const int x0 = wide ? 0 : e, x1 = wide ? w : e + WIDTH;
	const bitmap_rgb32 *layer3d = nullptr;
	if (pic.polygons && pic.mo.b) {
		const int p = m_video->replay_layer(pic.mo, layer3d);
		const uint64_t f = pic.mo.b->frame;
		const bool revert = !m_ram_log.empty() && m_ram_log.back().frame > f;
		if (revert) ram_log_apply(f, true);
		if (p >= 0) m_video->draw_layer(p);
		if (revert) ram_log_apply(f, false);
	}
	int bdx = 0, bdy = 0;
	bd_pixels(pic.bd_mode, pic.bd_h, pic.bd_v, bdx, bdy);
	bdx += m_fc_fx; bdy += m_fc_fy;   // (the free camera's: the rest of its move of the backdrop)
	compose(out, stride, layer3d, x0, x1, smooth, pen0, bdx, bdy);
	if (x0) {   // (the sides go by the frame's 2D)
		if (!live) swap_2d(st);
		sides_into(out, stride);
		if (!live) swap_2d(st);
	}
}

void model2a::draw_picture(u32 *out, size_t stride, bool polygons, bool async, float alpha, picture_parts *parts, const gpu3d_hook *gpu, bool also_cpu, const gpu2d_hook *tiles, vblank_tiles early_tiles)
{
	const int s = m_scale, e = m_ext, w = WIDTH + 2 * e;
	// smooth motion: the 2D drawn once a game frame, its in-between pictures reusing it
	const bool smoothly = alpha >= 0 && polygons && m_video->smooth() && !m_video->shows_framebuffer();
	freecam_sky_move();
	const uint64_t key[] = { m_frame_number, uint64_t(s), uint64_t(e), uint64_t(m_backdrop_smooth), uint64_t(parts != nullptr),
		uint64_t(uint32_t(m_fc_dx)) << 32 | uint32_t(m_fc_dy) | uint64_t(m_video->freecam_on()) << 31 };
	if (!smoothly || memcmp(key, m_2d_key, sizeof key)) {
		draw_2d(polygons, parts != nullptr, tiles, early_tiles);
		memcpy(m_2d_key, key, sizeof key);
		if (!smoothly) m_2d_key[0] = ~0ull;
	}
	const bool wide = m_2d_wide, smooth = m_2d_filter;
	const bool framebuffer = polygons && m_video->shows_framebuffer();
	// a 4:3 screen in a wide picture: black at the sides
	const int x0 = wide ? 0 : e, x1 = wide ? w : e + WIDTH;
	// smooth motion's 3D (the moment `alpha` of the way from the frame before the newest), and
	// the backdrop moved to that moment
	const bitmap_rgb32 *smooth3d = nullptr;
	int smooth_pic = -1, bdx = 0, bdy = 0;
	if (smoothly) smooth_pic = m_video->smooth_layer(m_frame_number - 1, alpha, smooth3d);
	else if (polygons && !framebuffer && m_video->freecam_on() && m_video->smooth())   // (the free camera's: every picture set up again from its list)
		smooth_pic = m_video->smooth_layer(m_frame_number - 1, 1.0f, smooth3d);
	// (with the lists kept, the host's: the backdrop at its unrounded place for the moment)
	int bd_mode = 0;
	double bd_h = 0, bd_v = 0;
	if (m_video->smooth() && polygons && !framebuffer) {
		bd_mode = backdrop_move(double(m_frame_number) - 2 + (smoothly ? alpha : 1.0), bd_h, bd_v);
		bd_pixels(bd_mode, bd_h, bd_v, bdx, bdy);
	}
	bdx += m_fc_fx; bdy += m_fc_fy;   // (the free camera's: the rest of its move of the backdrop)
	// what's needed to draw this picture again (redraw)
	if (m_video->smooth()) {
		auto pic = std::make_shared<picture_desc>();
		if (!m_cur_2d || m_cur_2d->frame != m_frame_number) { m_cur_2d = std::make_shared<frame_2d>(); m_cur_2d->frame = m_frame_number; }
		pic->state = m_cur_2d;
		pic->polygons = polygons;
		pic->framebuffer = framebuffer;
		if (smooth_pic >= 0) pic->mo = m_video->m_last_moment;
		else if (polygons && !framebuffer) pic->mo.b = m_video->shown_list();
		pic->bd_mode = bd_mode; pic->bd_h = bd_h; pic->bd_v = bd_v;
		m_last_pic = pic;
		m_moment3d = !pic->mo.b ? -1 : pic->mo.a ? double(pic->mo.b->frame) - 1 + pic->mo.alpha : double(pic->mo.b->frame);
	}
	// a smooth picture of an earlier frame's 3D (scene sync's delay) is drawn in that frame's
	// colours: the 3D's RAMs taken back meanwhile
	// the 3D to draw: smooth motion's moment, or the frame's own picture if it hasn't been drawn yet
	// (deferred). With the lists kept, a RAM change no longer draws the deferred pictures there and
	// then (before_3d_ram_change): each is drawn when shown, in its own frame's colours, the RAMs
	// taken back meanwhile, as smooth motion's are.
	// (the GPU's way: the picture shown isn't marked drawn, its map left as it was, unless the CPU
	// draws it too)
	const bool gpu_way = parts && gpu && !x0 && !framebuffer;
	if (parts) {
		parts->w = w; parts->s = s;
		parts->version = m_2d_version;
		parts->gpu2d = m_gpu2d_active;
		parts->backdrop = m_gpu2d_active ? nullptr : m_bd_pad.data();
		parts->mid = m_gpu2d_active ? nullptr : &m_sys24_mid.pix(0);
		parts->top = m_gpu2d_active ? nullptr : &m_sys24_top.pix(0);
		parts->filter = smooth;
		parts->bdx = bdx; parts->bdy = bdy;
		parts->sides = 0;
		parts->side_px = nullptr;
	}
	if (gpu_way) {   // the 3D handed to the GPU, in its own frame's colours
		const int gp = smooth_pic >= 0 ? smooth_pic : polygons ? m_video->shown_index() : -1;
		bool ok = true;
		if (gp >= 0) {
			const uint64_t f = smooth_pic >= 0 ? m_video->m_smooth_fb : m_video->picture_frame(gp);
			const bool back = m_video->smooth() && !m_ram_log.empty() && m_ram_log.back().frame > f;
			if (back) ram_log_apply(f, true);
			ok = (*gpu)(*m_video, gp);
			if (back) ram_log_apply(f, false);
		}
		if (ok) {
			parts->gpu3d = true;
			parts->has3d = gp >= 0;
			parts->map3d = nullptr;
			parts->map_stride = 0;
			parts->rows3d[0] = 0; parts->rows3d[1] = HEIGHT * s;
			if (!also_cpu) return;
		}
	}
	const bitmap_rgb32 *layer3d0 = smooth3d;
	const int pic3d = smooth_pic >= 0 ? smooth_pic : polygons && !framebuffer ? m_video->layer_to_draw(layer3d0) : -1;
	const uint64_t list_frame = smooth_pic >= 0 ? m_video->m_smooth_fb : pic3d >= 0 ? m_video->picture_frame(pic3d) : 0;
	const bool revert = pic3d >= 0 && m_video->smooth() && !m_ram_log.empty() && m_ram_log.back().frame > list_frame;
	if (parts) {   // the parts: the 2D as drawn, the 3D copied into `out` (on the job's thread, or now)
		const bitmap_rgb32 *layer3d = layer3d0;
		const int p = pic3d;
		const bool gpu_drew = parts->gpu3d;   // (the CPU's too, for checking: into out, the GPU's parts kept)
		if (!gpu_drew) {
			parts->has3d = layer3d != nullptr;
			parts->map3d = layer3d ? &layer3d->pix(0) : nullptr;
			parts->map_stride = layer3d ? layer3d->rowpixels() : 0;
		}
		const int W = w * s, H = HEIGHT * s;
		// smooth motion's picture, drawn again each time it's used, straight into px3d
		const bool direct = out && p >= 0 && p == smooth_pic && !x0;   // (a 4:3 screen's sides go by its edges: from the map)
		if (direct && !gpu_drew) parts->map3d = nullptr;
		auto draw3d = [this, out, stride, layer3d, p, revert, list_frame, W, parts, direct, x0, gpu_drew] {
			int r0 = 0, r1 = 0;
			if (layer3d) m_video->drawn_rows(layer3d, r0, r1);
			if (gpu_drew) { r0 = 0; r1 = HEIGHT * m_scale; if (out) for (int y = 0; y < r1; y++) memset(out + size_t(y) * stride, 0, size_t(W) * 4); }
			else { parts->rows3d[0] = r0; parts->rows3d[1] = r1; }
			if (revert) ram_log_apply(list_frame, true);
			if (direct) m_video->draw_layer_into(p, out, stride, r0, r1);
			else if (p >= 0) m_video->draw_layer(p);
			if (revert) ram_log_apply(list_frame, false);
			if (layer3d && !direct && out)
				parallel_rows(r1 - r0, [&](int y0, int y1) {
					for (int y = r0 + y0; y < r0 + y1; y++) memcpy(out + size_t(y) * stride, &layer3d->pix(y), size_t(W) * 4);
				});
			if (layer3d && out && m_edge_aa && r1 > r0 && !x0) {
				// Low: the 3D's edges smoothed, the rows beside it too (cleared first: px3d's other
				// rows are left as they were), the pixels beside the 3D taking the picture beneath
				const int a = std::max(0, r0 - 1), b = std::min(HEIGHT * m_scale, r1 + 1);
				if (a < r0) memset(out + size_t(a) * stride, 0, size_t(W) * 4);
				if (b > r1) memset(out + size_t(r1) * stride, 0, size_t(W) * 4);
				edge_smooth(out, stride, nullptr, 0, W / m_scale, a, b, parts->filter, m_pens[0], parts->bdx, parts->bdy);
				if (!gpu_drew) { parts->rows3d[0] = a; parts->rows3d[1] = b; }
			}
		};
		if (async && !x0 && !gpu_drew) {
			if (!m_job) m_job = std::make_unique<background_job>();
			m_job_active = true;
			m_job_reverts = revert;
			m_job->start([this, draw3d] {
				const auto t0 = std::chrono::steady_clock::now();
				draw3d();
				m_job_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			});
			return;
		}
		draw3d();
		if (x0) {   // the sides go by the picture's columns beside them: as compose() puts them together
			const u32 pen0 = m_pens[0];
			u32 edge[2 * HEIGHT];
			for (int side = 0; side < 2; side++) {
				const int X = side ? (e + WIDTH) * s - 1 : e * s, x = X / s;
				for (int y = 0; y < HEIGHT; y++) {
					const int Y = y * s;
					u32 c = m_sys24_top.pix(y, x);
					if (!c && layer3d) c = layer3d->pix(Y, X);
					if (!c) c = m_sys24_mid.pix(y, x);
					if (!c) {
						c = bdx || bdy ? m_sys24.pix(std::clamp(Y + bdy, 0, H - 1) / s, std::clamp(X + bdx, 0, W - 1) / s) : m_sys24.pix(y, x);
						if (!c) c = pen0;
					}
					edge[side * HEIGHT + y] = c;
				}
			}
			const size_t sw = size_t(2) * e * s;
			if (m_side_px.size() != sw * H) m_side_px.assign(sw * H, 0);
			parts->sides = fill_sides(m_side_px.data(), sw, edge, WIDTH * s, parts->side_colour);
			if (parts->sides == 2) parts->side_px = m_side_px.data();
		}
		return;
	}
	if (async && !framebuffer && !x0) {   // the 3D and the putting together on the job's thread
		const bitmap_rgb32 *layer3d = layer3d0;
		const int p = pic3d;
		const u32 pen0 = m_pens[0];
		if (!m_job) m_job = std::make_unique<background_job>();
		m_job_active = true;
		m_job_reverts = revert;
		m_job->start([this, out, stride, layer3d, p, x0, x1, smooth, pen0, bdx, bdy, revert, list_frame] {
			const auto t0 = std::chrono::steady_clock::now();
			if (revert) ram_log_apply(list_frame, true);
			if (p >= 0) m_video->draw_layer(p);
			if (revert) ram_log_apply(list_frame, false);
			compose(out, stride, layer3d, x0, x1, smooth, pen0, bdx, bdy);
			m_job_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		});
		return;
	}
	// the 3D: the renderer's picture (transparent pen 0), or in render test mode the framebuffer,
	// which composite() draws between the passes itself
	const bitmap_rgb32 *layer3d = layer3d0;
	if (pic3d >= 0) {
		if (revert) ram_log_apply(list_frame, true);
		m_video->draw_layer(pic3d);
		if (revert) ram_log_apply(list_frame, false);
	}
	constexpr u32 BLACK = 0xff000000;
	auto put_tiles = [&](const bitmap_rgb32 &src, bool over_pen0) {
		parallel_rows(HEIGHT, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++) {
				const u32 *t = &src.pix(y);
				for (int dy = 0; dy < s; dy++) {
					u32 *d = &m_screen.pix(y * s + dy);
					if (over_pen0 && x0) { std::fill(d, d + x0 * s, BLACK); std::fill(d + x1 * s, d + w * s, BLACK); }   // (render test mode: black)
					for (int x = x0; x < x1; x++)
						if (const u32 p = t[x] ? t[x] : over_pen0 ? m_pens[0] : 0)
							for (int dx = 0; dx < s; dx++) d[x * s + dx] = p;
				}
			}
		});
	};
	if (framebuffer) {   // (on the screen bitmap, then copied out)
		put_tiles(m_sys24, true);
		put_tiles(m_sys24_mid, false);
		m_video->composite(m_screen, rectangle(e * s, (e + WIDTH) * s - 1, 0, HEIGHT * s - 1));
		put_tiles(m_sys24_top, false);
		if (out != &m_screen.pix(0))
			for (int y = 0; y < HEIGHT * s; y++) memcpy(out + y * stride, &m_screen.pix(y), size_t(w * s) * 4);
		return;
	}
	compose(out, stride, layer3d, x0, x1, smooth, m_pens[0], bdx, bdy);
	if (x0) sides_into(out, stride);
}

// The 2D at the arcade's resolution: m_sys24, the backdrop (maps 2 and 3, opaque); m_sys24_mid,
// the layers over it under the 3D; m_sys24_top, the layers over the 3D. With the background
// filter the backdrop softened too; for render_parts the backdrop padded.
void model2a::prepare_tile_planes()
{
	if (m_palette_dirty) {
		for (int i = 0; i < 0x1000; i++) set_pen(i);
        if(g_cache_2d_pens)m_palette_dirty=false;
    }
    static const bool check_pens=getenv("VF2_HASH_2D_PENS")!=nullptr;
    if(check_pens){
        const auto *bytes=reinterpret_cast<const u8 *>(m_pens.data());
        const auto hash=m2::state_hash(bytes,0x1000*sizeof(m_pens[0]));
        std::fprintf(stderr,"System 24 pens: %llu %016llx\n",(unsigned long long)m_frame_number,(unsigned long long)hash);
    }
	m_tiles.update(m_pens.data());
}

bool model2a::custom_hud_names()
{
    std::lock_guard<std::mutex> lock(m_hud_mx);
    return !m_hud_names[0].empty()||!m_hud_names[1].empty()||!m_random_hud[0].empty()||!m_random_hud[1].empty();
}

void model2a::build_gpu_tile_frame(bool hud_off,gpu2d_frame &frame)
{
    for(auto &pass:frame.passes)pass.clear();
    m_tiles.gpu_data(frame);
    for(int layer=3;layer>=2;layer--)m_tiles.gpu_draw(frame.passes[0],m_sys24,layer<<1,segas24_tile::DRAW_OPAQUE);
    for(int layer=1;layer>=0;layer--)m_tiles.gpu_draw(frame.passes[1],m_sys24_mid,layer<<1,0);
    for(int layer=3;layer>=(hud_off?2:0);layer--)m_tiles.gpu_draw(frame.passes[2],m_sys24_top,(layer<<1)|1,0);
}

void model2a::publish_gpu_tiles()
{
    m_gpu2d_active=true;
    m_2d_wide=false;
    m_2d_filter=false;
    ++m_2d_version;
}

bool model2a::prepare_vblank_tiles(gpu2d_frame &frame)
{
    // At VBLANK end the guest has finished updating these tile inputs.
    // The descriptor borrows board storage until the next simulation begins.
    if(m_scale!=1||m_ext||m_render_test_mode||m_video->smooth()||m_video->freecam_on()||
       m_random||!m_rank_names.empty()||custom_hud_names())return false;
    prepare_tile_planes();
    build_gpu_tile_frame(m_hud_hidden&&game_mode()==9,frame);
    return true;
}

void model2a::draw_2d(bool polygons, bool parts, const gpu2d_hook *tiles, vblank_tiles early_tiles)
{
    if(early_tiles==vblank_tiles::staged){
        publish_gpu_tiles();
        return;
    }
    m_gpu2d_active=false;
    // A declined stage can retry from the same planes and dirty entries.
    // A second update here would discard that frame's dirty list.
    if(early_tiles==vblank_tiles::unavailable)prepare_tile_planes();
	const int s = m_scale, e = m_ext, w = WIDTH + 2 * e;
	const bool wide = wide_frame();
	const rectangle clip(0, WIDTH - 1, 0, HEIGHT - 1);
	// the 2D at the arcade's resolution, its column 0 at e; in a wide frame the backdrop's maps
	// (2 and 3) carry on to the sides, from the stage's panorama where it's known
	const bool fc = polygons && m_video->freecam_on() && freecam_scene();   // (the free camera: no HUD, the backdrop moved)
	const segas24_tile::panorama *pan = wide || fc ? find_panorama() : nullptr;
	// the three bitmaps at once, each on a thread (the tiles' planes are only read)
	const bool hud_off = (m_hud_hidden && game_mode() == 9) || fc;   // (Display > HUD Off, in a fight: maps 0 and 1 not drawn over the 3D)

    if(tiles&&parts&&s==1&&e==0&&!wide&&!fc&&!m_video->smooth()&&!custom_hud_names()){
        gpu2d_frame frame;
        build_gpu_tile_frame(hud_off,frame);
        if((*tiles)(frame)){
            publish_gpu_tiles();
            return;
        }
    }
	m_tiles.m_ox = e;
	parallel_tasks(3, [&](int task) {
		const rectangle all(0, w - 1, 0, HEIGHT - 1);
		if (task == 0) {
			m_sys24.fill(0, all);
			for (int layer = 3; layer >= 2; layer--) {
				if (fc && m_tiles.can_shift(layer << 1)) { m_tiles.draw_shifted(m_sys24, layer << 1, segas24_tile::DRAW_OPAQUE, wide ? e : 0, pan, m_fc_dx, m_fc_dy); continue; }
				m_tiles.draw(m_sys24, clip, layer << 1, 0, segas24_tile::DRAW_OPAQUE);
				if (wide) m_tiles.draw_sides(m_sys24, layer << 1, segas24_tile::DRAW_OPAQUE, e, pan);
			}
		} else if (task == 1) {
			m_sys24_mid.fill(0, all);
			for (int layer = 1; layer >= 0 && !fc; layer--)
				m_tiles.draw(m_sys24_mid, clip, layer << 1, 0, 0);
		} else {
			m_sys24_top.fill(0, all);
			for (int layer = 3; layer >= (hud_off ? 2 : 0); layer--) {
				if (fc && m_tiles.can_shift(layer << 1)) { m_tiles.draw_shifted(m_sys24_top, (layer << 1) | 1, 0, wide ? e : 0, pan, m_fc_dx, m_fc_dy); continue; }
				m_tiles.draw(m_sys24_top, clip, (layer << 1) | 1, 0, 0);
				if (wide && layer >= 2) m_tiles.draw_sides(m_sys24_top, (layer << 1) | 1, 0, e, pan);
			}
		}
	});
	m_tiles.m_ox = 0;
	{   // (online play: the players' names in place of their fighters', in the picture only; offline, a random fighter's)
		std::lock_guard<std::mutex> lk(m_hud_mx);
		const bool online = !m_hud_names[0].empty() || !m_hud_names[1].empty();
		const std::string *names = online ? m_hud_names : m_random_hud;
		if (!hud_off && (!names[0].empty() || !names[1].empty())) {
			const m2hud::Flag *fl[2] = { &m_hud_flags[0], &m_hud_flags[1] };
			if (!m2hud::draw(m_tiles, m_sys24_top, e, names, online ? fl : nullptr, m_hud_flag_at))
				for (auto &a : m_hud_flag_at) a[0] = a[1] = -1;
		} else
			for (auto &a : m_hud_flag_at) a[0] = a[1] = -1;
	}
	const bool smooth = m_backdrop_smooth && s > 1 && wide && !(polygons && m_video->shows_framebuffer());
	if (smooth) backdrop_smooth(w, !parts);
	m_2d_wide = wide;
	m_2d_filter = smooth;
	m_2d_version++;
	if (parts) {   // the backdrop PAD pixels larger each way, its edges carried on; opaque
		constexpr int P = picture_parts::PAD;
		const int pw = w + 2 * P;
		m_bd_pad.resize(size_t(pw) * (HEIGHT + 2 * P));
		const u32 pen0 = m_pens[0];
		parallel_rows(HEIGHT + 2 * P, [&](int y0, int y1) {
			for (int py = y0; py < y1; py++) {
				const int y = std::clamp(py - P, 0, HEIGHT - 1);
				u32 *d = &m_bd_pad[size_t(py) * pw];
				if (smooth) {
					const u32 *r = &m_bd_soft[size_t(y) * w];
					for (int px = 0; px < pw; px++) d[px] = 0xff000000 | r[std::clamp(px - P, 0, w - 1)];
				} else {
					const u32 *r = &m_sys24.pix(y);
					for (int px = 0; px < pw; px++) { const u32 c = r[std::clamp(px - P, 0, w - 1)]; d[px] = c ? c : pen0; }
				}
			}
		});
	}
}

// The 2D's passes (drawn) and the 3D put together into `out`, row by row on every core. bdx, bdy:
// smooth motion's move of the backdrop (screen pixels: where the backdrop shows, it comes from
// that far along in the backdrop as drawn). The backdrop shows where the layers over it are
// transparent.
void model2a::compose(u32 *out, size_t stride, const bitmap_rgb32 *layer3d, int x0, int x1, bool smooth, u32 pen0, int bdx, int bdy)
{
	const int s = m_scale, w = WIDTH + 2 * m_ext, W = w * s, H = HEIGHT * s;
	const bool aa = m_edge_aa && layer3d && !x0;   // (Low: the layers over the 3D put on after its edges are smoothed; not a 4:3 screen in a wide picture)
	const bool moved = bdx || bdy;
	parallel_rows(HEIGHT, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const u32 *bd = &m_sys24.pix(y), *mid = &m_sys24_mid.pix(y), *over = &m_sys24_top.pix(y);
			for (int dy = 0; dy < s; dy++) {
				u32 *d = out + size_t(y * s + dy) * stride;
				if (moved && !smooth) {   // the backdrop moved: each screen pixel of it from where it was
					const u32 *src = &m_sys24.pix(std::clamp(y * s + dy + bdy, 0, H - 1) / s);
					for (int x = x0; x < x1; x++) {
						if (!mid[x]) {
							for (int X = x * s; X < x * s + s; X++) {
								const u32 c = src[std::clamp(X + bdx, 0, W - 1) / s];
								d[X] = c ? c : pen0;
							}
						} else
							for (int dx = 0; dx < s; dx++) d[x * s + dx] = mid[x];
					}
				} else if (smooth) {
					// the backdrop smoothly enlarged, between the two rows around this one's middle,
					// where nothing was drawn over it; the rest in blocks
					const int Y = std::clamp(y * s + dy + bdy, 0, H - 1), num = 2 * Y + 1 - s;   // the source row: (Y + 0.5) / s - 0.5
					const int ya = num < 0 ? 0 : std::min(num / (2 * s), HEIGHT - 1), yb = std::min(ya + 1, HEIGHT - 1);
					const u32 fy = num < 0 ? 0 : u32((num - ya * 2 * s) * 256 / (2 * s));
					const u32 *ra = &m_bd_wide[size_t(ya) * w * s], *rb = &m_bd_wide[size_t(yb) * w * s];
					for (int x = x0; x < x1; x++) {
						if (!mid[x]) {
							for (int X = x * s; X < x * s + s; X++) {
								const int Xb = std::clamp(X + bdx, 0, W - 1);
								const u32 a = ra[Xb], b = rb[Xb];
								d[X] = 0xff000000 | ((((a & 0xff00ff) * (256 - fy) + (b & 0xff00ff) * fy) >> 8) & 0xff00ff) |
									   ((((a & 0xff00) * (256 - fy) + (b & 0xff00) * fy) >> 8) & 0xff00);
							}
						} else
							for (int dx = 0; dx < s; dx++) d[x * s + dx] = mid[x];
					}
				} else
					for (int x = x0; x < x1; x++) {
						const u32 p = mid[x] ? mid[x] : bd[x] ? bd[x] : pen0;
						for (int dx = 0; dx < s; dx++) d[x * s + dx] = p;
					}
				if (layer3d) {
					const u32 *t = &layer3d->pix(y * s + dy);
					for (int x = x0 * s; x < x1 * s; x++)
						if (t[x] != 0x00000000) d[x] = t[x];
				}
				if (!aa)
					for (int x = x0; x < x1; x++)
						if (const u32 p = over[x])
							for (int dx = 0; dx < s; dx++) d[x * s + dx] = p;
			}
		}
	});
	if (!aa) return;
	// Low: the 3D's edges smoothed on the picture beneath it, then the layers over it
	edge_smooth(out, stride, layer3d, x0, x1, 0, H, smooth, pen0, bdx, bdy);
	parallel_rows(HEIGHT, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const u32 *over = &m_sys24_top.pix(y);
			for (int dy = 0; dy < s; dy++) {
				u32 *d = out + size_t(y * s + dy) * stride;
				for (int x = x0; x < x1; x++)
					if (const u32 p = over[x])
						for (int dx = 0; dx < s; dx++) d[x * s + dx] = p;
			}
		}
	});
}

// The picture beneath the 3D at a pixel of the render scale (the backdrop and the layers over it
// under the 3D), as compose() puts it there
u32 model2a::below_3d(int X, int Y, bool smooth, u32 pen0, int bdx, int bdy) const
{
	const int s = m_scale, w = WIDTH + 2 * m_ext, W = w * s, H = HEIGHT * s;
	const int x = X / s, y = Y / s;
	if (const u32 m = m_sys24_mid.pix(y, x)) return m;
	if (smooth) {
		// (the softened backdrop enlarged with straight lines between its pixels' middles, as the
		// GPU's linear sampling does it in parts: the same function drawn whole and in parts, so
		// the blends are the same, though the backdrop shown beside them can differ by a step)
		const float u = (X + bdx + 0.5f) / s - 0.5f, v = (Y + bdy + 0.5f) / s - 0.5f;
		const int ua = std::clamp(int(std::floor(u)), 0, w - 1), va = std::clamp(int(std::floor(v)), 0, HEIGHT - 1);
		const int ub = std::clamp(int(std::floor(u)) + 1, 0, w - 1), vb = std::clamp(int(std::floor(v)) + 1, 0, HEIGHT - 1);
		const float fu = u - std::floor(u), fv = v - std::floor(v);
		const u32 c00 = m_bd_soft[size_t(va) * w + ua], c10 = m_bd_soft[size_t(va) * w + ub], c01 = m_bd_soft[size_t(vb) * w + ua], c11 = m_bd_soft[size_t(vb) * w + ub];
		u32 out = 0xff000000;
		for (int sh = 0; sh < 24; sh += 8) {
			const float t = ((c00 >> sh) & 255) * (1 - fu) + ((c10 >> sh) & 255) * fu, b = ((c01 >> sh) & 255) * (1 - fu) + ((c11 >> sh) & 255) * fu;
			out |= u32(std::lround(t * (1 - fv) + b * fv)) << sh;
		}
		return out;
	}
	if (bdx || bdy) {
		const u32 c = m_sys24.pix(std::clamp(Y + bdy, 0, H - 1) / s, std::clamp(X + bdx, 0, W - 1) / s);
		return c ? c : pen0;
	}
	const u32 c = m_sys24.pix(y, x);
	return c ? c : pen0;
}

// Low antialiasing on px (rows r0 to r1, columns of the screen x0 to x1 in arcade pixels), the 3D
// known by layer3d's pixels (drawn whole: px is the picture beneath with the 3D over it) or, with
// no layer3d, by px's own (in parts: px is the 3D alone, 0 where it drew nothing). Where a pixel
// stands out from its four neighbours it is blended toward the one across the edge, by how much it
// stands out, up to 3/8 of the way (vf1-recomp's edge_aa.cpp); only the 3D's pixels and those
// beside it, a pixel beside it taking the picture beneath as its colour. The blends are kept aside
// and written once all is read; in parts they go into px opaque, so the GPU's picture is this one.
void model2a::edge_smooth(u32 *px, size_t stride, const bitmap_rgb32 *layer3d, int x0, int x1, int r0, int r1, bool smooth, u32 pen0, int bdx, int bdy)
{
	const int s = m_scale, H = HEIGHT * s, X0 = x0 * s, X1 = x1 * s;
	const bool parts = layer3d == nullptr;
	const int va = r0, vb = r1;   // (in parts, px's rows outside these are someone else's: nothing in 3D there)
	r0 = std::max(r0, 1); r1 = std::min(r1, H - 1);
	if (r1 <= r0 || X1 - X0 < 3) return;
	static const std::vector<u32> none(size_t(8 * 4096), 0);   // a row of nothing (up to 8x of 80:9: 8 x 3,306)
	auto row = [&](int y) -> const u32 * { return !parts || (y >= va && y < vb) ? px + size_t(y) * stride : none.data(); };
	auto drawn = [&](int x, int y) { return parts ? row(y)[x] != 0 : layer3d->pix(y, x) != 0; };
	auto colour = [&](int x, int y) -> u32 {   // (beside the 3D: the picture beneath, by the one function either way)
		if (!drawn(x, y)) return below_3d(x, y, smooth, pen0, bdx, bdy) & 0xffffff;
		return row(y)[x] & 0xffffff;
	};
	auto luma = [](u32 c) { return int((c >> 16) & 0xff) * 77 + int((c >> 8) & 0xff) * 150 + int(c & 0xff) * 29; };   // (x 256)
	constexpr int THRESHOLD = 32 * 256, BANDS = 64;   // less contrast than this isn't an edge
	if (m_aa_changes.size() != BANDS) m_aa_changes.resize(BANDS);
	for (auto &c : m_aa_changes) c.clear();
	const int rows = r1 - r0, per_band = (rows + BANDS - 1) / BANDS;
	// each row's brightness once (x 256; -1: no 3D there, the picture beneath, worked out only
	// when a blend needs it), three rows at a time
	auto luma_row = [&](int y, std::vector<int> &L) {
		const u32 *r = row(y);
		const u32 *t = parts ? r : &layer3d->pix(y);
		for (int x = X0; x < X1; x++) L[x] = t[x] ? luma(r[x] & 0xffffff) : -1;
	};
	parallel_rows(BANDS, [&](int ba, int bb) {
		thread_local std::vector<int> L0, L1, L2;
		for (int band = ba; band < bb; band++) {
			auto &changes = m_aa_changes[band];
			const int ya = r0 + band * per_band, yb = std::min(r0 + (band + 1) * per_band, r1);
			if (ya >= yb) continue;
			L0.assign(X1 + 1, -1); L1.assign(X1 + 1, -1); L2.assign(X1 + 1, -1);
			luma_row(ya - 1, L0); luma_row(ya, L1);
			for (int y = ya; y < yb; y++) {
				luma_row(y + 1, L2);
				const int *Ln = L0.data(), *Lm = L1.data(), *Ls = L2.data();
				for (int x = std::max(X0, 1); x < X1 - 1; x++) {
					int lm = Lm[x], ln = Ln[x], ls = Ls[x], lw = Lm[x - 1], le = Lm[x + 1];
					if ((lm & ln & ls & lw & le) < 0) continue;   // (no 3D here or beside it)
					u32 cm, cn, cs, cw, ce;
					if ((lm | ln | ls | lw | le) >= 0) {   // inside the 3D, the most of them: as they are
						const int lo = std::min({ lm, ln, ls, lw, le }), hi = std::max({ lm, ln, ls, lw, le });
						if (hi - lo < THRESHOLD) continue;
						const u32 *rm = row(y);
						cm = rm[x] & 0xffffff; cw = rm[x - 1] & 0xffffff; ce = rm[x + 1] & 0xffffff; cn = row(y - 1)[x] & 0xffffff; cs = row(y + 1)[x] & 0xffffff;
					} else {   // at its edge: the picture beneath for what isn't 3D
						cm = colour(x, y); cn = colour(x, y - 1); cs = colour(x, y + 1); cw = colour(x - 1, y); ce = colour(x + 1, y);
						lm = luma(cm); ln = luma(cn); ls = luma(cs); lw = luma(cw); le = luma(ce);
					}
					const int lo = std::min({ lm, ln, ls, lw, le }), hi = std::max({ lm, ln, ls, lw, le });
					if (hi - lo < THRESHOLD) continue;
					// how much the pixel stands out from its neighbours' average (FXAA's subpixel amount)
					const float sub = std::min(1.0f, float(std::abs(ln + ls + lw + le - 4 * lm)) / float(4 * (hi - lo)));
					const float sm = sub * sub * (3 - 2 * sub);
					const int k = int(sm * sm * (0.75f * 0.5f * 256) + 0.5f);
					if (k == 0) continue;
					// the edge's direction, and the side across it that differs most
					const bool across_v = std::abs(ln + ls - 2 * lm) >= std::abs(lw + le - 2 * lm);
					const u32 c1 = across_v ? cn : cw, c2 = across_v ? cs : ce;
					const int l1 = across_v ? ln : lw, l2 = across_v ? ls : le;
					const u32 other = std::abs(l1 - lm) >= std::abs(l2 - lm) ? c1 : c2;
					auto mix = [&](int sh) { const int p = (cm >> sh) & 0xff, q = (other >> sh) & 0xff; return u32(p + (((q - p) * k + 128) >> 8)) << sh; };
					changes.push_back({ uint32_t(y) * uint32_t(stride) + uint32_t(x), 0xff000000u | mix(16) | mix(8) | mix(0) });
				}
				std::swap(L0, L1); std::swap(L1, L2);
			}
		}
	});
	for (const auto &c : m_aa_changes)
		for (const auto &k : c) px[k.first] = k.second;
}

// The backdrop filter: the backdrop (m_sys24) softened a touch (a Gaussian of 0.6 pixels, so the
// dithering and stair steps blur away, as vf1-recomp's skies: m_bd_soft) and, to `enlarge`,
// enlarged across with straight lines between the pixels' middles (m_bd_wide, w * s by 384);
// compose() blends its rows down.
void model2a::backdrop_smooth(int w, bool enlarge)
{
	const int s = m_scale, W = w * s;
	m_bd_soft.resize(size_t(w) * HEIGHT);
	if (enlarge) m_bd_wide.resize(size_t(W) * HEIGHT);
	const u32 pen0 = m_pens[0];
	static const float K[5] = { 0.0026f, 0.1655f, 0.6638f, 0.1655f, 0.0026f };   // sigma 0.6, normalised
	std::vector<u32> across(size_t(w) * HEIGHT);
	parallel_rows(HEIGHT, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const u32 *src = &m_sys24.pix(y);
			u32 *a = &across[size_t(y) * w];
			for (int x = 0; x < w; x++) {
				float r = 0, g = 0, b = 0;
				for (int i = -2; i <= 2; i++) {
					const u32 c0 = src[std::clamp(x + i, 0, w - 1)], c = c0 ? c0 : pen0;
					r += K[i + 2] * ((c >> 16) & 0xff); g += K[i + 2] * ((c >> 8) & 0xff); b += K[i + 2] * (c & 0xff);
				}
				a[x] = u32(r + 0.5f) << 16 | u32(g + 0.5f) << 8 | u32(b + 0.5f);
			}
		}
	});
	// down (the softening's second pass), then each row enlarged across
	parallel_rows(HEIGHT, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			u32 *row = &m_bd_soft[size_t(y) * w];
			for (int x = 0; x < w; x++) {
				float r = 0, g = 0, b = 0;
				for (int i = -2; i <= 2; i++) {
					const u32 c = across[size_t(std::clamp(y + i, 0, HEIGHT - 1)) * w + x];
					r += K[i + 2] * ((c >> 16) & 0xff); g += K[i + 2] * ((c >> 8) & 0xff); b += K[i + 2] * (c & 0xff);
				}
				row[x] = u32(r + 0.5f) << 16 | u32(g + 0.5f) << 8 | u32(b + 0.5f);
			}
			if (!enlarge) continue;
			u32 *d = &m_bd_wide[size_t(y) * W];
			for (int X = 0; X < W; X++) {
				const int num = 2 * X + 1 - s;   // the source column: (X + 0.5) / s - 0.5
				const int xa = num < 0 ? 0 : std::min(num / (2 * s), w - 1), xb = std::min(xa + 1, w - 1);
				const u32 fx = num < 0 ? 0 : u32((num - xa * 2 * s) * 256 / (2 * s));
				const u32 a = row[xa], b = row[xb];
				d[X] = ((((a & 0xff00ff) * (256 - fx) + (b & 0xff00ff) * fx) >> 8) & 0xff00ff) | ((((a & 0xff00) * (256 - fx) + (b & 0xff00) * fx) >> 8) & 0xff00);
			}
		}
	});
}

// A 4:3 screen in a wide picture: each side takes the colour of the screen's edge beside it when
// that edge is one colour (the SEGA logo's white, a fade's black or white: it reads as one wide
// screen), else black. An edge is one colour when its outermost column is, 95% of it (small
// things over it allowed). Each picture's sides go by that picture alone, so they change with the
// middle, never after it (in the attract's 12,000 frames and the scripted game's 3,000 no edge is
// one colour, or mixed, for just a picture or two between runs of the other: no flicker to hold
// back). A screen with painted sides (load_screen) has them instead, as far as they go; "TRY NEXT
// STAGE" has its wall carried on (wall_sides).
void model2a::sides_into(u32 *out, size_t stride)
{
	const int s = m_scale, e = m_ext;
	u32 edge[2 * HEIGHT];
	for (int y = 0; y < HEIGHT; y++) {
		edge[y] = out[size_t(y) * s * stride + size_t(e) * s];
		edge[HEIGHT + y] = out[size_t(y) * s * stride + size_t(e + WIDTH) * s - 1];
	}
	fill_sides(out, stride, edge);
}

int model2a::fill_sides(u32 *out, size_t stride, const u32 *edge, int gap, u32 *colours)
{
	const int s = m_scale, e = m_ext, w = WIDTH + 2 * e;
	u32 fill[2];
	for (int side = 0; side < 2; side++) {
		const u32 *col = edge + side * HEIGHT;
		u32 cand = 0;
		int votes = 0;
		for (int y = 0; y < HEIGHT; y++) {   // (the commonest colour, if one has most of the column)
			const u32 c = col[y] & 0xffffff;
			if (!votes) cand = c, votes = 1; else votes += c == cand ? 1 : -1;
		}
		int n = 0;
		for (int y = 0; y < HEIGHT; y++) n += (col[y] & 0xffffff) == cand;
		fill[side] = n * 20 >= HEIGHT * 19 ? 0xff000000 | cand : 0xff000000;
	}
	// painted sides: the screen recognised by its backdrop map
	const screen_art *art = nullptr;
	const u32 print = backdrop_print();
	for (const screen_art &a : m_screens) if (a.print == print) art = &a;
	if (!art && print == NEXT_STAGE_PRINT) { wall_sides(out, stride, gap); return 2; }
	if (!art) {
		if (colours) { colours[0] = fill[0]; colours[1] = fill[1]; return 1; }
		parallel_rows(HEIGHT * s, [&](int y0, int y1) {
			for (int y = y0; y < y1; y++) {
				u32 *row = out + size_t(y) * stride;
				std::fill(row, row + e * s, fill[0]);
				std::fill(row + (e + WIDTH) * s - gap, row + w * s - gap, fill[1]);
			}
		});
		return 2;
	}
	// the picture's column for each of the screen's (arcade x relative to the 4:3's left edge)
	const double k = double(art->h) / HEIGHT;   // picture pixels per arcade pixel
	auto pic_x = [&](double ax) { return int(std::floor(art->w / 2.0 + (ax - WIDTH / 2.0) * k)); };
	auto pic_y = [&](double ay) { return std::clamp(int(ay * k), 0, art->h - 1); };
	m_side_cols.resize(size_t(w) * s);
	for (int X = 0; X < w * s; X++) {
		const int c = pic_x((X + 0.5) / s - e);
		m_side_cols[X] = c >= 0 && c < art->w ? c : -1;
	}
	// The brightness, per side and channel: the game's 8 columns at the 4:3's edge (its 2D) against
	// the picture's 8 beside them, a gain (16.16). What's left of a step at the seam itself (the
	// game's edge column against the picture's beside it, after the gain) is made up near it, the
	// correction fading out over SEAM arcade pixels.
	constexpr int SEAM = 24;
	int gain[2][3], step[2][3];
	for (int side = 0; side < 2; side++) {
		double game[3] = {}, pic[3] = {}, edge_game[3] = {}, edge_pic[3] = {};
		for (int y = 0; y < HEIGHT; y += 2)
			for (int i = 0; i < 8; i++) {
				const u32 g = under_3d(y, e + (side ? WIDTH - 1 - i : i));
				const int c = pic_x(side ? WIDTH + i + 0.5 : -1 - i + 0.5);
				if (!g || c < 0 || c >= art->w) continue;
				const u32 p = art->px[size_t(pic_y(y + 0.5)) * art->w + c];
				if ((p >> 24) < 128) continue;
				for (int ch = 0; ch < 3; ch++) {
					game[ch] += (g >> (16 - 8 * ch)) & 0xff;
					pic[ch] += (p >> (16 - 8 * ch)) & 0xff;
					if (i == 0) {
						edge_game[ch] += (g >> (16 - 8 * ch)) & 0xff;
						edge_pic[ch] += (p >> (16 - 8 * ch)) & 0xff;
					}
				}
			}
		for (int ch = 0; ch < 3; ch++) {
			const double gn = std::clamp(pic[ch] > 0 ? game[ch] / pic[ch] : 1.0, 0.0, 2.0);
			gain[side][ch] = int(gn * 65536);
			const double n = HEIGHT / 2.0;
			step[side][ch] = int(std::clamp((edge_game[ch] - edge_pic[ch] * gn) / n, -32.0, 32.0) * 65536);
		}
	}
	// each screen column's share of the seam's correction (16.16)
	m_side_seam.resize(size_t(w) * s);
	for (int X = 0; X < w * s; X++) {
		const double d = X < e * s ? (e * s - X - 0.5) / s : (X + 0.5 - (e + WIDTH) * s) / s;
		m_side_seam[X] = int(std::max(0.0, 1.0 - d / SEAM) * 65536);
	}
	parallel_rows(HEIGHT * s, [&](int y0, int y1) {
		for (int Y = y0; Y < y1; Y++) {
			const u32 *src = &art->px[size_t(pic_y((Y + 0.5) / s)) * art->w];
			u32 *row = out + size_t(Y) * stride;
			for (int side = 0; side < 2; side++) {
				const int a = side ? (e + WIDTH) * s : 0, b = side ? w * s : e * s;
				const int *g = gain[side], *st = step[side];
				u32 *r = side ? row - gap : row;
				for (int X = a; X < b; X++) {
					const int c = m_side_cols[X];
					const u32 p = c >= 0 ? src[c] : 0;
					if ((p >> 24) < 128) { r[X] = fill[side]; continue; }
					const int64_t seam = m_side_seam[X];
					u32 o = 0xff000000;
					for (int ch = 0; ch < 3; ch++) {
						const int64_t v = int64_t((p >> (16 - 8 * ch)) & 0xff) * g[ch] + ((st[ch] * seam) >> 16);
						o |= u32(std::clamp(int((v + 32768) >> 16), 0, 255)) << (16 - 8 * ch);
					}
					r[X] = o;
				}
			}
		}
	});
	return 2;
}

// The 4:3 screens whose sides can be painted, by their backdrop's fingerprint (backdrop_print):
// the same whatever the palette does, so the sides fade with the screen. The player select's in
// both its layouts (one player, and two: "VS"), however it's reached.
namespace {
struct screen_record { const char *name; u32 print; };
const screen_record SCREEN_RECORDS[] = { { "select", 0xf045d4e6 } };
} // namespace

// "TRY NEXT STAGE" (between a 1P game's stages): tile map 2 unscrolled, a wall of riveted plates,
// each blank plate the same 12 x 12 tiles (96 x 96 pixels), the plates of rows 0, 2 and 3 starting
// at x 0 and those of row 1 (y 96-191) at x 48; the letters replace tiles in the middle only. The
// sides carry the wall on with the game's own blank plate, the one at x 384-479, y 0-95, as drawn
// (so they fade with it), each pixel enlarged as the rest of the 2D: (x - the row's start) mod 96,
// y mod 96 of it. (In the 4:3, that formula gives 90% of the screen as the game draws it: all but
// the lettered plates.)
void model2a::wall_sides(u32 *out, size_t stride, int gap)
{
	const int s = m_scale, e = m_ext, w = WIDTH + 2 * e;
	constexpr int PLATE = 96, PLATE_X = 384;
	parallel_rows(HEIGHT, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const int start = y >= PLATE && y < 2 * PLATE ? PLATE / 2 : 0;
			const int py = y % PLATE;
			for (int side = 0; side < 2; side++) {
				const int a = side ? e + WIDTH : 0, b = side ? w : e;   // (picture columns; the 4:3's left edge at e)
				for (int dy = 0; dy < s; dy++) {
					u32 *d = out + size_t(y * s + dy) * stride - (side ? gap : 0);
					for (int x = a; x < b; x++) {
						const u32 p0 = under_3d(py, e + PLATE_X + (((x - e - start) % PLATE) + PLATE) % PLATE), p = p0 ? p0 : m_pens[0];
						for (int dx = 0; dx < s; dx++) d[x * s + dx] = p;
					}
				}
			}
		}
	});
}

std::vector<std::string> model2a::screen_names()
{
	std::vector<std::string> n;
	for (const screen_record &r : SCREEN_RECORDS) n.push_back(r.name);
	return n;
}

bool model2a::load_screen(const std::string &name, std::vector<u32> argb, int w, int h)
{
	for (const screen_record &r : SCREEN_RECORDS)
		if (name == r.name && w > 0 && h > 0 && argb.size() >= size_t(w) * h) {
			std::erase_if(m_screens, [&](const screen_art &a) { return a.name == name; });
			m_screens.push_back({ name, r.print, std::move(argb), w, h });
			return true;
		}
	return false;
}

// Tile map 2's on-screen part, less rows 8-28: the player select's backdrop keeps those as they
// are while its profile panels (rows 8-28) change with the cursor. Only what's on screen (rows
// 0-47, columns 0-61): the rest of the map still holds the last fight's backdrop, so a select
// reached by a challenger mid-fight would print otherwise.
u32 model2a::backdrop_print() const
{
	u32 h = 2166136261u;
	for (int row = 0; row < 48; row++) {
		if (row >= 8 && row < 29) continue;
		for (int x = 0; x < 62; x++) h = (h ^ m_tiles.tile_r(0x2000 + row * 64 + x)) * 16777619u;
	}
	return h;
}

void model2a::set_render_scale(int scale)
{
	render_wait();
	m_scale = std::clamp(scale, 1, 8);
	resize_screen();
	m_video->set_scale(m_scale);
}

void model2a::set_aspect(float aspect)
{
	render_wait();
	m_aspect = std::clamp(aspect, 4.0f / 3.0f, 80.0f / 9.0f);
	// 496 x 384 fills a 4:3 tube: a picture `aspect` wide is 384 * aspect of the tube's square
	// pixels, 372 * aspect of the arcade's
	const float width = m_aspect * WIDTH * 3.0f / 4.0f;
	m_ext = std::max(0, int(std::lround((width - WIDTH) / 2)));
	resize_screen();
	const int old_ext = m_video->ext();
	m_video->set_ext(m_ext);
	set_scenery_culling(m_ext ? 2 : 0);
	// the 3D still to be shown set up again at the new width, from the lists kept, each drawn in
	// its own frame's colours (smooth motion's lists; without them it waits for the next frame)
	if (m_video->smooth() && m_ext != old_ext) {
		bool done[3] = {};
		for (int k = 0; k < 3; k++) {
			const int p = m_video->shown_picture(k);
			if (p < 0 || p > 2 || done[p]) continue;
			done[p] = true;
			if (!m_video->rebuild_picture(p)) continue;
			const uint64_t f = m_video->picture_frame(p);
			const bool revert = !m_ram_log.empty() && m_ram_log.back().frame > f;
			if (revert) ram_log_apply(f, true);
			m_video->draw_layer(p);
			if (revert) ram_log_apply(f, false);
		}
	}
}

// The scenery's culling. The stage's objects are drawn by 0x20708 and the routines it calls
// (each model through 0x7c60), and whether each is drawn goes by the camera's heading (its yaw,
// a 16-bit angle at 0x26 in the camera, 0x500814), from tables made for the 4:3 view (45 degrees
// across): turned, the scenery to the sides of a wider view would come and go. Every such test
// in the program (found by every read of the yaw, 0x26 in the camera, through any register):
//   0x207a0  the 16 objects of the background ring: a mask per eighth of the heading (rounded),
//            0x21628 (0x21170 on stages with flag 3)
//   0x209d0  the 32 objects near the ring, with the camera within it: a visibility mask for each
//            of 16 x 16 cells of the camera's place, for each eighth, 0x70058 (two eighths, the
//            other quadrants by symmetry: the cell turned and the objects renumbered, 0x21638)
//   0x20de8  objects with a heading of their own (stage flag 9): drawn within +-67.5 degrees
//            of it, in sixteenths, 0x20ea4
//   0x20ed4  objects at the angle the TGP works out from the camera (flag 7): +-45 degrees,
//            0x20eb4
//   0x21180  stage type 3's objects: a mask per eighth (truncated), 0x21200
//   0x21478  one object of the stage's (0x6eedc), drawn with the heading in a half turn
//   0x2ec78  stage type 11's two objects: a mask per eighth, 0x2ee70
//   0x2ef58  stage type 1: two objects drawn with the heading in a half turn, and one hidden by
//            eighths (0x2f778: 0xffff hides it)
//   0x2f0c8  stage type 1's objects, each with the eighths it's seen from (0x2f35c, bit per eighth)
//   0x2fa44  stage type 1's six animated objects: a mask per eighth, 0x2fc2c
//   0x2fce0  stage type 7's two groups: drawn by eighths, 0x2fd08 and 0x2fd10
//   0x30380  stage flag 11's objects (for 0x303e8, 0x30524, 0x30b8c): the camera's zone (0x303a4)
//            and a mask per eighth, 0x303c8
// (The camera outside the ring, 0x21084, draws every object, in two passes; 0x2fbc4's heading
// table starts animations, not drawing.) Widened, the half-turn tests always draw, and each
// eighth's objects are those of its neighbours too (1; two sixteenths more each way for the
// sixteenths), or of every eighth (2, widescreen's: at 32:9 the neighbours alone still lose a
// boat's sail and the edges of floors at the sides); each cell's mask takes in those eighths'
// (in all four quadrants, since the table serves them all). The patches are the program's data
// in memory, checked first (CRC-32 of the code and tables, 0x20708-0x216b7, 0x2ec24-0x2fd17,
// 0x30328-0x303e7, 0x70058-0x70c57), undone for 4:3.
bool model2a::set_scenery_culling(int mode)
{
	auto &rom = m_roms.regions[romset::MAINCPU];
	struct range { u32 from, to, crc; };
	static const range CHECKED[] = { { 0x20708, 0x216b8, 0x02d57b8c }, { 0x2ec24, 0x2fd18, 0x0d31ffb3 }, { 0x30328, 0x303e8, 0x373c814c },
		{ 0x70058, 0x70c58, 0x20b7f4c0 } };
	if (m_culling_orig.empty()) {
		for (const range &r : CHECKED)
			if (r.to > rom.size() || ::crc32(0, &rom[r.from], uInt(r.to - r.from)) != r.crc) return false;
		for (const range &r : CHECKED) m_culling_orig.insert(m_culling_orig.end(), &rom[r.from], &rom[r.to]);
	}
	// the program as it was, then the tables made again from it
	size_t at = 0;
	for (const range &r : CHECKED) { memcpy(&rom[r.from], &m_culling_orig[at], r.to - r.from); at += r.to - r.from; }
	m_culling = std::clamp(mode, 0, 2);
	if (!m_culling) { if (m_rec) m_rec->code_changed(0x20708, 0x70c58 - 0x20708); return true; }
	const bool all = m_culling == 2;
	auto rd = [&](u32 a, int size) { u32 v = 0; for (int i = 0; i < size; i++) v |= u32(rom[a + i]) << (8 * i); return v; };
	auto wr = [&](u32 a, int size, u32 v) { for (int i = 0; i < size; i++) rom[a + i] = u8(v >> (8 * i)); };
	// tables by eighths: a set bit draws (or, 0x2f778, hides)
	struct eighths { u32 addr; int size; bool hides; };
	static const eighths EIGHTHS[] = { { 0x21628, 2, false }, { 0x21170, 2, false }, { 0x21200, 4, false }, { 0x2ee70, 4, false },
		{ 0x2f35c, 1, false }, { 0x2fc2c, 1, false }, { 0x2fd08, 1, false }, { 0x2fd10, 1, false }, { 0x303c8, 4, false },
		{ 0x2f778, 2, true } };
	for (const eighths &t : EIGHTHS) {
		u32 v[8];
		for (int o = 0; o < 8; o++) v[o] = rd(t.addr + o * t.size, t.size);
		for (int o = 0; o < 8; o++) {
			u32 w = v[o];
			for (int d = 1; d < 8; d++)
				if (all || d == 1 || d == 7) w = t.hides ? (w & v[(o + d) & 7]) : (w | v[(o + d) & 7]);
			wr(t.addr + o * t.size, t.size, w);
		}
	}
	// tables by sixteenths (a byte each: drawn or not), two more each way
	for (u32 addr : { 0x20ea4u, 0x20eb4u }) {
		u8 v[16];
		memcpy(v, &rom[addr], 16);
		for (int s = 0; s < 16; s++) {
			bool on = all;
			for (int d = -2; d <= 2; d++) on = on || v[(s + d) & 15];
			rom[addr + s] = on;
		}
	}
	wr(0x21478, 4, 0x5c78160f);   // bbc 15,r15,0x21490 -> mov r15,r15: drawn at any heading
	wr(0x2ef58, 4, 0x0800000c);   // bbs 15,r5,0x2ef64 -> b 0x2ef64: the same
	// the cells: in quadrant q (the heading's quarter) the camera's cell (x, z) is looked up as
	// cell(q, x, z) and bit b of the mask is object perm[q][b]
	const u32 MASKS = 0x70058, PERM = 0x21638;
	auto cell = [](int q, int x, int z) { return q == 0 ? z * 16 + x : q == 1 ? (15 - x) * 16 + z : q == 2 ? (15 - x) + (15 - z) * 16 : (15 - z) + x * 16; };
	auto seen = [&](int o, int x, int z) {   // the objects (by number) drawn from cell (x, z) heading o
		const int q = o >> 1;
		const u32 bits = rd(MASKS + (o & 1) * 1024 + cell(q, x, z) * 4, 4);
		u32 objects = 0;
		for (int b = 0; b < 32; b++) if (bits >> b & 1) objects |= 1u << rom[PERM + q * 32 + b];
		return objects;
	};
	std::vector<u32> wide(512);
	for (int p = 0; p < 2; p++)
		for (int c = 0; c < 256; c++) {
			u32 bits = rd(MASKS + p * 1024 + c * 4, 4);
			for (int q = 0; q < 4; q++) {
				const int hi = c >> 4, lo = c & 15;
				const int x = q == 0 ? lo : q == 1 ? 15 - hi : q == 2 ? 15 - lo : hi;
				const int z = q == 0 ? hi : q == 1 ? lo : q == 2 ? 15 - hi : 15 - lo;
				const int o = 2 * q + p;
				u32 objects = 0;
				for (int d = 1; d < 8; d++)
					if (all || d == 1 || d == 7) objects |= seen((o + d) & 7, x, z);
				for (int b = 0; b < 32; b++) if (objects >> rom[PERM + q * 32 + b] & 1) bits |= 1u << b;
			}
			wide[p * 256 + c] = bits;
		}
	for (int i = 0; i < 512; i++) wr(MASKS + i * 4, 4, wide[i]);
	if (m_rec) m_rec->code_changed(0x20708, 0x70c58 - 0x20708);
	return true;
}

void model2a::resize_screen()
{
	const int w = WIDTH + 2 * m_ext;
	if (m_screen.width() != w * m_scale || m_screen.height() != HEIGHT * m_scale) m_screen.allocate(w * m_scale, HEIGHT * m_scale);
	if (m_sys24.width() != w) {
		m_sys24.allocate(w, HEIGHT + 4);
		m_sys24_mid.allocate(w, HEIGHT + 4);
		m_sys24_top.allocate(w, HEIGHT + 4);
	}
}

// A fight's screens (the attract's demos, the intros, knockouts and replays too) have a stage's
// backdrop up: tile map 2's control register set. It's 0 on the title, select and versus screens.
void model2a::freecam_fighters()
{
	constexpr u32 RECORDS = 0x0200620c, MODELS = 0x020e0004;   // (random_fighter.cpp's: the fighters' records, the models' table)
	auto rd = [&](u32 a) -> u32 {
		const std::vector<u8> &r = a >= 0x02000000 ? m_roms.regions[romset::MAIN_DATA] : m_roms.regions[romset::MAINCPU];
		const u32 o = a >= 0x02000000 ? a - 0x02000000 : a;
		if (size_t(o) + 4 > r.size()) return 0;
		u32 v; memcpy(&v, &r[o], 4); return v;
	};
	std::vector<u32> obas[2];
	for (int pl = 0; pl < 2; pl++) {
		u32 b;
		memcpy(&b, &m_workram[0x804 + 4 * pl], 4);
		if (b < 0x500000 || b - 0x500000 + 0x200 >= m_workram.size()) continue;
		const int f = m_workram[b - 0x500000 + 0x1b0];
		if (f >= 26) continue;
		const u32 rec = rd(RECORDS + 4 * u32(f));
		for (u32 o : { 0u, 0x34u, 0x44u }) {
			const u32 q = rd(rec + o);
			if (!q) continue;
			for (u32 s = 0; s < 16; s++) {
				const u32 mdl = rd(q + 4 * s);
				if (!mdl || mdl >= 0x2000) continue;
				if (const u32 oba = rd(MODELS + 16 * mdl + 8)) obas[pl].push_back(oba);
			}
		}
	}
	if (obas[0] != m_video->m_fc_fighters[0] || obas[1] != m_video->m_fc_fighters[1]) { render_wait(); m_video->m_fc_fighters[0] = std::move(obas[0]); m_video->m_fc_fighters[1] = std::move(obas[1]); }
}

bool model2a::freecam_scene() const
{
	const u16 bd = m_tiles.tile_r(0x5006);
	return bd != 0 && !(bd & 0x8000);
}

// The free camera's backdrop move: its yaw and pitch against the game's camera, at the backdrop's own rate (the game
// scrolls it by its camera's heading and pitch / 16, of 65536 a turn: 4096 pixels a turn, measured against the camera's
// yaw over a fight's pans: 11.4 a degree), in whole arcade pixels (m_fc_dx, m_fc_dy: the layers drawn moved) and the
// rest in the render scale's pixels (m_fc_fx, m_fc_fy: added to the backdrop's smooth move as it's put together), so it
// moves smoothly, not a pixel at a time
void model2a::freecam_sky_move()
{
	m_fc_dx = m_fc_dy = m_fc_fx = m_fc_fy = 0;
	m_video->m_fc_scene = freecam_scene();
	double yaw, pitch;
	if (!m_video->freecam_on() || !freecam_scene() || !m_video->freecam_sky(yaw, pitch)) return;
	constexpr double PX_PER_DEGREE = 4096.0 / 360;
	const double x = FC_SKY_SX * yaw * PX_PER_DEGREE, y = FC_SKY_SY * pitch * PX_PER_DEGREE;
	m_fc_dx = int(std::floor(x));
	m_fc_dy = int(std::floor(y));
	m_fc_fx = int(std::lround((x - m_fc_dx) * m_scale));
	m_fc_fy = int(std::lround((y - m_fc_dy) * m_scale));
}

bool model2a::wide_frame() const
{
	const u16 bd = m_tiles.tile_r(0x5006);
	return m_ext && bd != 0 && !(bd & 0x8000);
}

// The stages' backdrop panoramas, for widescreen. Each is a record in the program: a word with
// its rows (low 16 bits), then 18 pointers to blocks of 32 columns x rows tile map entries in the
// data ROM: 8 blocks for the full turn of the camera (2048 pixels) and the first 10 again. As the
// camera turns, the game copies the columns coming into view into tile map 2 (its rows from 0,
// panorama column c in map column c mod 64) and scrolls the map; the rest of the panorama is only
// in the ROM. The records are found by their shape, so a program they aren't in has none.
void model2a::read_panoramas()
{
	if (m_panoramas_read) return;
	m_panoramas_read = true;
	auto rd32 = [&](size_t o) { return u32(m_rom[o] | m_rom[o + 1] << 8 | m_rom[o + 2] << 16 | u32(m_rom[o + 3]) << 24); };
	for (size_t o = 0; o + 76 <= m_rom.size(); o += 4) {
		const u32 h = rd32(o);
		const int rows = int(h & 0xffff);
		if (rows < 8 || rows > 64 || (h >> 16) > 0xff) continue;
		u32 p[18];
		bool ok = true;
		for (int i = 0; i < 18 && ok; i++) {
			p[i] = rd32(o + 4 + 4 * i);
			ok = p[i] >= 0x02000000 && size_t(p[i] - 0x02000000) + size_t(64) * rows <= m_data.size();
		}
		for (int i = 0; i < 10 && ok; i++) ok = p[8 + i] == p[i];
		if (!ok) continue;
		panorama_record r;
		r.rows = rows;
		r.entries.resize(size_t(256) * rows);
		for (int b = 0; b < 8; b++)
			for (int y = 0; y < rows; y++)
				for (int x = 0; x < 32; x++) {
					const u8 *q = &m_data[p[b] - 0x02000000 + 2 * (y * 32 + x)];
					r.entries[size_t(y) * 256 + b * 32 + x] = u16(q[0] | q[1] << 8);
				}
		m_panoramas.push_back(std::move(r));
	}
}

// Which panorama tile map 2 holds, and where. Each of the record's rows that's on screen (with a
// line scroll the rows can scroll apart: the stage's sky layers) is placed by its columns on
// screen against the record's at the four places its scroll allows (the map holds 512 pixels of
// the 2048): the one with the most matching, if the row tells them apart (rows of one tile tell
// nothing: those take the commonest place). The record is the one with the most matching columns
// over all, at least half of those on screen; the last one found is tried first. Null if none.
const segas24_tile::panorama *model2a::find_panorama()
{
	read_panoramas();
	if (m_panoramas.empty()) return nullptr;
	// each tile row's map x at the screen's left edge, from a line showing it (-1: none)
	int x0_row[64];
	std::fill(std::begin(x0_row), std::end(x0_row), -1);
	for (int y = 0; y < HEIGHT; y++) {
		int map, my, x0;
		if (!m_tiles.line_source(2 << 1, y, map, my, x0)) return nullptr;
		if (map == 2 && x0_row[my >> 3] < 0) x0_row[my >> 3] = x0;
	}
	struct placing { int matched = 0, checked = 0; int8_t turn[64]; };
	auto place = [&](const panorama_record &r) {
		placing p;
		for (int row = 0; row < 64; row++) p.turn[row] = -1;
		for (int row = 0; row < r.rows && row < 64; row++) {
			const int x0 = x0_row[row];
			if (x0 < 0) continue;
			int n[4] = {}, seen = 0;
			for (int c = 0; c < 64; c++) {
				const int xs = (c * 8 - x0) & 511;
				if (xs > WIDTH - 8) continue;
				seen++;
				const u16 e = m_tiles.tile_r((2 << 12) | (row * 64 + c));
				for (int q = 0; q < 4; q++)
					n[q] += e == r.entries[size_t(row) * 256 + (((x0 + 512 * q + xs) >> 3) & 255)];
			}
			const int best = int(std::max_element(n, n + 4) - n);
			p.matched += n[best];
			p.checked += seen;
			if (n[best] > *std::min_element(n, n + 4)) p.turn[row] = int8_t(best);
		}
		return p;
	};
	int best = -1;
	placing bp;
	if (m_pan_last >= 0) {   // most frames: the last one
		bp = place(m_panoramas[m_pan_last]);
		if (bp.matched * 4 >= bp.checked * 3 && bp.checked) best = m_pan_last;
	}
	if (best < 0)
		for (int k = 0; k < int(m_panoramas.size()); k++) {
			placing p = place(m_panoramas[k]);
			if (best < 0 || p.matched > bp.matched) best = k, bp = p;
		}
	if (best < 0 || !bp.checked || bp.matched * 2 < bp.checked) { m_pan_last = -1; return nullptr; }
	m_pan_last = best;
	const panorama_record &r = m_panoramas[best];
	// the rows that couldn't be told: the commonest place
	int votes[4] = {};
	for (int row = 0; row < r.rows; row++) if (bp.turn[row] >= 0) votes[bp.turn[row]]++;
	const int8_t common = int8_t(std::max_element(votes, votes + 4) - votes);
	m_pan_view = segas24_tile::panorama{ r.entries.data(), 256, r.rows, {}, {} };
	for (int row = 0; row < 64; row++) {
		const bool on = row < r.rows && x0_row[row] >= 0;
		m_pan_view.x0[row] = int16_t(on ? x0_row[row] : 0);
		m_pan_view.at[row] = int16_t(on ? (x0_row[row] + 512 * (bp.turn[row] >= 0 ? bp.turn[row] : common)) & 2047 : -1);
	}
	return &m_pan_view;
}

// ---------------------------------------------------------------------------------------------
// 315-5649 I/O chip, with the EEPROM on port A/B (model2a: eeprom_w, in0_r)
// ---------------------------------------------------------------------------------------------

u8 model2a::in0_r()
{
	u8 data = m_in0;
	if (m_ctrlmode)
		return 0xc0 | (m_eeprom.do_r(now()) << 5) | 0x10 | (data & 0x0f);
	return data;
}

u8 model2a::io_read(int offset)
{
	u8 data = 0xff;
	switch (offset) {
	case 0x06:
		if (m_io_mode & 0x80) {   // port G counter mode: no counters connected
			data = 0;
			m_io_port[6] = (m_io_port[6] & 0xf8) | ((m_io_port[6] + 1) & 7);
			break;
		}
		[[fallthrough]];
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
		if (BIT(m_io_config, offset)) {
			switch (offset) {
			case 1: data = in0_r(); break;
			case 2: data = m_in1; break;
			case 3: data = m_in2; break;
			case 6: data = m_sw; break;
			default: data = 0xff; break;
			}
		} else
			data = m_io_port[offset];
		break;
	case 0x0b: case 0x0c: data = 0; break;   // RS-422 input
	case 0x0d: data = 0x0c; break;           // RS-422 status (MAME's hack)
	case 0x0f: data = 0xff; m_io_analog = (m_io_analog + 1) & 7; break;
	}
	return data;
}

void model2a::io_write(int offset, u8 data)
{
	switch (offset) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
		m_io_port[offset] = data;
		if (offset == 0) eeprom_w(data);   // port A; E is the billboard, F the lamps
		break;
	case 0x08: m_io_config = data; break;
	case 0x0e: m_io_mode = data; break;
	case 0x0f: m_io_analog = data & 7; break;
	}
}

void model2a::eeprom_w(u8 data)
{
	const uint64_t t = now();
	m_ctrlmode = BIT(data, 0);
	m_eeprom.di = BIT(data, 5);
	m_eeprom.clk_w(BIT(data, 7), t);
	m_eeprom.cs_w(BIT(data, 6), t);
}

// MAME's eeprom_serial_93cxx_device: 64 cells of 16 bits, 6 address bits. Times in i960 cycles.
namespace { enum { EV_CS_RISE = 1, EV_CS_FALL = 2, EV_CLK_RISE = 4, EV_CLK_FALL = 8 }; }
static constexpr uint64_t EE_WRITE = 1750 * 25, EE_WRITE_ALL = 8000 * 25, EE_ERASE = 1000 * 25, EE_ERASE_ALL = 8000 * 25;

void model2a::eeprom93c46::cs_w(int s, uint64_t now)
{
	s &= 1;
	if (s == cs) return;
	cs = s;
	if (s) cs_rise = now;
	event(s ? EV_CS_RISE : EV_CS_FALL, now);
}

void model2a::eeprom93c46::clk_w(int s, uint64_t now)
{
	s &= 1;
	if (s == clk) return;
	clk = s;
	event(s ? EV_CLK_RISE : EV_CLK_FALL, now);
}

int model2a::eeprom93c46::do_r(uint64_t now) const
{
	const bool ready = now >= completion;
	if (state == WAIT_FOR_START_BIT) return ready ? 1 : 0;   // DO doubles as READY/BUSY
	if (state == READING_DATA) return (shift & 0x80000000) ? 1 : 0;
	return 1;
}

void model2a::eeprom93c46::event(int ev, uint64_t now)
{
	const bool ready = now >= completion;
	switch (state) {
	case IN_RESET:
		if (ev == EV_CS_RISE) state = WAIT_FOR_START_BIT;
		break;
	case WAIT_FOR_START_BIT:
		if (ev == EV_CLK_RISE && di && ready && now > cs_rise) { cmd_addr = 0; bits = 0; state = WAIT_FOR_COMMAND; }
		else if (ev == EV_CS_FALL) state = IN_RESET;
		break;
	case WAIT_FOR_COMMAND:
		if (ev == EV_CLK_RISE) {
			cmd_addr = (cmd_addr << 1) | di;
			if (++bits == 2 + 6) execute(now);
		} else if (ev == EV_CS_FALL) state = IN_RESET;
		break;
	case READING_DATA:
		if (ev == EV_CLK_RISE) {
			int bit_index = bits++;
			if (bit_index % 16 == 0 && bit_index == 0)
				shift = u32(cells[(address + bits / 16) & 63]) << 16;
			else
				shift = (shift << 1) | 1;
		} else if (ev == EV_CS_FALL) state = IN_RESET;
		break;
	case WAIT_FOR_DATA:
		if (ev == EV_CLK_RISE) {
			shift = (shift << 1) | di;
			if (++bits == 16) execute_write(now);
		} else if (ev == EV_CS_FALL) state = IN_RESET;
		break;
	case WAIT_FOR_COMPLETION:
		if (ev == EV_CS_FALL) state = IN_RESET;
		break;
	}
}

void model2a::eeprom93c46::execute(uint64_t now)
{
	command = INVALID;
	address = cmd_addr & 63;
	switch (cmd_addr >> 6) {
	case 0:
		switch (address >> 4) {
		case 0: command = LOCK; break;
		case 1: command = WRITEALL; break;
		case 2: command = ERASEALL; break;
		case 3: command = UNLOCK; break;
		}
		address = 0;
		break;
	case 1: command = WRITE; break;
	case 2: command = READ; break;
	case 3: command = ERASE; break;
	}
	bits = 0;
	switch (command) {
	case READ: shift = 0; state = READING_DATA; break;
	case WRITE: case WRITEALL: shift = 0; state = WAIT_FOR_DATA; break;
	case ERASE:
		if (locked) { state = IN_RESET; break; }
		cells[address] = 0xffff; completion = now + EE_ERASE; state = WAIT_FOR_COMPLETION;
		break;
	case LOCK: locked = true; state = IN_RESET; break;
	case UNLOCK: locked = false; state = IN_RESET; break;
	case ERASEALL:
		if (locked) { state = IN_RESET; break; }
		std::fill(std::begin(cells), std::end(cells), 0xffff); completion = now + EE_ERASE_ALL; state = WAIT_FOR_COMPLETION;
		break;
	default: break;
	}
}

void model2a::eeprom93c46::execute_write(uint64_t now)
{
	if (locked) { state = IN_RESET; return; }
	if (command == WRITE) { cells[address] = u16(shift); completion = now + EE_WRITE; }
	else { for (auto &c : cells) c &= u16(shift); completion = now + EE_WRITE_ALL; }
	state = WAIT_FOR_COMPLETION;
}

// ---------------------------------------------------------------------------------------------
// The sound board (MAME's model2_scsp and model2_snd): the 68000 on its map, the SCSP, and the
// link from the main board's UART into the SCSP's MIDI input.
// ---------------------------------------------------------------------------------------------

// The 68000's bus (model2_snd): RAM and the SCSP take a wait state per access (before_delay(1)),
// which, as in MAME, can push an access into the next timeslice; the interrupt acknowledge goes
// by the cpu space's autovectors, which wait for the E clock (vpa_sync) and a cycle after.
struct model2a::snd_bus : m68000_bus
{
	model2a &m;
	u64 last_vpa_time = 0;   // m68000_device::m_last_vpa_time
	const int ram_tag = 0, scsp_tag = 0;
	explicit snd_bus(model2a &mm) : m(mm) {}

	u16 rom(const std::vector<u8> &r, u32 o) const { return o + 1 < r.size() ? u16(r[o] << 8 | r[o + 1]) : 0; }
	u16 peek(offs_t a) override
	{
		a &= 0xfffffe;
		const std::vector<u8> &samples = m.m_roms.regions[romset::SAMPLES];
		if (a < 0x080000) return m.m_soundram[a >> 1];
		if (a >= 0x600000 && a < 0x680000) return rom(m.m_roms.regions[romset::AUDIOCPU], a - 0x600000);
		if (a >= 0x800000 && a < 0xa00000) return rom(samples, a - 0x800000);
		if (a >= 0xa00000 && a < 0xe00000) return rom(samples, a - 0xa00000 + 0x200000);   // bank4
		if (a >= 0xe00000) return rom(samples, a - 0xe00000 + 0x600000);                   // bank5
		return 0;
	}
	u64 vpa_sync(u64 current_time)
	{
		if (last_vpa_time >= current_time) return last_vpa_time;
		const u64 mod = current_time % 10;
		last_vpa_time = mod < 7 ? current_time - mod + 10 : current_time - mod + 20;
		return last_vpa_time;
	}
	u16 read(int space, offs_t a, u16 mem_mask) override
	{
		m68000_device &cpu = *m.m_68k;
		if (space == CPU_SPACE) {   // the autovectors: 0xfffff3 + 2 * (level - 1)
			const u64 tc = cpu.total_cycles();
			u64 ac = vpa_sync(tc);
			if (ac < tc) ac = tc;
			if (cpu.access_before_time(ac, tc)) return 0;
			const u16 v = u16(0x18 + ((a >> 1) & 7));
			cpu.access_after_delay(1);
			return v;
		}
		a &= 0xfffffe;
		if (a < 0x080000) {
			if (cpu.access_before_delay(1, &ram_tag)) return 0;
			return m.m_soundram[a >> 1];
		}
		if (a >= 0x100000 && a < 0x101000) {
			if (cpu.access_before_delay(1, &scsp_tag)) return 0;
			return m.m_scsp->read((a - 0x100000) >> 1);
		}
		return peek(a);
	}
	void write(offs_t a, u16 data, u16 mem_mask) override
	{
		m68000_device &cpu = *m.m_68k;
		a &= 0xfffffe;
		if (a < 0x080000) {
			if (cpu.access_before_delay(1, &ram_tag)) return;
			u16 &w = m.m_soundram[a >> 1];
			w = u16((w & ~mem_mask) | (data & mem_mask));
			return;
		}
		if (a >= 0x100000 && a < 0x101000) {
			if (cpu.access_before_delay(1, &scsp_tag)) return;
			if (m.on_68k_scsp_write) m.on_68k_scsp_write(a - 0x100000, data, mem_mask);
			if (m.m_68k->pc() == 0x600a96 && (a & 0xffc1f) == 0) m.m_scsp->set_next_group((a >> 5) & 31, m.voice_group());
			m.m_scsp->write((a - 0x100000) >> 1, data, mem_mask);
			return;
		}
		// 0x400000: model2snd_ctrl, sample banking only with more than 8 MB of samples (not VF2's)
	}
};

void model2a::sound_reset()
{
	m_soundram.assign(0x40000, 0);
	// reset_model2_scsp: copy the 68k vector table into RAM (16 bytes; the regions hold the
	// 68000's bytes in its own order, high byte first)
	const std::vector<u8> &prog = m_roms.regions[romset::AUDIOCPU];
	for (int i = 0; i < 8; i++) m_soundram[i] = u16(prog[2 * i] << 8 | prog[2 * i + 1]);
	scsp_device::host h;
	h.irq = [this](int line, int state) { set_input_line(L_68K_IRQ0 + (line & 7), state == CLEAR_LINE ? CLEAR_LINE : ASSERT_LINE); };
	h.midi_out = [this](int state) { if (!state) m_midi_out_bits++; };   // to the UART's RxD (VF2 never reads it)
	h.timer_adjust = [this](int t, atime delay, atime period) {
		if (delay == scsp_device::NEVER) timer_disable(T_SCSP0 + t);
		else timer_adjust(T_SCSP0 + t, delay, 0, period);
	};
	h.sample_index = [this] {   // sound_stream::get_current_sample_index
		// floor(t * 44100 / 10^18), as the whole seconds and the rest: 10^18 is 2^18 5^18, and
		// dividing by 2^18 (a shift) and then by 5^18 gives the same quotient, in 64 bits
		const atime t = time();
		constexpr uint64_t FIVE18 = 3814697265625ull;
		if (t >> 82) return uint64_t(t / AS_PER_SEC) * SOUND_RATE + uint64_t((t % AS_PER_SEC) * SOUND_RATE / AS_PER_SEC);
		const uint64_t secs = uint64_t(t >> 18) / FIVE18;
		const atime rest = t - atime(secs) * AS_PER_SEC;
		return secs * SOUND_RATE + uint64_t(rest * SOUND_RATE >> 18) / FIVE18;
	};
	h.time_is_zero = [this] { return time() == 0; };
	m_scsp = std::make_unique<scsp_device>(45158400 / 2, m_soundram.data(), u32(m_soundram.size() * 2), h);
	for (int g = 0; g < 2; g++) m_scsp->set_group_gain(g, m_volume[g]);
	m_audio_out.clear();
	m_audio_raw.clear();
	uart_txd(1);   // the i8251's reset: TxD high
	// the sound manager's update, every 1/50 s from power-on
	timer_adjust(T_SOUND_UPDATE, AS_PER_SEC / 50, 0, AS_PER_SEC / 50);
	// the 68000: power-on and reset (it reads its vectors when it first runs)
	m_snd_bus = std::make_unique<snd_bus>(*this);
	m_68k = std::make_unique<m68000_device>(*m_snd_bus, 45158400 / 4);
	m_68k->start();
	m_68k->reset();
}

// Music or effects, for the host's two volumes. VF2's sound program starts each voice from one
// place, 0x600a96 (`move.b #$10,(A5,D4.w)`: the slot's KYONEX with its key bit clear, before it
// sets it up and keys it on), with A0 at the channel it plays for (a 16-byte entry of its channel
// table at 0x4000) and the note in D2's low byte. Every voice the game plays starts there. The
// entry's first byte has bit 0 clear for the tracks of its own sequencer (the music, entries 0-9)
// and set for the channels the main board plays on directly (entries 10 and up): the winner's
// line (10), the fighters' voices (11), the announcer (13's notes under 0x20, 15, 16), the
// crowd (13's notes 0x20 and up: a roar at a big hit, a knockdown, a KO and its replay, the
// WINNER screen; at the level of a hit in the game's mix), and the hits and swishes (14). One
// exception goes with the music: the night stage's weather, channel 14's notes 0x22, 0x28 and
// 0x29. 0x22 is its thunder: a 3-second roll about a second after each lightning flash, every
// 6-7 s, played at TL 2, so in the game's mix it sits about 2 dB under the music and 12 dB over a
// typical hit; 0x28 is a lower rumble and 0x29 a quiet one. Turning the music down turns it down
// too, as the game keeps it with the music.
int model2a::voice_group() const
{
	const u32 a0 = m_68k->reg(8);
	if (a0 < 0x4000 || a0 >= 0x4200) return 0;
	if (!((m_soundram[a0 >> 1] >> 8) & 1)) return 0;   // the sequencer: music
	const u32 entry = (a0 - 0x4000) >> 4, note = m_68k->reg(2) & 0xff;
	if (entry == 14 && (note == 0x22 || note == 0x28 || note == 0x29)) return 0;   // the night stage's weather
	return 1;
}

void model2a::set_volumes(float music, float effects)
{
	m_volume[0] = music;
	m_volume[1] = effects;
	if (m_scsp) for (int g = 0; g < 2; g++) m_scsp->set_group_gain(g, m_volume[g]);
}

void model2a::sound_update_tick()
{
	static const bool sound_prof=getenv("VF2_SOUND_PROFILE")!=nullptr;
	const uint64_t snd_t0=sound_prof?now_ns():0;
	m_scsp->update();
	take_audio();
	if(sound_prof){m_sound_scsp_ns+=now_ns()-snd_t0;++m_sound_scsp_updates;}
}

// The samples the SCSP has made so far: to the host at the output gain (MVOL / 15), as 16-bit.
// Above level 1 a peak limiter keeps them under -1 dBFS: down at once, back up over about 0.1 s.
void model2a::take_audio()
{
	std::vector<float> &o = m_scsp->m_out;
	if (o.empty()) return;
	const float g = m_scsp->output_gain();
	auto s16 = [](double v) { return int16_t(std::clamp(std::lround(v * 32768.0), -32768l, 32767l)); };
	// Probe: the loudest sample this run produced, to prove the stream is not silence.
	auto level = [&](int16_t s) { const int a = s < 0 ? -int(s) : int(s); if (a > m_sound_peak) m_sound_peak = a; };
	if (m_sound_on && m_level == 1.0f)
		for (float v : o) { const int16_t s = s16(double(v) * g); level(s); m_audio_out.push_back(s); }
	else if (m_sound_on) {
		constexpr float CEILING = 0.891f, RELEASE = 1.0f / (0.1f * SOUND_RATE);
		for (size_t i = 0; i + 1 < o.size(); i += 2) {
			const float l = o[i] * g * m_level, r = o[i + 1] * g * m_level;
			const float peak = std::max(std::fabs(l), std::fabs(r));
			const float want = peak > CEILING ? CEILING / peak : 1.0f;
			m_limit = want < m_limit ? want : m_limit + (want - m_limit) * RELEASE;
			const int16_t ls = s16(l * m_limit), rs = s16(r * m_limit);
			level(ls); level(rs);
			m_audio_out.push_back(ls);
			m_audio_out.push_back(rs);
		}
	}
	if (m_keep_raw) m_audio_raw.insert(m_audio_raw.end(), o.begin(), o.end());
	o.clear();
}

void model2a::uart_txd(int state) { m_scsp->midi_in(state); }

// A sound board alone (set_sound_only): the bytes it's given reach the SCSP's MIDI input a MIDI byte's time apart
// (320 us), each at its own moment, on T_UART (no UART runs here), as the byte link delivers them on the game's
// board. (They were taken only as a slice of the 68000's time began, so always at the instant another timer fired,
// mostly the SCSP's timer A, the sound driver's tick. With the timers' new period (8ccab24) their interrupts met so
// that the driver read the empty MIDI input thousands of times (3,007 in 52 s against 2), the last byte again each
// time: its commands garbled, a match's voices, announcer and effects lost or cut short, sometimes its music)
void model2a::sound_bytes(const uint8_t *b, size_t n)
{
	if (!n) return;
	const bool idle = m_snd_queue.empty();
	m_snd_queue.insert(m_snd_queue.end(), b, b + n);
	if (m_sound_only && idle && !m_timer[T_UART].enabled) {
		const atime now = time();
		timer_adjust(T_UART, m_snd_next > now ? m_snd_next - now : 0);
	}
}

void model2a::snd_link_byte()
{
	if (m_snd_queue.empty()) { timer_disable(T_UART); return; }
	m_scsp->midi_in_byte(m_snd_queue.front());
	m_snd_queue.pop_front();
	const atime gap = AS_PER_SEC / 1000000 * 320;
	m_snd_next = time() + gap;
	if (m_snd_queue.empty()) timer_disable(T_UART);
	else timer_adjust(T_UART, gap);
}

uint64_t model2a::snd_cycles() const
{
	const_cast<model2a*>(this)->advance_aux_clocks(m_base);
	const cpu_sched &c = m_sched[CPU_68K];
	return c.cycles + (m_executing == CPU_68K ? uint64_t(m_68k->cycles_used()) : 0);
}

// ---------------------------------------------------------------------------------------------
// i8251 UART: what MAME's model does on the transmit side (VF2 never reads it back). TxC is the
// board's 500 kHz clock (clock_device: low at power-on, a toggle every 1 us); the transmitter
// steps on every br_factor-th falling edge, counted from the mode write.
// ---------------------------------------------------------------------------------------------

namespace { enum { TX_READY = 0x01, TX_EMPTY = 0x04 }; }

void model2a::sound_ready()
{
	const u32 line = 1 << 10;
	if ((m_uart.txrdy() || m_uart.rxrdy()) && (m_intena & line)) {
		m_intreq |= line;
		irq_update();
	}
}

void model2a::uart_tx_ready_changed() { sound_ready(); }   // txrdy_handler and rxrdy_handler

void model2a::uart_start_tx()
{
	m_uart.tx_bits_left = 10;   // start, 8 data, 1 stop (mode 0x4e)
	m_uart.tx_shift = u16(0x200 | m_uart.tx_data << 1);
	m_uart.status &= ~TX_EMPTY;
	m_uart.status |= TX_READY;
	m_uart_bytes++;
	if (m_link_fast) {   // its start bit goes out at the transmitter's next step (this one, in a step)
		m_link_byte = m_uart.tx_data;
		m_link_delivered = false;
		m_link_t0 = m_link_in_step ? time() : link_next_step(time());
	}
	if (m_uart_log.size() < 4096) m_uart_log.push_back(m_uart.tx_data);
	if (on_uart_tx) on_uart_tx(m_uart.tx_data);
}

void model2a::uart_clock_tick()
{
	m_uart.txc ^= 1;
	timer_adjust(T_UART, AS_PER_SEC / 1000000);
	if (!m_uart.txc) uart_transmit_clock();   // write_txc: the falling edge
}

void model2a::uart_transmit_clock()
{
	uart8251 &u = m_uart;
	if (++u.txc_count != u.br_factor) return;
	u.txc_count = 0;
	if (u.tx_bits_left == 0) {
		if ((u.status & TX_READY) == 0 && (u.tx_enabled() || u.delayed_tx_en))
			uart_start_tx();
		else {
			u.status |= TX_EMPTY;
			uart_txd(!BIT(u.command, 3));   // TxD back to marking (high), unless sending a break
		}
		uart_tx_ready_changed();
	}
	if (u.tx_bits_left > 0) {   // the next bit out, to the SCSP's MIDI input
		const int bit = (u.tx_shift >> (10 - u.tx_bits_left)) & 1;
		u.tx_bits_left--;
		uart_txd(bit);
	}
}

void model2a::uart_write(int offset, u8 data)
{
	if (getenv("VF2_UART_LOG")) fprintf(stderr, "uart %s %02x at frame %llu\n", offset ? "ctl" : "data", data, (unsigned long long)m_frame_number);
	uart8251 &u = m_uart;
	if (offset == 0) {   // data_w
		u.tx_data = data;
		u.status &= ~TX_READY;
		uart_tx_ready_changed();
		u.delayed_tx_en = u.tx_enabled();
		if (u.tx_enabled() && u.tx_bits_left == 0 && (u.status & TX_READY) == 0) uart_start_tx();
		uart_tx_ready_changed();
		if (m_link_fast) link_schedule();
		return;
	}
	switch (u.flags) {
	case uart8251::NEXT_SYNC1: u.flags = uart8251::NEXT_SYNC2; break;
	case uart8251::NEXT_SYNC2: u.flags = uart8251::NEXT_COMMAND; break;
	case uart8251::NEXT_MODE:
		u.mode = data;
		u.br_factor = 1;
		u.flags = uart8251::NEXT_COMMAND;
		if (data & 3)
			u.br_factor = (data & 3) == 2 ? 16 : (data & 3) == 3 ? 64 : 1;
		else   // synchronous: sync characters follow (VF2 only passes through here on its reset)
			u.flags = (data & 0x80) ? uart8251::NEXT_SYNC2 : uart8251::NEXT_SYNC1;
		u.txc_count = 0;
		m_link_mode_edge = m_link_fast ? link_edges(time()) : 0;
		break;
	case uart8251::NEXT_COMMAND:
		u.command = data;
		if (data & 0x40) u.flags = uart8251::NEXT_MODE;
		if (u.tx_enabled() && u.tx_bits_left == 0 && (u.status & TX_READY) == 0) uart_start_tx();
		uart_tx_ready_changed();
		break;
	}
	if (m_link_fast) link_schedule();
}

// ---------------------------------------------------------------------------------------------
// The byte link (set_byte_link): what the bit-level link above does, at the moments it does it,
// with T_UART set for each one instead of every 1 us. TxC falls every 2 us from time 0; the
// transmitter steps on every br_factor-th fall from the mode write. A byte's start bit goes out
// at a step (t0), a bit a step; the SCSP's receiver (31250 baud: a bit is 32 us, its clock a
// toggle every 16 us, set off by the start bit's edge with a first delay of 32 us) reads the
// stop bit 19 toggles on, at t0 + 304 us, where the byte goes into its MIDI buffer. The step
// after the stop bit (t0 + 10 bits) starts the next byte or marks the transmitter empty; an idle
// step changes nothing unless a byte is waiting or the interrupt can be asked for again.
// ---------------------------------------------------------------------------------------------

namespace { constexpr vf2::uint128_t LINK_EDGE = 1000000000000000000ull / 500000; }   // TxC's period (2 us)

uint64_t model2a::link_edges(atime t) const { return div_atime(t, LINK_EDGE); }   // the falls up to t

model2a::atime model2a::link_next_step(atime t) const
{
	const uint64_t br = uint64_t(m_uart.br_factor), e = link_edges(t);
	const uint64_t n = (e - m_link_mode_edge) / br + 1;
	return atime(m_link_mode_edge + n * br) * LINK_EDGE;
}

void model2a::link_schedule()
{
	const uart8251 &u = m_uart;
	const atime now = time();
	atime when;
	if (u.tx_bits_left > 0) {
		const atime bit = atime(u.br_factor) * LINK_EDGE, half = (AS_PER_SEC / 31250) / 2;
		when = m_link_delivered ? m_link_t0 + 10 * bit : m_link_t0 + 2 * half + 17 * half;
	} else {
		const u32 line = 1 << 10;
		const bool start = (u.status & TX_READY) == 0 && (u.tx_enabled() || u.delayed_tx_en);
		const bool ask = (u.txrdy() || u.rxrdy()) && (m_intena & line) && !(m_intreq & line);
		if (!start && (u.status & TX_EMPTY) && !ask) {
			if (m_timer[T_UART].enabled) timer_disable(T_UART);
			return;
		}
		when = link_next_step(now);
	}
	if (m_timer[T_UART].enabled && m_timer[T_UART].expire == when) return;
	timer_adjust(T_UART, when > now ? when - now : 0);
}

void model2a::link_event()
{
	uart8251 &u = m_uart;
	if (u.tx_bits_left > 0 && !m_link_delivered) {   // the byte read at the sound board
		m_link_delivered = true;
		m_scsp->midi_in_byte(m_link_byte);
	} else {   // a step: after a byte's stop bit, or idle
		u.tx_bits_left = 0;
		m_link_in_step = true;
		if ((u.status & TX_READY) == 0 && (u.tx_enabled() || u.delayed_tx_en))
			uart_start_tx();
		else
			u.status |= TX_EMPTY;
		uart_tx_ready_changed();
		m_link_in_step = false;
	}
	link_schedule();
}


void model2a::reset_scheduler_stats(){
#if defined(VF2_SCHED_DIAGNOSTICS)
    m_scheduler_diagnostics={};
#endif
}
void model2a::report_scheduler_stats(FILE *out)const{
#if defined(VF2_SCHED_DIAGNOSTICS)
    const auto &d=m_scheduler_diagnostics;
    std::fprintf(out,"Scheduler: %llu calls, %llu passes, %llu timer-head scans\n",(unsigned long long)d.slices,(unsigned long long)d.passes,(unsigned long long)d.head_scans);
    std::fprintf(out,"Scheduler sync: %llu schedules, %llu single-line deliveries, %llu batch deliveries; %llu i960 and %llu TGP slice abort requests\n",
                 (unsigned long long)d.sync_calls,(unsigned long long)d.single_line,
                 (unsigned long long)d.batched_line,(unsigned long long)d.sync_abort[CPU_I960],
                 (unsigned long long)d.sync_abort[CPU_TGP]);
    const char *names[]={"i960","TGP","68K","Z80"};
    for(unsigned i=0;i<N_CPUS;i++)std::fprintf(out,"Scheduler %s: %llu visits, %llu dispatches, %llu active, %.2f budget/dispatch, %.2f cycles/dispatch, %llu early returns\n",names[i],(unsigned long long)d.visits[i],(unsigned long long)d.dispatches[i],(unsigned long long)d.active[i],d.dispatches[i]?double(d.budget[i])/d.dispatches[i]:0,d.dispatches[i]?double(d.advanced[i])/d.dispatches[i]:0,(unsigned long long)d.early[i]);
    for(unsigned i=0;i<N_TIMERS;i++)if(d.adjusted[i]||d.fired[i])std::fprintf(out,"Scheduler timer %u: %llu adjustments, %llu firings\n",i,(unsigned long long)d.adjusted[i],(unsigned long long)d.fired[i]);
#else
    (void)out;
#endif
}
#undef SCHED_COUNT

void model2a::collapse_i960_timer_poll(void *context,u32 limit,u32 &counter,int &budget)
{
    auto &board=*static_cast<model2a *>(context);
    if(!board.m_modular_frame||board.m_in_callback||board.m_executing!=CPU_I960||
            board.m_workram[0]||!board.m_timerrun[2]||
            board.m_sched[CPU_I960].period!=AS_PER_SEC/25000000||
            counter!=(board.m_timervals[2]&i960rec::timer_poll_mask))return;

    // The preceding real iteration sampled the countdown at the same cycle
    // offset. Update the board's latched timer value as the last read would.
    const auto elapsed=i960rec::timer_poll_elapsed(counter,limit,budget);
    board.m_timervals[2]-=elapsed;
    counter-=elapsed;
    budget-=int(elapsed);
}
