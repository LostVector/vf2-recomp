// license:BSD-3-Clause
// copyright-holders:R. Belmont
// Sega Model 1 sound board wiring, based on MAME's shared/segam1audio.cpp.
#include "../../third_party/mame/m68000/m68000.h"
#include "../../third_party/model1-multipcm/multipcm.h"
#include "../../third_party/ymfm/ymfm_opn.h"

struct model2a::original_sound_board : m68000_bus, ymfm::ymfm_interface
{
	static constexpr u32 FM_CLOCK = 8000000;
	static constexpr u32 PCM_CLOCK = 10000000;
	static constexpr u32 FM_DIVISOR = 144, PCM_DIVISOR = 224;
	static constexpr uint64_t NEVER = UINT64_MAX;

	explicit original_sound_board(model2a &board)
		: m(board), rom(m.m_roms.regions[romset::AUDIOCPU]),
		  pcm1(PCM_CLOCK, samples(0), sample_size()), pcm2(PCM_CLOCK, samples(1), sample_size())
	{
		ym.reset();
	}

    const u16 *verified_voice_scan_ram() const
    {
        static const bool enabled = std::getenv("DAYTONA_FAST_VOICE_SCAN") != nullptr;
        constexpr size_t firmware_size = 0x40000;
        constexpr uint32_t firmware_crc = 0xe3ee4434;
        if (!enabled || rom.size() < firmware_size ||
            crc32(0, rom.data(), firmware_size) != firmware_crc)
            return nullptr;
        return ram.data();
    }

	void receive(u8 byte)
	{
		if (!(uart_command & 4) || !m.m_sound_on) return;
		if (rx_ready) uart_errors |= 0x10;
		rx_data = byte;
		rx_ready = true;
		m.set_input_line(L_68K_IRQ0 + 2, ASSERT_LINE);
	}

	u16 peek(offs_t addr) override
	{
		addr &= ~1u;
		if (addr >= 0xf00000 && addr < 0xf10000) return ram[(addr - 0xf00000) >> 1];
		if (addr >= 0x080000 && addr < 0x0a0000) addr -= 0x060000;
		if (addr < rom.size()) return (u16(rom[addr]) << 8) | rom[addr + 1];
		return 0xffff;
	}

	u16 read(int space, offs_t addr, u16 mask) override
	{
		if (space == CPU_SPACE) return 0x18 + ((addr >> 1) & 7);
		if (!(mask & 0xff)) return peek(addr);
		addr &= ~1u;
		if (addr == 0xc20000) {
			rx_ready = false;
			m.set_input_line(L_68K_IRQ0 + 2, CLEAR_LINE);
			return rx_data;
		}
		if (addr == 0xc20002) return 0x85 | (rx_ready ? 2 : 0) | uart_errors;
		if ((addr & 0xfffff8) == 0xc40000 || (addr & 0xfffff8) == 0xc60000) return 0;
		if (addr >= 0xd00000 && addr < 0xd00008) {
			flush_fm(cycles());
			return ym.read((addr - 0xd00000) >> 1);
		}
		return peek(addr);
	}

	void write(offs_t addr, u16 data, u16 mask) override
	{
		addr &= ~1u;
		if (addr >= 0xf00000 && addr < 0xf10000) {
			u16 &storage = ram[(addr - 0xf00000) >> 1];
			storage = (storage & ~mask) | (data & mask);
			return;
		}
		if (!(mask & 0xff)) return;
		if (addr == 0xc20002) {
			if (uart_needs_mode) uart_needs_mode = false;
			else {
				uart_command = u8(data);
				uart_needs_mode = BIT(data, 6);
				if (BIT(data, 4)) uart_errors = 0;
			}
		} else if ((addr & 0xfffff8) == 0xc40000 || (addr & 0xfffff8) == 0xc60000) {
			flush_pcm(cycles());
			(BIT(addr, 17) ? pcm2 : pcm1).write((addr & 7) >> 1, u8(data));
		} else if (addr == 0xc50000 || addr == 0xc70000) {
			flush_pcm(cycles());
			(BIT(addr, 17) ? pcm2 : pcm1).set_bank(data);
		} else if (addr >= 0xd00000 && addr < 0xd00008) {
			flush_fm(cycles());
			ym.write((addr - 0xd00000) >> 1, u8(data));
		}
	}

