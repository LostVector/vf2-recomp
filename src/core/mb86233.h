// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// Fujitsu MB86233 (TGP): MAME's core without the device framework (BSD-3-Clause,
// copyright-holders: Olivier Galibert).
#pragma once
#include "mame_shim.h"
#include <span>

// The TGP's four address spaces, word-addressed (one 32-bit word per address).
struct mb86233_bus
{
	struct internal_ram {
        u32 *low=nullptr,*high=nullptr;
        // Optional contiguous 0000-03ff view. The unmapped 0100-01ff
        // words read as zero and must never be written.
        const u32 *read_window=nullptr;
    };
	virtual ~mb86233_bus() = default;
	virtual u32 prog_r(u32 a) = 0;
	// Optional side-effect-free, 4096-word program view, equivalent to prog_r.
	virtual const u32 *program_data()const {return nullptr;}
	// Nonzero opts into a view stable for an entire run() without a debugger
	// hook. Every program mutation/load/reset must change this revision.
	virtual uint64_t program_revision()const{return 0;}
	// Optional fixed Model 2 internal windows: 0000-00ff and 0200-03ff.
	// Both arrays remain at these addresses for the bus lifetime; other data
	// reads return zero and writes have no effect. No access side effects.
	virtual internal_ram direct_internal_ram(){return {};}
    // Opt-in for deferred private arithmetic: no other device or bus client
    // reads/writes these internal windows between run() calls. Inspectors must
    // materialize the CPU first; reset/state operations do so automatically.
    virtual bool isolated_internal_ram()const{return false;}
    // Opt-in, stable throughout run(): I/O/RF callbacks observe only PC/cycle
    // accounting and GPIO/control, never CPU datapath/address/status registers
    // or state snapshots. They may abort a slice, assert stall on reads, and
    // change GPIO. They cannot reset/re-enter the CPU or change its program.
    // Other buses retain fully materialized instruction-handler callbacks.
    virtual bool native_control_callbacks()const{return false;}
	virtual u32 data_r(u32 a) = 0;
	virtual void data_w(u32 a, u32 v) = 0;
	virtual u32 io_r(u32 a) = 0;
	virtual void io_w(u32 a, u32 v) = 0;
	virtual u32 rf_r(u32 a) { return 0; }
	virtual void rf_w(u32 a, u32 v) {}
};

class mb86233_device
{
public:
	explicit mb86233_device(mb86233_bus &bus) : m_bus(&bus) {}
	enum regs_ids {
		REG_A, REG_B, REG_D, REG_P,
		REG_R, REG_C0, REG_C1,
		REG_B0, REG_B1, REG_X0, REG_X1, REG_I0, REG_I1,
		REG_SFT, REG_VSM, REG_MASK, REG_M,
		REG_PCS0, REG_PCS1, REG_PCS2, REG_PCS3,
		REG_SP
	};

	enum st_flags {
		F_ZRC  = 0x00000001,
		F_ZRD  = 0x00000002,
		F_SGC  = 0x00000004,
		F_SGD  = 0x00000008,
		F_CPC  = 0x00000010,
		F_CPD  = 0x00000020,
		F_OVC  = 0x00000040,
		F_OVD  = 0x00000080,
		F_UNC  = 0x00000100,
		F_UND  = 0x00000200,
		F_DVZC = 0x00000400,
		F_DVZD = 0x00000800,
		F_CA   = 0x00001000,
		F_CPP  = 0x00002000,
		F_OVM  = 0x00004000,
		F_UNM  = 0x00008000,
		F_SIF0 = 0x00010000,
		F_SIF1 = 0x00020000,
		F_SOF0 = 0x00040000,

		F_PIF  = 0x00100000,
		F_POF  = 0x00200000,
		F_PAIF = 0x00400000,
		F_PAOF = 0x00800000,
		F_F0S  = 0x01000000,
		F_F1S  = 0x02000000,
		F_IT   = 0x04000000,
		F_ZX0  = 0x08000000,
		F_ZX1  = 0x10000000,
		F_ZX2  = 0x20000000,
		F_ZC0  = 0x40000000,
		F_ZC1  = 0x80000000
	};


	void reset();
	// MAME's execute_run with icount = cycles; returns the cycles used (one per instruction).
	int run(int cycles) { m_icount = m_cycles_running = cycles; execute_run(); return m_cycles_running - m_icount; }
	// For a bus with an isolated internal RAM and a fixed, side-effect-free
	// program mapping. Increment revision whenever that image changes.
	int run_mapped(int cycles, std::span<const u32,4096> program, const uint64_t &revision)
	{
		m_icount = m_cycles_running = cycles;
		execute_run(program.data(), &revision);
		return m_cycles_running - m_icount;
	}
	int cycles_used() const { return m_cycles_running - m_icount; }
	// MAME's abort_timeslice: the run ends after this instruction, the rest isn't spent.
	void abort_timeslice() { const int left = m_icount; m_cycles_running -= left; m_icount -= left; }
	// An input FIFO was empty: the instruction is retried (MAME's stall()).
	void stall() { m_stall = true; }
	u16 pc() const { const_cast<mb86233_device*>(this)->materialize_native_matrix();return m_pc; }
    void materialize_native_matrix();
	u32 program_word(u16 address) const { return m_bus->prog_r(address); }
	bool precompiled() const;
	uint64_t compiled_instructions() const { return m_compiled_instructions; }
	uint64_t interpreted_instructions() const { return m_interpreted_instructions; }
	uint64_t compiled_blocks_run()const{return m_blocks_run;}
	uint64_t block_instructions()const{return m_block_instructions;}
	uint64_t native_matrices()const{return m_native_matrices;}
    uint64_t resumed_matrices()const{return m_resumed_matrices;}
    uint64_t native_instructions()const{return m_native_instructions;}
    uint64_t native_entries()const{return m_native_entries;}
    uint64_t native_helpers()const{return m_native_helpers;}
	std::function<void(u16)> on_instruction;   // before each instruction (the debugger's hook)

