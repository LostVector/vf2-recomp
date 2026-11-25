// The native recompile (see i960_native.h). The whole program is one piece of code: a label at
// every instruction, entered at the one for IP, the flow between instructions as native jumps
// (into another function's code too: they share one frame), and back to the runtime's loop when
// the slice's cycles run out, an access stalls, or IP leads where no code was made.
//
// Each instruction stores PIP and IP as the interpreter has them, then does its work: through a
// call to execute_op's case for its opcode (i960_ops.h, made into one function per opcode and
// sub-opcode, the operands decoded from the word), or as native code for the common ones, with
// the same cycle counts and memory accesses as the interpreter's. After it: the stall check (a
// FIFO read retried by the loop, as the interpreter retries it), the cycle check, the flow.
#include "i960_native.h"
#include "i960_ops.h"
#include "../../third_party/sljit/sljitLir.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

namespace i960rec {

namespace {

using R = i960_rec;
using C = i960_device;

// ---- what the native code calls ----

// execute_op's case for one opcode (and, for the REG format, sub-opcode): the rest of the
// instruction word comes as an argument, the opcode is folded in
template <uint32_t MAJ, uint32_t SUB>
void SLJIT_FUNC h_op(C *c, sljit_u32 op, sljit_u32 disp)
{
	const uint32_t w = (MAJ >= 0x58 && MAJ < 0x80) ? (op & ~0xff000780u) | (MAJ << 24) | (SUB << 7) : (op & 0x00ffffffu) | (MAJ << 24);
	R::exec(*c, w, disp);
}

using op_fn = void(SLJIT_FUNC *)(C *, sljit_u32, sljit_u32);

template<unsigned Size,bool Signed=false>
void SLJIT_FUNC h_bus_read(C *c,sljit_u32 address,sljit_u32 destination){
    u32 value;
    if constexpr(Size==4)value=R::rd32<false>(*c,address);
    else if constexpr(Size==2){
        const u16 raw=R::rd16<false>(*c,address);
        value=Signed?u32(s32(s16(raw))):raw;
    }else{
        const u8 raw=R::rd8<false>(*c,address);
        value=Signed?u32(s32(s8(raw))):raw;
    }
    if(!R::stalled(*c))R::regs(*c)[destination]=value;
}
template<unsigned Size>
void SLJIT_FUNC h_bus_write(C *c,sljit_u32 address,sljit_u32 value){
    if constexpr(Size==4)R::wr32<false>(*c,address,value);
    else if constexpr(Size==2)R::wr16<false>(*c,address,u16(value));
    else R::wr8<false>(*c,address,u8(value));
}
template<int Words, bool Store>
void SLJIT_FUNC h_bus_multi(C *c,sljit_u32 address,sljit_u32 first_register){
    if constexpr(Store)R::st_multi(*c,address,first_register,Words);
    else R::ld_multi(*c,address,first_register,Words);
}
op_fn g_ops[256][16];

template <uint32_t MAJ, uint32_t... SUB> void fill_subs(std::integer_sequence<uint32_t, SUB...>)
{
	((g_ops[MAJ][SUB] = &h_op<MAJ, SUB>), ...);
}
template <uint32_t MAJ> void fill_one()
{
	if constexpr (MAJ >= 0x58 && MAJ < 0x80) fill_subs<MAJ>(std::make_integer_sequence<uint32_t, 16>());
	else g_ops[MAJ][0] = &h_op<MAJ, 0>;
}
template <uint32_t... MAJ> void fill_ops(std::integer_sequence<uint32_t, MAJ...>) { (fill_one<MAJ>(), ...); }
// (the opcodes execute_op has: no others are compiled)
using all_majors = std::integer_sequence<uint32_t,
	0x08, 0x09, 0x0a, 0x0b, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x39, 0x3a, 0x3b, 0x3c,
	0x3d, 0x3e, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6c, 0x6d, 0x6e,
	0x70, 0x74, 0x78, 0x79, 0x80, 0x82, 0x84, 0x85, 0x86, 0x88, 0x8a, 0x8c, 0x90, 0x92, 0x98, 0x9a, 0xa0, 0xa2, 0xb0, 0xb2,
	0xc0, 0xc2, 0xc8, 0xca>;

op_fn op_for(uint32_t w)
{
	static const bool filled = (fill_ops(all_majors()), true);
	(void)filled;
	const uint32_t maj = w >> 24;
	return g_ops[maj][(maj >= 0x58 && maj < 0x80) ? (w >> 7) & 0xf : 0];
}

// Where the code for IP is (0: none: back to the loop)
sljit_uw SLJIT_FUNC h_label(C *c)
{
	const runtime &rt = *c->rec();
	const uint32_t ip = R::ip(*c);
	return ip < runtime::LIMIT && !(ip & 3) ? rt.nlabel[ip >> 2] : 0;
}

void SLJIT_FUNC h_timer_poll(C *c)
{
    const auto &rt=*c->rec();
    auto *regs=R::regs(*c);
    rt.collapse_timer_poll(rt.timer_poll_context,regs[13],regs[15],R::icount(*c));
}

// A spin loop's skip (see program::spins): round again while the cycles last, all but the last
// time skipped, when the memory it reads is plain
void SLJIT_FUNC h_spin(C *c, sljit_u32 cost, sljit_uw loads)
{
	const auto &ls = *reinterpret_cast<const std::vector<uint32_t> *>(loads);
	int &ic = R::icount(*c);
	if (ic <= int(cost)) return;
	for (uint32_t a : ls) if (!R::rp(*c, a) || !R::rp(*c, a + 3)) return;
	ic -= (ic - 1) / int(cost) * int(cost);
}

struct gen
{
	sljit_compiler *C_ = nullptr;
	const program &p;
	runtime &rt;
	C &cpu;
	native_stats &st;
	std::unordered_map<uint32_t, sljit_label *> labels;
	std::vector<std::pair<sljit_jump *, uint32_t>> to_label;
	std::vector<sljit_jump *> to_exit, to_dispatch;
    struct pc_exit { sljit_jump *jump; uint32_t pip,ip; sljit_label *label=nullptr; };
    std::vector<pc_exit> lazy_exits;
    const bool lazy_pc_ok=std::getenv("VF2_REC_LAZY_PC")!=nullptr;
    bool lazy_pc_active=false;
    const bool scalar32=std::getenv("VF2_REC_SCALAR32")!=nullptr;
    const bool native_arithmetic=std::getenv("VF2_REC_NATIVE_ARITHMETIC")!=nullptr;
    const bool register_cycles=std::getenv("VF2_REC_REG_CYCLES")!=nullptr;
    const bool direct_branches=std::getenv("VF2_REC_DIRECT_BRANCHES")!=nullptr;
    const bool predecoded_bursts=std::getenv("VF2_REC_PREDECODED_BURSTS")!=nullptr;
    const bool bus_address=std::getenv("VF2_REC_BUS_ADDRESS")!=nullptr;
    const bool port_stores=std::getenv("VF2_REC_PORT_STORES")!=nullptr;