	void update()
	{
		const uint64_t now = cycles();
		flush_fm(now);
		flush_pcm(now);
		const uint64_t end = now * SOUND_RATE / SOUND_CPU_CLOCK;
		while (output_samples < end) {
			++output_samples;
			const auto &f = fm.at(output_samples * FM_CLOCK / (SOUND_RATE * FM_DIVISOR));
			const auto &p = pcm.at(output_samples * PCM_CLOCK / (SOUND_RATE * PCM_DIVISOR));
			for (unsigned ch = 0; ch < 2; ++ch) {
				int value = int((0.30f * f[ch] + 0.50f * p[ch]) * 32768.0f);
				value = std::clamp(value, -32768, 32767);
				m.m_sound_peak = std::max(m.m_sound_peak, std::abs(value));
				m.m_audio_out.push_back(int16_t(value));
			}
		}
	}

	void ymfm_set_timer(uint32_t timer, int32_t clocks) override
	{
		fm_timers[timer] = clocks < 0 ? NEVER : fm_clock + uint32_t(clocks);
	}
	void ymfm_set_busy_end(uint32_t clocks) override { busy_end = fm_clock + clocks; }
	bool ymfm_is_busy() override { return fm_clock < busy_end; }

private:
	using stereo = std::array<float, 2>;
	struct samples_pending {
		std::deque<stereo> queue;
		stereo latest{};
		uint64_t generated = 0, consumed = 0;

		const stereo &at(uint64_t index)
		{
			while (consumed < index && !queue.empty()) {
				latest = queue.front();
				queue.pop_front();
				++consumed;
			}
			return latest;
		}
	};

	uint64_t cycles() const { return m.snd_cycles(); }
	u32 sample_size() const { return u32(m.m_roms.regions[romset::SAMPLES].size() / 2); }
	const u8 *samples(unsigned chip) const
	{
		const auto &data = m.m_roms.regions[romset::SAMPLES];
		return data.empty() ? nullptr : data.data() + chip * sample_size();
	}

	void generate_fm(uint64_t clock)
	{
		ymfm::ym3438::output_data data[64];
		const uint64_t due = clock / FM_DIVISOR;
		while (fm.generated < due) {
			const unsigned count = unsigned(std::min<uint64_t>(std::size(data), due - fm.generated));
			ym.generate(data, count);
			for (unsigned i = 0; i < count; ++i)
				fm.queue.push_back({data[i].data[0] / 32768.0f, data[i].data[1] / 32768.0f});
			fm.generated += count;
		}
	}

	void flush_fm(uint64_t now)
	{
		const uint64_t target = now * FM_CLOCK / SOUND_CPU_CLOCK;
		for (;;) {
			const unsigned timer = fm_timers[1] < fm_timers[0];
			const uint64_t next = fm_timers[timer];
			if (next > target) break;
			generate_fm(next);
			fm_clock = next;
			fm_timers[timer] = NEVER;
			m_engine->engine_timer_expired(timer);
		}
		generate_fm(target);
		fm_clock = target;
	}

	void flush_pcm(uint64_t now)
	{
		float left1[64], right1[64], left2[64], right2[64];
		const uint64_t due = now * PCM_CLOCK / (SOUND_CPU_CLOCK * uint64_t(PCM_DIVISOR));
		while (pcm.generated < due) {
			const int count = int(std::min<uint64_t>(std::size(left1), due - pcm.generated));
			pcm1.generate(left1, right1, count);
			pcm2.generate(left2, right2, count);
			for (int i = 0; i < count; ++i) pcm.queue.push_back({left1[i] + left2[i], right1[i] + right2[i]});
			pcm.generated += count;
		}
	}

	model2a &m;
	const std::vector<u8> &rom;
	std::array<u16, 0x8000> ram{};
	u8 uart_command = 0, uart_errors = 0, rx_data = 0;
	bool uart_needs_mode = true, rx_ready = false;
	snd::MultiPcm pcm1, pcm2;
	std::array<uint64_t, 2> fm_timers{NEVER, NEVER};
	uint64_t fm_clock = 0, busy_end = 0, output_samples = 0;
	ymfm::ym3438 ym{*this};
	samples_pending fm, pcm;
};