	// Save states (src/machine/state_io.h): every register, run both ways; power_on: a cold start's, reset()
	// after it (reset() leaves the GPIO lines as they were)
	template <class IO> void state(IO &io)
	{
        materialize_native_matrix();
		io.pod(m_cycles_running); io.pod(m_icount);
		io.pod(m_st); io.pod(m_a); io.pod(m_b); io.pod(m_d); io.pod(m_p);
		io.pod(m_alu_stmask); io.pod(m_alu_stset); io.pod(m_alu_r1); io.pod(m_alu_r2);
		io.pod(m_ppc); io.pod(m_pc); io.pod(m_sp); io.pod(m_b0); io.pod(m_b1); io.pod(m_x0); io.pod(m_x1); io.pod(m_i0); io.pod(m_i1);
		io.pod(m_vsmr); io.pod(m_pcs); io.pod(m_mask); io.pod(m_m);
		io.pod(m_r); io.pod(m_rpc); io.pod(m_c0); io.pod(m_c1); io.pod(m_sft); io.pod(m_vsm);
		io.pod(m_gpio0); io.pod(m_gpio1); io.pod(m_gpio2); io.pod(m_gpio3);
		io.pod(m_stall);
	}
	void power_on() { m_cycles_running = m_icount = 0; m_gpio0 = m_gpio1 = m_gpio2 = m_gpio3 = false; reset(); }

	void gpio0_w(int state) { m_gpio0 = state; }
	void gpio1_w(int state) { m_gpio1 = state; }
	void gpio2_w(int state) { m_gpio2 = state; }
	void gpio3_w(int state) { m_gpio3 = state; }

private:
	friend struct mb86233_matrix_fixture;
    friend struct mb86233_native_generator;
    friend unsigned vf2_tgp_native(mb86233_device &);
	mb86233_bus *m_bus;
	mb86233_bus::internal_ram m_fast_ram;
	bool m_fast_ram_checked=false;
	template<bool Private=false>
	__attribute__((always_inline)) inline u32 data_read(u32 a){
        if(const auto *window=m_fast_ram.read_window){
            const u16 address=u16(a);
            return window[address<0x400?address:0x100];
        }
		if(Private||m_fast_ram.low){a&=0xffff;if(a<0x100)return m_fast_ram.low[a];if(a-0x200<0x200)return m_fast_ram.high[a-0x200];return 0;}
		return m_bus->data_r(a);
	}
	template<bool Private=false>
	__attribute__((always_inline)) inline void data_write(u32 a,u32 value){
		if(Private||m_fast_ram.low){a&=0xffff;if(a<0x100)m_fast_ram.low[a]=value;else if(a-0x200<0x200)m_fast_ram.high[a-0x200]=value;return;}
		m_bus->data_w(a,value);
	}
	int m_cycles_running = 0;
	void execute_run();
	void execute_run(const u32 *mapped, const uint64_t *mapped_revision);
    void execute_run_impl(const u32 *mapped, const uint64_t *mapped_revision);
	template<bool Private=false,class Opcode> void execute_one(Opcode opcode);
	template<u32 Opcode> static void compiled_instruction(mb86233_device *cpu);
    static void command_intake_entry(mb86233_device *cpu);
    static void command_dispatch_entry(mb86233_device *cpu);
    template<u16 First,u32 Opcode> static void command_suffix_entry(mb86233_device *cpu);
    template<u16 First> unsigned finish_command_dispatch(int prior);
    template<u8 Command,u32 Opcode> static void command_entry(mb86233_device *cpu);
    template<u16 First,u32... Opcodes> static void private_region_entry(mb86233_device *cpu);
    template<u32 Opcode> void private_step();
    template<bool ExactFloating,u16 First,u32... Opcodes> static void private_region_entry_impl(mb86233_device *cpu);
    template<u16 First,u32... Opcodes> static void private_4a_resume_entry(mb86233_device *cpu);
    static void command37_setup_entry(mb86233_device *cpu);
    static void command37_finish_entry(mb86233_device *cpu);
    bool command37_finish();
    template<u16 First,u32... Opcodes> static void command32_region_entry(mb86233_device *cpu);
	static void matrix_entry(mb86233_device *cpu);
	bool native_matrix();
    struct matrix_timing {u16 entry;u8 instructions;u8 prefix[59];};
    static const matrix_timing *native_matrix_timing(u16 entry);
    bool start_native_matrix_pending();
    void advance_native_matrix_pending();
    void save_native_matrix_state(u16 output_address,unsigned output_words,
                                  u16 other_address=0,unsigned other_words=0);
    void activate_native_matrix_pending(const matrix_timing *timing,uint64_t instructions);
    u32 *native_ram_at(u16 address){
        return address<0x200 ? m_fast_ram.low+address : m_fast_ram.high+(address-0x200);
    }
    struct pending_matrix {
        bool active=false;u8 issued=0;const matrix_timing *timing=nullptr;
        uint64_t revision=0;const u32 *program=nullptr;
        struct original {
            u32 st,a,b,d,p,mask,set,r1,r2;
            u16 b0,b1,x0,x1;
            u32 output[12],other[3];
        } initial{};
        u16 output_address=0,other_address=0;
        u8 output_words=0,other_words=0;
    };
	struct compiled_entry {u32 opcode;void (*run)(mb86233_device*);};
	static const compiled_entry *compiled_program();
	template<u32 Opcode> bool block_step(u16 expected);
	template<u32 A,u32 B,u32 C,u32 D,u32 E,u32 F,u32 G,u32 H>
	static unsigned compiled_block(mb86233_device *cpu,unsigned offset);
	struct compiled_block_entry {unsigned (*run)(mb86233_device*,unsigned);};
	static const compiled_block_entry *compiled_blocks();
	int m_icount = 0;