    bool is_hot_port_store(const decoded &d) const {
        // VF2 sets g10/g11 to the geometry/coprocessor windows and g12
        // to the FIFO offset before these routines. The address still comes
        // from the live registers and the normal bus handles every value.
        constexpr uint32_t without_source = ~0x00f80000u;
        const uint32_t form = d.op & without_source;
        if (form != (0x927edc1cu & without_source) && form != (0x927e9c1cu & without_source))
            return false;
        const int owner = p.owner_of(d.addr);
        if (owner < 0) return false;
        const uint32_t entry = p.functions[owner].entry;
        return entry == 0x16504 || entry == 0x1d320;
    }

	sljit_sw o_ip = 0, o_pip = 0, o_icount = 0, o_stalled = 0;
	std::map<uint32_t, sljit_sw> spun;   // a spin loop's head -> its flag on the stack: it has run since the code was entered
    static constexpr uint32_t timer_poll_head=0x4cb7c,timer_poll_branch=0x4cba0,timer_poll_exit=0x4cba4;
    bool timer_poll=false;
    bool matches_timer_poll()const{
        struct instruction {uint32_t address,opcode,disp;};
        constexpr instruction expected[]={
            {timer_poll_head,0x80703000,0x500000},{0x4cb84,0x3503a020,0},
            {0x4cb88,0x98703000,0xf00008},{0x4cb90,0x8c703000,0xfffff},
            {0x4cb98,0x587b808f,0},{0x4cb9c,0x327b8008,0},
            {timer_poll_branch,0x317b5fdc,0}
        };
        for(const auto &i:expected){
            if(!compiled(i.address))return false;
            const auto &d=p.insns.at(i.address);
            if(d.op!=i.opcode||(d.len==8&&d.disp!=i.disp))return false;
        }
        return compiled(timer_poll_exit);
    }

	sljit_sw off(const void *field) const { return reinterpret_cast<const char *>(field) - reinterpret_cast<const char *>(&cpu); }

	bool compiled(uint32_t a) const
	{
		auto it = p.insns.find(a);
		return it != p.insns.end() && it->second.k != kind::bad && p.owner.count(a);
	}
	void go(uint32_t t)   // IP is t: carry on there
	{
		if (compiled(t)) to_label.push_back({ sljit_emit_jump(C_, SLJIT_JUMP), t });
		else to_dispatch.push_back(sljit_emit_jump(C_, SLJIT_JUMP));
	}
	void load_ip() { sljit_emit_op1(C_, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_ip); }
	void if_ip(uint32_t t)   // IP (in R0) == t: carry on there
	{
		if (compiled(t)) to_label.push_back({ sljit_emit_cmp(C_, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, t), t });
		else to_dispatch.push_back(sljit_emit_cmp(C_, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, t));
	}

	// ---- the common instructions as native code (execute_op's cases, the same cycles) ----
	const bool native_ok = getenv("VF2_REC_CALLS") == nullptr;   // testing: every instruction through execute_op's case
	sljit_sw o_r = 0, o_ac = 0, o_fp = 0;
	uint8_t *const *rpage = nullptr, *const *wpage = nullptr;

    void commit_pc(uint32_t pip,uint32_t ip){
        sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_MEM1(SLJIT_S0),o_pip,SLJIT_IMM,pip);
        sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_MEM1(SLJIT_S0),o_ip,SLJIT_IMM,ip);
    }
    void flush_cycles(){
        if(register_cycles)sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_MEM1(SLJIT_S0),o_icount,SLJIT_S1,0);
    }
    void reload_cycles(){
        if(register_cycles)sljit_emit_op1(C_,SLJIT_MOV_S32,SLJIT_S1,0,SLJIT_MEM1(SLJIT_S0),o_icount);
    }
	void call_op(const decoded &d)
	{
        flush_cycles();
        if(lazy_pc_active)commit_pc(d.addr,d.addr+uint32_t(d.len));
		sljit_emit_op1(C_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
		sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(d.op));
		sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, sljit_sw(d.len == 8 ? d.disp : 0));
		sljit_emit_icall(C_, SLJIT_CALL, SLJIT_ARGS3V(P, 32, 32), SLJIT_IMM, SLJIT_FUNC_ADDR(op_for(d.op)));
        reload_cycles();
	}
    // R1 is the effective address; R2 is a register index or scalar store value.
    void call_bus(op_fn helper,int cycles){
        ic_sub(cycles);
        flush_cycles();
        sljit_emit_op1(C_,SLJIT_MOV,SLJIT_R0,0,SLJIT_S0,0);
        sljit_emit_icall(C_,SLJIT_CALL,SLJIT_ARGS3V(P,32,32),SLJIT_IMM,SLJIT_FUNC_ADDR(helper));
        reload_cycles();
    }
	void stall_check()
	{
		sljit_emit_op1(C_, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), o_stalled);
		to_exit.push_back(sljit_emit_cmp(C_, SLJIT_NOT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0));
	}
	sljit_sw reg(int i) const { return o_r + 4 * i; }
	void ld_r(sljit_s32 R, int i) { sljit_emit_op1(C_, SLJIT_MOV_U32, R, 0, SLJIT_MEM1(SLJIT_S0), reg(i)); }
	void st_r(int i, sljit_s32 R) { sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), reg(i), R, 0); }
    void ic_sub(int k) {
        if(register_cycles)sljit_emit_op2(C_,SLJIT_SUB32,SLJIT_S1,0,SLJIT_S1,0,SLJIT_IMM,k);
        else sljit_emit_op2(C_,SLJIT_SUB32,SLJIT_MEM1(SLJIT_S0),o_icount,SLJIT_MEM1(SLJIT_S0),o_icount,SLJIT_IMM,k);
    }
	void src1(sljit_s32 R, uint32_t op) { if (op & 0x800) sljit_emit_op1(C_, SLJIT_MOV32, R, 0, SLJIT_IMM, op & 0x1f); else ld_r(R, op & 0x1f); }
	void src2(sljit_s32 R, uint32_t op) { if (op & 0x1000) sljit_emit_op1(C_, SLJIT_MOV32, R, 0, SLJIT_IMM, (op >> 14) & 0x1f); else ld_r(R, (op >> 14) & 0x1f); }
	// get_ea into R1 (uses R0 too)
	bool ea(const decoded &d)
	{
		const uint32_t op = d.op;
		const int abase = (op >> 14) & 0x1f, index = op & 0x1f, scale = (op >> 7) & 7;
		if (!(op & 0x1000)) {
			const uint32_t offset = op & 0xfff;
			if (op & 0x2000) {
				ld_r(SLJIT_R1, abase);
				if (offset) sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, offset);
			} else
				sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, offset);
			return true;
		}
		auto indexed = [&](sljit_s32 R) {   // R = r[index] << scale
			ld_r(R, index);
			if (scale) sljit_emit_op2(C_, SLJIT_SHL32, R, 0, R, 0, SLJIT_IMM, scale);
		};
		switch ((op >> 10) & 0xf) {
		case 0x4: ld_r(SLJIT_R1, abase); return true;
		case 0x5: sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(uint32_t(d.disp + d.addr + 8))); return true;
		case 0x7: indexed(SLJIT_R0); ld_r(SLJIT_R1, abase); sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0); return true;
		case 0xc: sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(d.disp)); return true;
		case 0xd: ld_r(SLJIT_R1, abase); sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(d.disp)); return true;
		case 0xe: indexed(SLJIT_R1); sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(d.disp)); return true;
		case 0xf:
			indexed(SLJIT_R0); ld_r(SLJIT_R1, abase);
			sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
			sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(d.disp));
			return true;
		default: return false;
		}
	}
	// R2 = the page's memory for the address in R1 (u32), else a jump to `slow` (pushed): plain,
	// below the pages' limit, aligned to `align`
	void page(uint8_t *const *table, int align, std::vector<sljit_jump *> &slow)
	{
		if (align > 1) {
			sljit_emit_op2u(C_, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_R1, 0, SLJIT_IMM, align - 1);
			slow.push_back(sljit_emit_jump(C_, SLJIT_NOT_ZERO));
		}
		slow.push_back(sljit_emit_cmp(C_, SLJIT_GREATER_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, sljit_sw(runtime::PAGES << 12)));
		sljit_emit_op2(C_, SLJIT_LSHR32, SLJIT_R3, 0, SLJIT_R1, 0, SLJIT_IMM, 12);
		sljit_emit_op1(C_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, sljit_sw(table));
		sljit_emit_op1(C_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R3), SLJIT_WORD_SHIFT);
		slow.push_back(sljit_emit_cmp(C_, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0));
		sljit_emit_op2(C_, SLJIT_AND32, SLJIT_R3, 0, SLJIT_R1, 0, SLJIT_IMM, 0xfff);
	}
	// AC's condition code from comparing R0 (src1) with R1 (src2): 4 less, 2 equal, 1 greater
	void compare(bool sig)
	{
		sljit_emit_op2u(C_, SLJIT_SUB32 | SLJIT_SET_Z | (sig ? SLJIT_SET_SIG_LESS : SLJIT_SET_LESS), SLJIT_R0, 0, SLJIT_R1, 0);
		sljit_emit_op_flags(C_, SLJIT_MOV32, SLJIT_R2, 0, sig ? SLJIT_SIG_LESS : SLJIT_LESS);
		sljit_emit_op_flags(C_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_ZERO);
		sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 1);
		sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R2, 0);
		sljit_emit_op2(C_, SLJIT_SHL32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1);
		sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R2, 0);   // 1 + Z + 3 * less
		set_cc(SLJIT_R3);
	}
	void set_cc(sljit_s32 R)   // AC = (AC & ~7) | R
	{
		sljit_emit_op1(C_, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0), o_ac);
		sljit_emit_op2(C_, SLJIT_AND32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, ~7);
		sljit_emit_op2(C_, SLJIT_OR32, SLJIT_R2, 0, SLJIT_R2, 0, R, 0);
		sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), o_ac, SLJIT_R2, 0);
	}
    // For BNO, the condition is that all three condition bits are clear.
    sljit_jump *test_cc_false(uint32_t mask,bool have_compare_result=false)
    {
        if(!have_compare_result)
            sljit_emit_op1(C_,SLJIT_MOV_U32,SLJIT_R2,0,SLJIT_MEM1(SLJIT_S0),o_ac);
        sljit_emit_op2u(C_,SLJIT_AND32|SLJIT_SET_Z,
                       have_compare_result?SLJIT_R3:SLJIT_R2,0,SLJIT_IMM,mask?mask:7);
        return sljit_emit_jump(C_, mask ? SLJIT_ZERO : SLJIT_NOT_ZERO);
    }
    void branch_if(uint32_t mask, uint32_t target)
    {
        auto *no=test_cc_false(mask);
		sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), o_ip, SLJIT_IMM, sljit_sw(target));
		sljit_set_label(no, sljit_emit_label(C_));
	}

    void branch_operands(uint32_t op)
    {
        const unsigned first=(op>>19)&31;
        if(op&0x2000)sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_R0,0,SLJIT_IMM,first);
        else ld_r(SLJIT_R0,first);
        ld_r(SLJIT_R1,(op>>14)&31);
    }

    // The raw bit tested by BBC/BBS is left in R1.
    void branch_bit(uint32_t op)
    {
        branch_operands(op);
        sljit_emit_op2(C_,SLJIT_AND32,SLJIT_R0,0,SLJIT_R0,0,SLJIT_IMM,31);
        sljit_emit_op2(C_,SLJIT_LSHR32,SLJIT_R1,0,SLJIT_R1,0,SLJIT_R0,0);
        sljit_emit_op2(C_,SLJIT_AND32,SLJIT_R1,0,SLJIT_R1,0,SLJIT_IMM,1);
    }

    sljit_jump *check_budget()
    {
        if(!register_cycles)
            sljit_emit_op1(C_,SLJIT_MOV_S32,SLJIT_R0,0,SLJIT_MEM1(SLJIT_S0),o_icount);
        return sljit_emit_cmp(C_,SLJIT_SIG_LESS_EQUAL|SLJIT_32,
                             register_cycles?SLJIT_S1:SLJIT_R0,0,SLJIT_IMM,0);
    }

    void direct_edge(uint32_t source,uint32_t target,bool fall_through)
    {
        lazy_exits.push_back({check_budget(),source,target});
        if(!compiled(target))commit_pc(source,target);
        if(!fall_through)go(target);
    }

    bool emit_direct_branch(const decoded &d,uint32_t next_emitted)
    {
        const unsigned op=d.op>>24;
        if(d.k==kind::jump && op==0x08 && d.has_target){
            ic_sub(1);
            direct_edge(d.addr,d.target,next_emitted==d.target);
            return true;
        }
        if(d.k!=kind::branch || !d.has_target || p.spins.count(d.addr) ||
           (timer_poll && d.addr==timer_poll_branch))return false;

        sljit_jump *not_taken;
        if(op>=0x10 && op<=0x17){
            ic_sub(1);
            not_taken=test_cc_false(op&7);
        }else if((op>=0x31 && op<=0x36) || (op>=0x39 && op<=0x3e)){
            ic_sub(4);
            branch_operands(d.op);
            compare(op>=0x39);
            not_taken=test_cc_false(op&7,true);
        }else if(op==0x30 || op==0x37){
            ic_sub(4);
            branch_bit(d.op);
            if(op==0x30)sljit_emit_op2(C_,SLJIT_XOR32,SLJIT_R1,0,SLJIT_R1,0,SLJIT_IMM,1);
            sljit_emit_op2(C_,SLJIT_SHL32,SLJIT_R3,0,SLJIT_R1,0,SLJIT_IMM,1);
            set_cc(SLJIT_R3);
            not_taken=sljit_emit_cmp(C_,SLJIT_EQUAL|SLJIT_32,SLJIT_R1,0,SLJIT_IMM,0);
        }else return false;

        // Both branches have committed AC and debited the same guest cycles.
        // Materialize the architectural PC only on a slice or native-code exit.
        direct_edge(d.addr,d.target,false);
        sljit_set_label(not_taken,sljit_emit_label(C_));
        const uint32_t next=d.addr+uint32_t(d.len);
        direct_edge(d.addr,next,next_emitted==next);
        return true;
    }

    void floating_source(sljit_s32 target,uint32_t op,bool second){
        const unsigned index=second?(op>>14)&31:op&31;
        if(!(op&(second?0x1000:0x800))){
            sljit_emit_fop1(C_,SLJIT_CONV_F64_FROM_F32,target,0,SLJIT_MEM1(SLJIT_S0),reg(index));
        }else if(index<4){
            sljit_emit_fop1(C_,SLJIT_MOV_F64,target,0,SLJIT_MEM1(SLJIT_S0),o_fp+8*index);
        }else sljit_emit_fset64(C_,target,index==0x16?1.0:0.0);
    }
    bool floating_arithmetic(const decoded &d){
        const auto op=d.op;const unsigned sub=(op>>7)&15,dst=(op>>19)&31;
        if(!native_arithmetic||(op&0x2000&&dst>=4))return false;
        const auto operation=sub==0xb?SLJIT_DIV_F64:sub==0xc?SLJIT_MUL_F64:sub==0xd?SLJIT_SUB_F64:sub==0xf?SLJIT_ADD_F64:0;
        if(!operation)return false;
        ic_sub(sub==0xb?35:sub==0xc?18:10);
        // Binary32 add/subtract/multiply with binary32 inputs and destination
        // have the same result as the reference's double intermediate. Keep
        // division and actual double FP-register operands on the wide path.
        const bool narrow=scalar32&&!(op&0x3800)&&sub!=0xb;
        if(narrow){
            sljit_emit_fop1(C_,SLJIT_MOV_F32,SLJIT_FR0,0,SLJIT_MEM1(SLJIT_S0),reg(op&31));
            sljit_emit_fop1(C_,SLJIT_MOV_F32,SLJIT_FR1,0,SLJIT_MEM1(SLJIT_S0),reg((op>>14)&31));
        }else{floating_source(SLJIT_FR0,op,false);floating_source(SLJIT_FR1,op,true);}
        sljit_emit_fop2(C_,operation|(narrow?SLJIT_32:0),SLJIT_FR0,0,SLJIT_FR1,0,SLJIT_FR0,0);
        // Match set_rif's canonical negative NaN, including invalid operations.
        auto *nan=sljit_emit_fcmp(C_,SLJIT_UNORDERED|(narrow?SLJIT_32:0),SLJIT_FR0,0,SLJIT_FR0,0);
        if(op&0x2000)sljit_emit_fop1(C_,SLJIT_MOV_F64,SLJIT_MEM1(SLJIT_S0),o_fp+8*dst,SLJIT_FR0,0);
        else sljit_emit_fop1(C_,narrow?SLJIT_MOV_F32:SLJIT_CONV_F32_FROM_F64,SLJIT_MEM1(SLJIT_S0),reg(dst),SLJIT_FR0,0);
        auto *done=sljit_emit_jump(C_,SLJIT_JUMP);sljit_set_label(nan,sljit_emit_label(C_));
        if(op&0x2000){
            sljit_emit_fset64(C_,SLJIT_FR0,u2d(0xfff8000000000000ull));
            sljit_emit_fop1(C_,SLJIT_MOV_F64,SLJIT_MEM1(SLJIT_S0),o_fp+8*dst,SLJIT_FR0,0);
        }else sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_MEM1(SLJIT_S0),reg(dst),SLJIT_IMM,sljit_sw(0xffc00000u));
        sljit_set_label(done,sljit_emit_label(C_));return true;
    }

	bool emit_inline(const decoded &d)
	{
		const uint32_t op = d.op, o = op >> 24, sub = (op >> 7) & 0xf, dst = (op >> 19) & 0x1f;
		const bool lit_dst = op & 0x2000;
		switch (o) {
		case 0x08:   // b
			ic_sub(1);
			sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), o_ip, SLJIT_IMM, sljit_sw(d.target));
			return true;
		case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
			ic_sub(1);
			branch_if(o & 7, d.target);
			return true;
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {   // testxx
			ic_sub(1);
			sljit_emit_op1(C_, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0), o_ac);
			sljit_emit_op2u(C_, SLJIT_AND32 | SLJIT_SET_Z, SLJIT_R2, 0, SLJIT_IMM, (o & 7) ? (o & 7) : 7);
			sljit_emit_op_flags(C_, SLJIT_MOV32, SLJIT_R0, 0, (o & 7) ? SLJIT_NOT_ZERO : SLJIT_ZERO);
			st_r(dst, SLJIT_R0);
			return true;
		}
		case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
		case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: {   // cmpob / cmpib and branch
			ic_sub(4);
            branch_operands(op);
			compare(o >= 0x39);
			branch_if(o & 7, d.target);
			return true;
		}
		case 0x30: case 0x37: {   // bbc, bbs
			ic_sub(4);
            branch_bit(op);
			// taken (bbc: clear, bbs: set): cc 2 and IP = target; else cc 0
			sljit_jump *notaken = sljit_emit_cmp(C_, SLJIT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, o == 0x30 ? 1 : 0);
			sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM, 2);
			set_cc(SLJIT_R3);
			sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), o_ip, SLJIT_IMM, sljit_sw(d.target));
			sljit_jump *done = sljit_emit_jump(C_, SLJIT_JUMP);
			sljit_set_label(notaken, sljit_emit_label(C_));
			sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM, 0);
			set_cc(SLJIT_R3);
			sljit_set_label(done, sljit_emit_label(C_));
			return true;
		}
		case 0x58: {
			if (lit_dst) return false;
			int cost = 1;
			sljit_s32 alu = 0;
			bool inv1 = false, inv2 = false;
			switch (sub) {
			case 0x1: alu = SLJIT_AND32; break;                      // and
			case 0x2: alu = SLJIT_AND32; inv1 = true; break;         // andnot: t2 & ~t1
			case 0x4: alu = SLJIT_AND32; inv2 = true; break;         // notand: ~t2 & t1
			case 0x6: alu = SLJIT_XOR32; break;                      // xor
			case 0x7: alu = SLJIT_OR32; break;                       // or
			case 0xb: alu = SLJIT_OR32; inv1 = true; break;          // ornot: t2 | ~t1
			case 0xd: alu = SLJIT_OR32; inv2 = true; break;          // notor: ~t2 | t1
			case 0x3: case 0xc: case 0x0: {                          // setbit, clrbit, notbit
				ic_sub(2);
				src1(SLJIT_R0, op); src2(SLJIT_R1, op);
				sljit_emit_op2(C_, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 31);
				sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, 1);
				sljit_emit_op2(C_, SLJIT_SHL32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R0, 0);
				if (sub == 0x3) sljit_emit_op2(C_, SLJIT_OR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
				else if (sub == 0x0) sljit_emit_op2(C_, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0);
				else { sljit_emit_op2(C_, SLJIT_XOR32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, -1); sljit_emit_op2(C_, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R2, 0); }
				st_r(dst, SLJIT_R1);
				return true;
			}
			default: return false;
			}
			ic_sub(cost);
			src1(SLJIT_R0, op); src2(SLJIT_R1, op);
			if (inv1) sljit_emit_op2(C_, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, -1);
			if (inv2) sljit_emit_op2(C_, SLJIT_XOR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, -1);
			sljit_emit_op2(C_, alu, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
			st_r(dst, SLJIT_R1);
			return true;
		}
		case 0x59: {
			if (lit_dst) return false;
			switch (sub) {
			case 0x0: case 0x1: case 0x2: case 0x3:   // addo addi subo subi
				ic_sub(1);
				src1(SLJIT_R0, op); src2(SLJIT_R1, op);
				sljit_emit_op2(C_, sub < 2 ? SLJIT_ADD32 : SLJIT_SUB32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
				st_r(dst, SLJIT_R1);
				return true;
			case 0x8: case 0xc: case 0xe: case 0xb: {   // shro, shlo, shli, shri: t1 >= 32 as MAME has it
				ic_sub(1);
				src1(SLJIT_R0, op); src2(SLJIT_R1, op);
				sljit_jump *big = sljit_emit_cmp(C_, SLJIT_GREATER_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 32);
				sljit_emit_op2(C_, sub == 0x8 ? SLJIT_LSHR32 : sub == 0xb ? SLJIT_ASHR32 : SLJIT_SHL32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_R0, 0);
				sljit_jump *done = sljit_emit_jump(C_, SLJIT_JUMP);
				sljit_set_label(big, sljit_emit_label(C_));
				if (sub == 0xb) sljit_emit_op2(C_, SLJIT_ASHR32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 31);
				else sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 0);
				sljit_set_label(done, sljit_emit_label(C_));
				st_r(dst, SLJIT_R1);
				return true;
			}
			default: return false;
			}
		}
		case 0x5a:
			switch (sub) {
			case 0x0: case 0x1:   // cmpo, cmpi
				ic_sub(1);
				src1(SLJIT_R0, op); src2(SLJIT_R1, op);
				compare(sub == 1);
				return true;
			case 0x4: case 0x5: case 0x6: case 0x7:   // cmpinco, cmpinci, cmpdeco, cmpdeci
				if (lit_dst) return false;
				ic_sub(2);
				src1(SLJIT_R0, op); src2(SLJIT_R1, op);
				compare(sub & 1);
				src2(SLJIT_R1, op);
				sljit_emit_op2(C_, sub < 6 ? SLJIT_ADD32 : SLJIT_SUB32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 1);
				st_r(dst, SLJIT_R1);
				return true;
			default: return false;
			}
		case 0x5c: case 0x5d: case 0x5e: case 0x5f: {   // mov, movl, movt, movq
			if (sub != 0xc) return false;
			const int n = int(o - 0x5c) + 1;
			const uint32_t t2 = o == 0x5c ? dst : o == 0x5d ? (op >> 19) & 0x1e : (op >> 19) & 0x1c;
			if (o == 0x5c && lit_dst) return false;
			const uint32_t s = op & 0x1f;
			if (!(op & 0x800) && n > 1 && ((s != t2 && s < t2 + n && t2 < s + n) || s + n > 32)) return false;   // (overlapping, or past g15: memcpy's)
			ic_sub(n == 1 ? 2 : n == 2 ? 2 : n);
			for (int i = 0; i < n; i++) {
				if (op & 0x800) sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, s);
				else ld_r(SLJIT_R0, int(s) + i);
				st_r(int(t2) + i, SLJIT_R0);
			}
			return true;
		}
        case 0x70: case 0x74:
            if(!native_arithmetic||sub!=1||lit_dst)return false;
            ic_sub(18);src1(SLJIT_R0,op);src2(SLJIT_R1,op);
            sljit_emit_op2(C_,SLJIT_MUL32,SLJIT_R1,0,SLJIT_R1,0,SLJIT_R0,0);
            st_r(dst,SLJIT_R1);return true;
        case 0x78:return floating_arithmetic(d);
		case 0x8c:   // lda
			if (!ea(d)) return false;
			ic_sub(1);
			st_r(dst, SLJIT_R1);
			return true;
		case 0x80: case 0xc0: case 0x88: case 0xc8: case 0x90: {   // ldob ldib ldos ldis ld
			if (!memb_mode_ok(op)) return false;
			const int size = o == 0x90 ? 4 : (o == 0x88 || o == 0xc8) ? 2 : 1;
			std::vector<sljit_jump *> slow;
			if (!ea(d)) return false;
			page(rpage, size, slow);
			ic_sub(4);
			const sljit_s32 mv = size == 4 ? SLJIT_MOV_U32 : o == 0x80 ? SLJIT_MOV_U8 : o == 0xc0 ? SLJIT_MOV_S8 : o == 0x88 ? SLJIT_MOV_U16 : SLJIT_MOV_S16;
			sljit_emit_op1(C_, mv, SLJIT_R0, 0, SLJIT_MEM2(SLJIT_R2, SLJIT_R3), 0);
			st_r(dst, SLJIT_R0);
			sljit_jump *done = sljit_emit_jump(C_, SLJIT_JUMP);
			sljit_label *sl = sljit_emit_label(C_);
			for (sljit_jump *j : slow) sljit_set_label(j, sl);
            if(bus_address){
                // R1 still holds the effective address on every page guard exit.
                sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_R2,0,SLJIT_IMM,dst);
                const bool signed_load=o==0xc0||o==0xc8;
                const op_fn helper=size==4?h_bus_read<4>:
                    size==2?(signed_load?h_bus_read<2,true>:h_bus_read<2>):
                    (signed_load?h_bus_read<1,true>:h_bus_read<1>);
                call_bus(helper,4);
            }else call_op(d);
            stall_check();
			sljit_set_label(done, sljit_emit_label(C_));
			return true;
		}
        case 0x98: case 0x9a: case 0xa0: case 0xa2: case 0xb0: case 0xb2: {
            if(!predecoded_bursts||!bus_address||!memb_mode_ok(op)||!ea(d))return false;
            const int words=int(o>>4)-7;
            const bool store=(o&2)!=0;
            const unsigned first=dst&unsigned(words==2?0x1e:0x1c);
            // Both paths use the ordinary transfer code for page probes,
            // device flags, register updates and a stalled burst's retry.
            constexpr op_fn loads[]={h_bus_multi<2,false>,h_bus_multi<3,false>,h_bus_multi<4,false>};
            constexpr op_fn stores[]={h_bus_multi<2,true>,h_bus_multi<3,true>,h_bus_multi<4,true>};
            sljit_emit_op1(C_,SLJIT_MOV32,SLJIT_R2,0,SLJIT_IMM,first);
            call_bus((store?stores:loads)[words-2],words+(store?1:3));
            stall_check();
            return true;
        }
		case 0x82: case 0xc2: case 0x8a: case 0xca: case 0x92: {   // stob stib stos stis st
			if (!memb_mode_ok(op)) return false;
			const int size = o == 0x92 ? 4 : (o == 0x8a || o == 0xca) ? 2 : 1;
			if (!ea(d)) return false;
			if (bus_address && port_stores && is_hot_port_store(d)) {
				ld_r(SLJIT_R2,dst);
				call_bus(h_bus_write<4>,2);
				return true;
			}
			std::vector<sljit_jump *> slow;
			page(wpage, size, slow);
			ic_sub(2);
			ld_r(SLJIT_R0, dst);
			sljit_emit_op1(C_, size == 4 ? SLJIT_MOV32 : size == 2 ? SLJIT_MOV_U16 : SLJIT_MOV_U8, SLJIT_MEM2(SLJIT_R2, SLJIT_R3), 0, SLJIT_R0, 0);
			sljit_jump *done = sljit_emit_jump(C_, SLJIT_JUMP);
			sljit_label *sl = sljit_emit_label(C_);
			for (sljit_jump *j : slow) sljit_set_label(j, sl);
            if(bus_address){
                ld_r(SLJIT_R2,dst);
                const op_fn helper=size==4?h_bus_write<4>:size==2?h_bus_write<2>:h_bus_write<1>;
                call_bus(helper,2);
            }else call_op(d);
			sljit_set_label(done, sljit_emit_label(C_));
			return true;
		}
		default:
			return false;
		}
	}

	void insn(const decoded &d, uint32_t next_emitted)
	{
		const uint32_t a = d.addr, next = a + uint32_t(d.len), o = d.op >> 24;
		labels[a] = sljit_emit_label(C_);
		if (d.k == kind::bad) {   // the interpreter's (back to the loop at it)
			sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_MEM1(SLJIT_S0), o_ip, SLJIT_IMM, a);
			to_exit.push_back(sljit_emit_jump(C_, SLJIT_JUMP));
			return;
		}
		st.instructions++;
		if (!rt.native_pc.empty()) {
			const sljit_sw counter = reinterpret_cast<sljit_sw>(&rt.native_pc[a >> 2]);
			sljit_emit_op1(C_, SLJIT_MOV_U32, SLJIT_R0, 0, SLJIT_MEM0(), counter);
			sljit_emit_op2(C_, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
			sljit_emit_op1(C_, SLJIT_MOV_U32, SLJIT_MEM0(), counter, SLJIT_R0, 0);
		}
		if (auto h = spun.find(a); h != spun.end()) sljit_emit_op1(C_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), h->second, SLJIT_IMM, 1);
        if(timer_poll&&a==timer_poll_exit)
            sljit_emit_op1(C_,SLJIT_MOV,SLJIT_MEM1(SLJIT_SP),spun.at(timer_poll_head),SLJIT_IMM,0);
        if(direct_branches && native_ok && emit_direct_branch(d,next_emitted)){
            if(!rt.native_inline.empty())rt.native_inline[a>>2]=1;
            ++st.native;
            return;
        }
        // These normal integer instructions cannot observe the architectural
        // PCs unless they fall back to a helper. Commit before that helper,
        // on leaving compiled code, or in a cold cycle-budget exit stub.
        lazy_pc_active=lazy_pc_ok&&native_ok&&d.k==kind::normal&&
            ((o>=0x20&&o<=0x27)||(o>=0x58&&o<=0x5f)||o==0x8c||(native_arithmetic&&(o==0x70||o==0x74||o==0x78)));
        if(!lazy_pc_active)commit_pc(a,next);
		const bool may_stall = o == 0x80 || o == 0x88 || o == 0x90 || o == 0x98 || o == 0x9a || o == 0xa0 || o == 0xa2 || o == 0xb0 || o == 0xb2 || o == 0xc0 || o == 0xc8 ||
							   o == 0x60 || o == 0x66 || o == 0x0a;
		const bool inlined = native_ok && emit_inline(d);
		if (!rt.native_inline.empty()) rt.native_inline[a >> 2] = inlined;
		if (inlined) st.native++;
		else {
			// execute_op's case for it
			call_op(d);
			if (may_stall) stall_check();   // a stalled read: the loop retries it
		}
		// the slice's cycles
        auto *budget_exit=check_budget();
        if(lazy_pc_active)lazy_exits.push_back({budget_exit,a,next});
        else to_exit.push_back(budget_exit);
        auto on = [&]() {
            if(lazy_pc_active&&!compiled(next))commit_pc(a,next);
            if(next_emitted!=next)go(next);
        };
		switch (d.k) {
		case kind::normal:
			if (o == 0x65 || o == 0x60 || o == 0x6e) {   // (may have taken an interrupt)
				load_ip();
				to_dispatch.push_back(sljit_emit_cmp(C_, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, next));
			}
			on();
			break;
		case kind::branch: {
			load_ip();
			auto sp = p.spins.find(a);
            const bool timer=timer_poll&&a==timer_poll_branch;
			if ((timer||sp != p.spins.end()) && compiled(d.target)) {
				sljit_jump *no = sljit_emit_cmp(C_, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, d.target);
				// (only when the last time round all ran since the code was entered: in this slice)
				sljit_jump *fresh = sljit_emit_cmp(C_, SLJIT_EQUAL, SLJIT_MEM1(SLJIT_SP), spun.at(d.target), SLJIT_IMM, 0);
				sljit_emit_op1(C_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
                if(timer){
                    flush_cycles();
                    sljit_emit_icall(C_,SLJIT_CALL,SLJIT_ARGS1V(P),SLJIT_IMM,SLJIT_FUNC_ADDR(h_timer_poll));
                }else{
					sljit_emit_op1(C_, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, sp->second.cost);
					sljit_emit_op1(C_, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, sljit_sw(&sp->second.loads));
                    flush_cycles();
					sljit_emit_icall(C_, SLJIT_CALL, SLJIT_ARGS3V(P, 32, W), SLJIT_IMM, SLJIT_FUNC_ADDR(h_spin));
                }
                reload_cycles();
				sljit_set_label(fresh, sljit_emit_label(C_));
				go(d.target);
				sljit_set_label(no, sljit_emit_label(C_));
			} else
				if_ip(d.target);
			if (d.target != next) on();
			break;
		}
		case kind::jump:
			go(d.target);
			break;
		case kind::call:
			if (d.has_target) go(d.target);
			else to_dispatch.push_back(sljit_emit_jump(C_, SLJIT_JUMP));
			break;
		case kind::ijump:
			if (auto it = p.hinted.find(a); it != p.hinted.end()) {
				load_ip();
				for (uint32_t t : it->second) if (compiled(t)) if_ip(t);
			}
			to_dispatch.push_back(sljit_emit_jump(C_, SLJIT_JUMP));
			break;
		case kind::ret:
			to_dispatch.push_back(sljit_emit_jump(C_, SLJIT_JUMP));
			break;
		default:
			break;
		}
	}

	void *build()
	{
		C_ = sljit_create_compiler(nullptr);
		if (!C_) return nullptr;
		o_ip = off(&R::ip(cpu));
		o_pip = off(&R::pip(cpu));
		o_icount = off(&R::icount(cpu));
		o_stalled = off(&R::stalled(cpu));
		o_r = off(R::regs(cpu));
		o_ac = off(&R::ac(cpu));
        o_fp=off(R::fpregs(cpu));
		rpage = rt.rpage.data();
		wpage = rt.wpage.data();
		for (auto &[br, sp] : p.spins)
			if (compiled(sp.head) && !spun.count(sp.head)) { const sljit_sw at = sljit_sw(spun.size() * sizeof(sljit_sw)); spun[sp.head] = at; }
        timer_poll=rt.collapse_timer_poll&&rt.native_pc.empty()&&matches_timer_poll();
        if(timer_poll&&!spun.count(timer_poll_head))spun[timer_poll_head]=sljit_sw(spun.size()*sizeof(sljit_sw));
		// entry(c, label): the spin loops' flags cleared, then straight to the instruction's code
		sljit_emit_enter(C_, 0, SLJIT_ARGS2V(P, W), 4 | (native_arithmetic?SLJIT_ENTER_FLOAT(2):0), 2, sljit_s32(spun.size() * sizeof(sljit_sw)));
		for (auto &[h, at] : spun) sljit_emit_op1(C_, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), at, SLJIT_IMM, 0);
        if(register_cycles){
            sljit_emit_op1(C_,SLJIT_MOV,SLJIT_R0,0,SLJIT_S1,0);
            reload_cycles();
            sljit_emit_ijump(C_,SLJIT_JUMP,SLJIT_R0,0);
        }else sljit_emit_ijump(C_,SLJIT_JUMP,SLJIT_S1,0);
		// every instruction, in address order (the flow from one to another as jumps)
		std::vector<uint32_t> order;
		for (auto &[a, d] : p.insns) if (p.owner.count(a)) order.push_back(a);
		for (size_t i = 0; i < order.size(); i++)
			insn(p.insns.at(order[i]), i + 1 < order.size() ? order[i + 1] : ~0u);
		// back to the loop
		sljit_label *exit = sljit_emit_label(C_);
        flush_cycles();
        sljit_emit_return_void(C_);
        for(auto &stub:lazy_exits){
            stub.label=sljit_emit_label(C_);sljit_set_label(stub.jump,stub.label);
            commit_pc(stub.pip,stub.ip);
            sljit_set_label(sljit_emit_jump(C_,SLJIT_JUMP),exit);
        }
        // IP is somewhere else: its code, if there's some
		sljit_label *dispatch = sljit_emit_label(C_);
		sljit_emit_op1(C_, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
        flush_cycles();
		sljit_emit_icall(C_, SLJIT_CALL, SLJIT_ARGS1(W, P), SLJIT_IMM, SLJIT_FUNC_ADDR(h_label));
        reload_cycles();
		sljit_jump *none = sljit_emit_cmp(C_, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
		sljit_emit_ijump(C_, SLJIT_JUMP, SLJIT_R0, 0);
		sljit_set_label(none, exit);
		for (sljit_jump *j : to_exit) sljit_set_label(j, exit);
		for (sljit_jump *j : to_dispatch) sljit_set_label(j, dispatch);
		for (auto &[j, t] : to_label) sljit_set_label(j, labels.at(t));
		void *code = sljit_generate_code(C_, 0, nullptr);
		if (!code) { sljit_free_compiler(C_); return nullptr; }
		st.code_bytes = sljit_get_generated_code_size(C_);
		rt.nlabel.assign(runtime::LIMIT / 4, 0);
		for (auto &[a, l] : labels)
			if (a < runtime::LIMIT && p.insns.at(a).k != kind::bad) rt.nlabel[a >> 2] = sljit_get_label_addr(l);
		// Sample PCs can be resolved without inserting counters into generated
		// code. The table belongs to this process/allocation, never another run.
		if(const char *prefix=std::getenv("VF2_CPU_SAMPLES")){
			const std::string path=std::string(prefix)+".jit";
			if(FILE *out=std::fopen(path.c_str(),"w")){
				std::vector<std::pair<sljit_uw,uint32_t>> ordered;
				for(const auto &[guest,label]:labels)ordered.emplace_back(sljit_get_label_addr(label)&~sljit_uw(1),guest);
				std::sort(ordered.begin(),ordered.end());
				for(size_t i=0;i<ordered.size();++i){
					const auto [start,guest]=ordered[i];
					const auto end=i+1<ordered.size()?ordered[i+1].first:sljit_get_label_addr(exit)&~sljit_uw(1);
					const int owner=p.owner_of(guest);
					if(start<end&&owner>=0)std::fprintf(out,"%llx %llx %08x %08x\n",(unsigned long long)start,(unsigned long long)end,guest,p.functions[owner].entry);
				}
                // Cold PC-commit stubs still belong to their guest instruction.
                for(size_t i=0;i<lazy_exits.size();++i){
                    const auto start=sljit_get_label_addr(lazy_exits[i].label)&~sljit_uw(1);
                    const auto end=sljit_get_label_addr(i+1<lazy_exits.size()?lazy_exits[i+1].label:dispatch)&~sljit_uw(1);
                    const auto guest=lazy_exits[i].pip;const int owner=p.owner_of(guest);
                    if(start<end&&owner>=0)std::fprintf(out,"%llx %llx %08x %08x\n",(unsigned long long)start,(unsigned long long)end,guest,p.functions[owner].entry);
                }
				std::fclose(out);
			}else std::perror("JIT profile map");
		}
		sljit_free_compiler(C_);
		return code;
	}
};

} // namespace