	u32 m_st, m_a, m_b, m_d, m_p;
	u32 m_alu_stmask, m_alu_stset, m_alu_r1, m_alu_r2;
	u16 m_ppc, m_pc, m_sp, m_b0, m_b1, m_x0, m_x1, m_i0, m_i1, m_vsmr, m_pcs[4], m_mask, m_m;
	u8 m_r, m_rpc, m_c0, m_c1, m_sft, m_vsm;
	bool m_gpio0 = false, m_gpio1 = false, m_gpio2 = false, m_gpio3 = false;

	bool m_stall;
	uint64_t m_compiled_instructions=0,m_interpreted_instructions=0;
	uint64_t m_blocks_run=0,m_block_instructions=0;
	uint8_t m_sample_command=255; // diagnostic only, excluded from architectural state
	bool m_native_matrix_allowed=false;
	uint64_t m_native_matrices=0;
	uint64_t m_checked_revision=0;
	const u32 *m_checked_program=nullptr;
	bool m_block_program_matches=false;
    // Keep the original hot DSP registers at their compact ARM load offsets.
    // Deferred execution bookkeeping belongs after the architectural fields.
    pending_matrix m_pending_matrix;
    bool m_native_resume_allowed=false;
    bool m_private_regions_allowed=false;
    uint64_t m_resumed_matrices=0;
    uint64_t m_native_instructions=0,m_native_entries=0,m_native_helpers=0;
    uint32_t m_native_helper_slice=0;

	static u32 set_exp(u32 val, u32 exp);
	static u32 set_mant(u32 val, u32 mant);
	static u32 get_exp(u32 val);
	static u32 get_mant(u32 val);

	void alu_update_st();
	void alu_pre(u32 alu);
	void alu_post_1(u32 alu);
	template<bool Private> void alu_post_2(u32 alu);
	u16 ea_pre_0(u32 r);
	void ea_post_0(u32 r);
	u16 ea_pre_1(u32 r);
	void ea_post_1(u32 r);
	void pcs_push();
	void pcs_pop();
	inline void stset_set_sz_int(u32 val);
	inline void stset_set_sz_fp(u32 val);

	u32 read_reg(u32 r);
	void write_reg(u32 r, u32 v);
	template<bool Private> void write_mem_internal_1(u32 r, u32 v, bool bank);
	void write_mem_external_1(u32 r, u32 v);
	void write_mem_io_1(u32 r, u32 v);
    template<u32 Opcode> static void compiled_repeat_move(mb86233_device *cpu);
    template<unsigned FirstColumn,unsigned SecondColumn,u32 Opcode> static void rotation_entry(mb86233_device *cpu);
    template<unsigned FirstColumn,unsigned SecondColumn> bool native_rotation();
    template<unsigned FirstColumn,unsigned SecondColumn> bool start_native_rotation_pending();
    template<bool WriteMatrix> bool native_translation();
    template<bool WriteMatrix> bool start_native_translation_pending();
    template<bool WriteMatrix,u32 Opcode> static void translation_entry(mb86233_device *cpu);
    uint64_t m_native_rotations=0,m_resumed_rotations=0;
    uint64_t m_native_translations=0,m_resumed_translations=0;
public:
    uint64_t native_rotations()const{return m_native_rotations;}
    uint64_t resumed_rotations()const{return m_resumed_rotations;}
    uint64_t native_translations()const{return m_native_translations;}
    uint64_t resumed_translations()const{return m_resumed_translations;}
};