bool build_native(runtime &rt, uint32_t (*rd)(void *, uint32_t), void *ctx, uint32_t limit, const hints &map, native_stats *stats)
{
	const auto t0 = std::chrono::steady_clock::now();
	if (!rt.cpu) return false;
	if (std::getenv("VF2_GUEST_COUNTS") && rt.native_pc.empty()) {
		rt.native_pc.resize(runtime::LIMIT / 4);
		rt.native_inline.resize(runtime::LIMIT / 4);
		rt.count_interp = true;
	}
	native_stats st;
	auto prog = std::make_shared<program>(analyse([&](uint32_t a) { return rd(ctx, a); }, limit, map));
	gen g{ nullptr, *prog, rt, *rt.cpu, st };
	void *code = g.build();
	if (!code) return false;
	if (rt.native_code) sljit_free_code(rt.native_code, nullptr);
	rt.native_code = code;
	rt.native_program = prog;
	rt.nentry = reinterpret_cast<runtime::nfn_t>(code);
	rt.rd = rd;
	rt.rd_ctx = ctx;
	rt.map = map;
	rt.limit = limit;
	st.functions = prog->functions.size();
	st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	rt.st.functions = st.functions;
	rt.st.instructions = st.instructions;
	rt.st.dropped = 0;
	if (stats) *stats = st;
	return true;
}

void free_native(void *code)
{
	if (code) sljit_free_code(code, nullptr);
}

} // namespace i960rec
