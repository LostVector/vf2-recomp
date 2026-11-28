// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// Fujitsu MB86233 (TGP), the Model 1 geometry coprocessor. MAME's core
// (src/devices/cpu/mb86233/mb86233.cpp, BSD-3-Clause, copyright-holders: Olivier Galibert),
// without the device framework: memory goes through mb86233_bus.
#include "mb86233.h"
#include <array>
#include <cstdarg>
#include <type_traits>
#include "tgp_sample_context.h"
extern unsigned vf2_tgp_native(mb86233_device &) __attribute__((weak));

// Expose constant instruction fields to ARM optimization in specialized builds.
#if defined(VF2_TGP_AOT) && defined(__GNUC__)
#define VF2_TGP_HELPER __attribute__((always_inline)) inline
#else
#define VF2_TGP_HELPER
#endif

static const bool g_tgp_command_samples=std::getenv("VF2_TGP_COMMAND_SAMPLES")!=nullptr;

static void mb_log(const char *fmt, ...)
{
	if (!getenv("MB86233_LOG")) return;
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

void mb86233_device::reset()
{
    materialize_native_matrix();
	m_sample_command=255;
	m_pc = 0;
	m_ppc = 0;
	m_st = F_ZRC|F_ZRD|F_ZX0|F_ZX1|F_ZX2|F_ZC0|F_ZC1;
	m_sp = 0;

	m_a = 0;
	m_b = 0;
	m_d = 0;
	m_p = 0;
	m_r = 1;
	m_rpc = 1;
	m_c0 = 1;
	m_c1 = 1;
	m_b0 = 0;
	m_b1 = 0;
	m_x0 = 0;
	m_x1 = 0;
	m_i0 = 0;
	m_i1 = 0;
	m_sft = 0;
	m_vsm = 0;
	m_vsmr = 7;
	m_mask = 0;
	m_m = 1;

	m_alu_stmask = 0;
	m_alu_stset = 0;
	m_alu_r1 = 0;
	m_alu_r2 = 0;

	std::fill(std::begin(m_pcs), std::end(m_pcs), 0);

	m_stall = false;
}

VF2_TGP_HELPER u32 mb86233_device::set_exp(u32 val, u32 exp)
{
	return (val & 0x807fffff) | ((exp & 0xff) << 23);
}

VF2_TGP_HELPER u32 mb86233_device::set_mant(u32 val, u32 mant)
{
	return (val & 0x07f800000) | ((mant & 0x00800000) << 8) | (mant & 0x007fffff);
}

VF2_TGP_HELPER u32 mb86233_device::get_exp(u32 val)
{
	return (val >> 23) & 0xff;
}

VF2_TGP_HELPER u32 mb86233_device::get_mant(u32 val)
{
	return val & 0x80000000 ? val | 0x7f800000 : val & 0x807fffff;
}

VF2_TGP_HELPER void mb86233_device::pcs_push()
{
	for(unsigned int i=3; i; i--)
		m_pcs[i] = m_pcs[i-1];
	m_pcs[0] = m_pc;
}

VF2_TGP_HELPER void mb86233_device::pcs_pop()
{
	m_pc = m_pcs[0];
	for(unsigned int i=0; i != 3; i++)
		m_pcs[i] = m_pcs[i+1];
}

VF2_TGP_HELPER void mb86233_device::stset_set_sz_int(u32 val)
{
	m_alu_stset = val ? (val & 0x80000000 ? F_SGD : 0) : F_ZRD;
}

VF2_TGP_HELPER void mb86233_device::stset_set_sz_fp(u32 val)
{
	m_alu_stset = (val & 0x7fffffff) ? (val & 0x80000000 ? F_SGD : 0) : F_ZRD;
}

VF2_TGP_HELPER void mb86233_device::alu_pre(u32 alu)
{
	switch(alu) {
	case 0x00: break; // no alu

	case 0x01: {
		// andd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x02: {
		// orad
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d | m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x03: {
		// eord
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ^ m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x04: {
		// notd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = ~m_d;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x05: {
		// fcpd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		u32 r = f2u(u2f(m_d) - u2f(m_a));
		stset_set_sz_fp(r);
		break;
	}

	case 0x06: {
		// fadd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x07: {
		// fsbd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x08: {
		// fml
		m_alu_stmask = 0;
		m_alu_r1 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = 0;
		break;
	}

	case 0x09: {
		// fmsd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0a: {
		// fmrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0b: {
		// fabd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & 0x7fffffff;
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0c: {
		// fsmd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0d: {
		// fspd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_p;
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0e: {
		// cxfd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(s32(m_d));
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x0f: {
		// cfxd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		switch((m_m >> 1) & 3) {
		case 0: m_alu_r1 = f2s32(roundf(u2f(m_d))); break;   // (as x86-64 on every machine: mame_shim.h)
		case 1: m_alu_r1 = f2s32(ceilf(u2f(m_d))); break;
		case 2: m_alu_r1 = f2s32(floorf(u2f(m_d))); break;
		case 3: m_alu_r1 = f2s32(u2f(m_d)); break;
		}
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x10: {
		// fdvd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) / u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x11: {
		// fned
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ? m_d ^ 0x80000000 : 0;
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x13: {
		// d = b + a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) + u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x14: {
		// d = b - a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) - u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x16: {
		// lsrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d >> (m_sft & 31);   // (vf2-recomp: a count of 32 or more, from a program in a snapshot, as x86 and Arm shift: undefined in C++)
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x17: {
		// lsld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d << (m_sft & 31);
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x18: {
		// asrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = s32(m_d) >> (m_sft & 31);
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x19: {
		// asld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = u32(s32(m_d)) << (m_sft & 31);
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x1a: {
		// addd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d + m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x1b: {
		// subd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d - m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	default:
		mb_log("unhandled alu pre %02x\n", alu);
		break;
	}
}

VF2_TGP_HELPER void mb86233_device::alu_update_st()
{
	m_st = (m_st & ~m_alu_stmask) | m_alu_stset;
}

VF2_TGP_HELPER void mb86233_device::alu_post_1(u32 alu)
{
	// integer alu post ops
	switch(alu) {
	case 0x01: case 0x02: case 0x03: case 0x04:
	case 0x0e: case 0x0f: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		break;

	default:
		break;
	}
}

template<bool Private>
VF2_TGP_HELPER void mb86233_device::alu_post_2(u32 alu)
{
	// floating point alu post ops
	// assume each one takes 2 cycles
	switch (alu) {
	case 0x05:
		// flags only
		alu_update_st();
		if constexpr(!Private)m_icount--;
		break;

	case 0x06: case 0x07: case 0x0b: case 0x0c:
	case 0x10: case 0x11: case 0x13: case 0x14:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		if constexpr(!Private)m_icount--;
		break;

	case 0x08:
		// p update
		m_p = m_alu_r1;
		if constexpr(!Private)m_icount--;
		break;

	case 0x09: case 0x0a: case 0x0d:
		// d, p update
		m_d = m_alu_r1;
		m_p = m_alu_r2;
		alu_update_st();
		if constexpr(!Private)m_icount--;
		break;

	default:
		break;
	}
}

VF2_TGP_HELPER u16 mb86233_device::ea_pre_0(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b0 + m_x0;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b0 + m_x0;
		case 0x20: return m_x0;
		case 0x40: return m_b0 + (m_x0 & m_vsmr);
		case 0x60: return m_x0 & m_vsmr;
		}
	}
	}
	return 0;
}

VF2_TGP_HELPER void mb86233_device::ea_post_0(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x0 += m_i0;
	else
		m_x0 += util::sext(r, 5);
}

VF2_TGP_HELPER u16 mb86233_device::ea_pre_1(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b1 + m_x1;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b1 + m_x1;
		case 0x20: return m_x1;
		case 0x40: return m_b1 + (m_x1 & m_vsmr);
		case 0x60: return m_x1 & m_vsmr;
		}
	}
	}
	return 0;
}

VF2_TGP_HELPER void mb86233_device::ea_post_1(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x1 += m_i1;
	else
		m_x1 += util::sext(r, 5);
}

VF2_TGP_HELPER u32 mb86233_device::read_reg(u32 r)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30)
		return m_bus->rf_r(r & 0x1f);
	switch(r) {
	case 0x00: return m_b0;
	case 0x01: return m_b1;
	case 0x02: return m_x0;
	case 0x03: return m_x1;

	case 0x0c: return m_c0;
	case 0x0d: return m_c1;

	case 0x10: return m_a;
	case 0x11: return get_exp(m_a);
	case 0x12: return get_mant(m_a);
	case 0x13: return m_b;
	case 0x14: return get_exp(m_b);
	case 0x15: return get_mant(m_b);
	case 0x19: return m_d;
		/* c */
	case 0x1a: return get_exp(m_d);
	case 0x1b: return get_mant(m_d);
	case 0x1c: return m_p;
	case 0x1d: return get_exp(m_p);
	case 0x1e: return get_mant(m_p);
	case 0x1f: return m_sft;

	case 0x34: return m_rpc;

	default:
		mb_log("unimplemented read_reg(%02x) (%x)\n", r, m_ppc);
		return 0;
	}
}

VF2_TGP_HELPER void mb86233_device::write_reg(u32 r, u32 v)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30) {
		m_bus->rf_w(r & 0x1f, v);
		return;
	}
	switch(r) {
	case 0x00: m_b0 = v; break;
	case 0x01: m_b1 = v; break;
	case 0x02: m_x0 = v; break;
	case 0x03: m_x1 = v; break;

	case 0x05: m_i0 = v; break;
	case 0x06: m_i1 = v; break;

	case 0x08: m_sp = v; break;

	case 0x0a: m_vsm = v & 7; m_vsmr = (8 << m_vsm) - 1; break;

	case 0x0c:
		m_c0 = v;
		if(m_c0 == 1)
			m_st |= F_ZC0;
		else
			m_st &= ~F_ZC0;
		break;

	case 0x0d:
		m_c1 = v;
		if(m_c1 == 1)
			m_st |= F_ZC1;
		else
			m_st &= ~F_ZC1;
		break;

	case 0x0f: break;

	case 0x10: m_a = v; break;
	case 0x11: m_a = set_exp(m_a, v); break;
	case 0x12: m_a = set_mant(m_a, v); break;
	case 0x13: m_b = v; break;
	case 0x14: m_b = set_exp(m_b, v); break;
	case 0x15: m_b = set_mant(m_b, v); break;
		/* c */
	case 0x19: m_d = v; break;
	case 0x1a: m_d = set_exp(m_d, v); break;
	case 0x1b: m_d = set_mant(m_d, v); break;
	case 0x1c: m_p = v; break;
	case 0x1d: m_p = set_exp(m_p, v); break;
	case 0x1e: m_p = set_mant(m_p, v); break;
	case 0x1f: m_sft = v; break;

	case 0x34: m_rpc = v; break;
	case 0x3c: m_mask = v; break;

	default:
		mb_log("unimplemented write_reg(%02x, %08x) (%x)\n", r, v, m_ppc);
		break;
	}
}

template<bool Private>
VF2_TGP_HELPER void mb86233_device::write_mem_internal_1(u32 r, u32 v, bool bank)
{
	u16 ea = ea_pre_1(r);
	if(bank)
		ea += 0x200;
	data_write<Private>(ea, v);
	ea_post_1(r);
}

VF2_TGP_HELPER void mb86233_device::write_mem_io_1(u32 r, u32 v)
{
	u16 ea = ea_pre_1(r);
	m_bus->io_w(ea, v);
	ea_post_1(r);
}

#if defined(__GNUC__) || defined(__clang__)
#define MB86233_INLINE __attribute__((always_inline)) inline
#else
#define MB86233_INLINE inline
#endif
template<bool Private,class Opcode> MB86233_INLINE void mb86233_device::execute_one(Opcode decoded)
{
		const u32 opcode=decoded;

		switch((opcode >> 26) & 0x3f) {
		case 0x00: {
			// lab
			u32 r1 = opcode & 0x1ff;
			u32 r2 = (opcode >> 9) & 0x1ff;
			u32 alu = (opcode >> 21) & 0x1f;
			u32 op = (opcode >> 18) & 0x7;

			alu_pre(alu);

			switch(op) {
			case 0: case 1: {
				// lab mem, mem (e)

				u32 ea1 = ea_pre_0(r1);
				u32 v1 = data_read<Private>(ea1);
				if(!Private&&m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2);
				u32 v2 = m_bus->io_r(ea2);
				if(!Private&&m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			case 3: {
				// lab mem, mem + 0x200

				u32 ea1 = ea_pre_0(r1);
				u32 v1 = data_read<Private>(ea1);
				if(!Private&&m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2) + 0x200;
				u32 v2 = data_read<Private>(ea2);
				if(!Private&&m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			case 4: {
				// lab mem + 0x200, mem

				u32 ea1 = ea_pre_0(r1) + 0x200;
				u32 v1 = data_read<Private>(ea1);
				if(!Private&&m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2);
				u32 v2 = data_read<Private>(ea2);
				if(!Private&&m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			default:
				mb_log("unhandled lab subop %x\n", op);
				mb_log("%x\n", m_ppc);
				break;

			}

			alu_post_1(alu);
			alu_post_2<Private>(alu);
			break;
		}


		case 0x07: {
			// ld / mov
			u32 r1 = opcode & 0x1ff;
			u32 r2 = (opcode >> 9) & 0x1ff;
			u32 alu = (opcode >> 21) & 0x1f;
			u32 op = (opcode >> 18) & 0x7;

			alu_pre(alu);

			switch(op) {
			case 0: {
				// mov mem, mem (e)
				u32 ea = ea_pre_0(r1);
				u32 v = data_read<Private>(ea);
				if(!Private&&m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_io_1(r2, v);
				break;
			}

			case 1: {
				// mov mem, mem (e)
				u32 ea = ea_pre_0(r1);
				u32 v = data_read<Private>(ea);
				if(!Private&&m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_io_1(r2, v);
				break;
			}

			case 2: {
				// mov mem (e), mem
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->io_r(ea);
				if(!Private&&m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1<Private>(r2, v, false);
				break;
			}

			case 3: {
				// mov mem, mem + 0x200
				u32 ea = ea_pre_0(r1);
				u32 v = data_read<Private>(ea);
				if(!Private&&m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1<Private>(r2, v, true);
				break;
			}

			case 4: {
				// mov mem + 0x200, mem
				u32 ea = ea_pre_0(r1) + 0x200;
				u32 v = data_read<Private>(ea);
				if(!Private&&m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1<Private>(r2, v, false);
				break;
			}

			case 5: {
				// mov mem (o), mem
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->prog_r(ea);
				if(!Private&&m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1<Private>(r2, v, false);
				break;
			}

			case 7: {
				switch(r2 >> 6) {
				case 0: {
					// mov reg, mem
					u32 v = read_reg(r2);
					if(!Private&&m_stall) goto do_stall;
					alu_post_1(alu);
					write_mem_internal_1<Private>(r1, v, false);
					break;
				}

				case 1: {
					// mov reg, mem (e)
					u32 v = read_reg(r2);
					if(!Private&&m_stall) goto do_stall;
					alu_post_1(alu);
					write_mem_io_1(r1, v);
					break;
				}

				case 2: {
					// mov mem + 0x200, reg
					u32 ea = ea_pre_1(r1) + 0x200;
					u32 v = data_read<Private>(ea);
					if(!Private&&m_stall) goto do_stall;
					ea_post_1(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 3: {
					// mov mem, reg
					u32 ea = ea_pre_1(r1);
					u32 v = data_read<Private>(ea);
					if(!Private&&m_stall) goto do_stall;
					ea_post_1(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 4: {
					// mov mem (e), reg
					u32 ea = ea_pre_1(r1);
					u32 v = m_bus->io_r(ea);
					if(!Private&&m_stall) goto do_stall;
					ea_post_1(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 5: {
					// mov mem (o), reg
					u32 ea = ea_pre_0(r1);
					u32 v = m_bus->prog_r(ea);
					if(!Private&&m_stall) goto do_stall;
					ea_post_0(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 6: {
					// mov reg, reg
					u32 v = read_reg(r1);
					if(!Private&&m_stall) goto do_stall;
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				default:
					alu_post_1(alu);
					mb_log("unhandled ld/mov subop 7/%x (%x)\n", r2 >> 6, m_ppc);
					break;
				}
				break;
			}

			default:
				alu_post_1(alu);
				mb_log("unhandled ld/mov subop %x (%x)\n", op, m_ppc);
				break;
			}

			// For floating point ops, registers are updated after transfer
			alu_post_2<Private>(alu);
			break;
		}

		case 0x0d: {
			// stm/clm
			u32 sub2 = (opcode >> 17) & 7;

			// Theorically has restricted alu too

			switch(sub2) {
			case 5:
				// stmh
				// bit 0 = floating point
				// bit 1-2 = rounding mode
				m_m = u16(opcode);
				break;

			default:
				mb_log("unimplemented opcode 0d/%x (%x)\n", sub2, m_ppc);
				break;
			}
			break;
		}

		case 0x0e: {
			// lipl / lia / lib / lid
			switch((opcode >> 24) & 0x3) {
			case 0:
				m_p = (m_p & 0xffffff000000) | (opcode & 0xffffff);
				break;
			case 1:
				m_a = util::sext(opcode, 24);
				break;
			case 2:
				m_b = util::sext(opcode, 24);
				break;
			case 3:
				m_d = util::sext(opcode, 24);
				break;
			}
			break;
		}

		case 0x0f: {
			// rep/clr0/clr1/set
			u32 alu = (opcode >> 20) & 0x1f;
			u32 sub2 = (opcode >> 17) & 7;

			alu_pre(alu);

			switch(sub2) {
			case 0:
				// clr0
				if(opcode & 0x0004) m_a = 0;
				if(opcode & 0x0008) m_b = 0;
				if(opcode & 0x0010) m_d = 0;
				break;

			case 1:
				// clr1 - flags mapping unknown
				break;

			case 2: {
				// rep
				u8 r = u8(opcode & 0x8000 ? read_reg(opcode) : opcode);
				if(!Private&&m_stall) goto do_stall;
				m_r = r;
				goto rep_start;
			}

			case 3:
				// set - flags mapping unknown
				// 0800 = enable interrupt flag
				break;

			default:
				mb_log("unimplemented opcode 0f/%x (%x)\n", sub2, m_ppc);
				break;
			}

			alu_post_1(alu);
			break;
		}

		case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: {
			// ldi
			write_reg(opcode >> 24, util::sext(opcode, 24));
			break;
		}

		case 0x2f: case 0x3f: {
			// Conditional branch of every kind
			u32 cond = ( opcode >> 20 ) & 0x1f;
			u32 subtype = ( opcode >> 17 ) & 7;
			u32 data = opcode & 0xffff;
			bool invert = opcode & 0x40000000;

			bool cond_passed = false;

			switch(cond) {
			case 0x00: // zrd - d zero
				cond_passed = m_st & F_ZRD;
				break;

			case 0x01: // ged - d >= 0
				cond_passed = !(m_st & F_SGD);
				break;

			case 0x02: // led - d <= 0
				cond_passed = m_st & (F_ZRD | F_SGD);
				break;

			case 0x0a: // gpio0
				cond_passed = m_gpio0;
				break;

			case 0x0b: // gpio1
				cond_passed = m_gpio1;
				break;

			case 0x0c: // gpio2
				cond_passed = m_gpio2;
				break;

			case 0x10: // zc0 - c0 == 1
				cond_passed = !(m_st & F_ZC0);
				break;

			case 0x11: // zc1 - c1 == 1
				cond_passed = !(m_st & F_ZC1);
				break;

			case 0x12: // gpio3
				cond_passed = m_gpio3;
				break;

			case 0x16: // alw - always
				cond_passed = true;
				break;

			default:
				mb_log("unimplemented condition %x (%x)\n", cond, m_ppc);
				break;
			}
			if(invert)
				cond_passed = !cond_passed;

			if(cond_passed) {
				switch(subtype) {
				case 0: // brif #adr
					m_pc = data;
					break;

				case 1: // brul
					if(opcode & 0x4000) {
						// brul reg
						u32 v = read_reg(opcode);
						if(!Private&&m_stall) goto do_stall;
						m_pc = v;
					} else {
						// brul adr
						u32 ea = ea_pre_0(opcode);
						u32 v = data_read<Private>(ea);
						if(!Private&&m_stall) goto do_stall;
						ea_post_0(opcode);
						m_pc = v;
					}
					break;

				case 2: // bsif #adr
					pcs_push();
					m_pc = data;
					break;

				case 3: // bsul
					if(opcode & 0x4000) {
						// bsul reg
						u32 v = read_reg(opcode);
						if(!Private&&m_stall) goto do_stall;
						pcs_push();
						m_pc = v;
					} else {
						// bsul adr
						u32 ea = ea_pre_0(opcode);
						u32 v = data_read<Private>(ea);
						if(!Private&&m_stall) goto do_stall;
						ea_post_0(opcode);
						pcs_push();
						m_pc = v;
					}
					break;

				case 5: // rtif #adr
					pcs_pop();
					break;

				case 6: { // ldif adr, rn
					u32 ea = ea_pre_0(opcode);
					u32 v = data_read<Private>(ea);
					if(!Private&&m_stall) goto do_stall;
					ea_post_0(opcode);
					write_reg(opcode >> 9, v);
					break;
				}

				default:
					mb_log("unimplemented branch subtype %x (%x)\n", subtype, m_ppc);
					break;
				}
			}

			if(subtype < 2)
				switch(cond) {
				case 0x10:
					if(m_c0 != 1) {
						m_c0 --;
						if(m_c0 == 1)
							m_st |= F_ZC0;
					}
					break;

				case 0x11:
					if(m_c1 != 1) {
						m_c1 --;
						if(m_c1 == 1)
							m_st |= F_ZC1;
					}
				break;
				}

			break;
		}

		default:
			mb_log("unimplemented opcode type %02x (%x)\n", (opcode >> 26) & 0x3f, m_ppc);
			break;
		}

		if(!Private&&m_r != 1) {
			m_pc = m_ppc;
			m_r --;
		}

	rep_start:
		if(0) {
		do_stall:
			m_pc = m_ppc;
			m_stall = false;
		}
		if constexpr(!Private)m_icount--;
}

template<u32 Opcode> void mb86233_device::compiled_instruction(mb86233_device *cpu)
{
	cpu->execute_one(std::integral_constant<u32,Opcode>{});
}

#if defined(VF2_TGP_AOT)
namespace {
constexpr bool private_register(u32 reg)
{
    reg &= 0x3f;
    return reg < 0x20 || reg >= 0x30;
}

constexpr bool private_instruction(u32 opcode)
{
    // These forms can only touch the datapath and the direct internal RAM.
    const u32 family=opcode>>26, move=(opcode>>18)&7;
    if(family==0 || family==7){
        if(move==3 || move==4)return true;
        if(family==7 && move==7){
            const u32 from=opcode&0x1ff, to=(opcode>>9)&0x1ff;
            return private_register(to) &&
                ((to>>6)==0 || (to>>6)==2 || (to>>6)==3 ||
                 ((to>>6)==6 && private_register(from)));
        }
        return false;
    }
    return family==0x0e || (family>=0x10 && family<=0x1f);
}

constexpr int private_cycles(u32 opcode)
{
    if(opcode>>26!=0 && opcode>>26!=7)return 1;
    const u32 alu=(opcode>>21)&31;
    return 1+((alu>=5 && alu<=13) || alu==16 || alu==17 || alu==19 || alu==20);
}
}

template<u32 Opcode>
MB86233_INLINE void mb86233_device::private_step()
{
    constexpr unsigned family=Opcode>>26;
    constexpr unsigned alu=(family==0||family==7)?((Opcode>>21)&31):0;
    auto both_nonfinite=[](u32 a,u32 b){return (a&b&0x7f800000u)==0x7f800000u;};
    bool canonical=false;
    if constexpr(alu==5||alu==6||alu==7||alu==16)canonical|=both_nonfinite(m_d,m_a);
    if constexpr(alu==8||alu==9||alu==10||alu==13)canonical|=both_nonfinite(m_a,m_b);
    if constexpr(alu==9||alu==10||alu==12)canonical|=both_nonfinite(m_d,m_p);
    if constexpr(alu==19||alu==20)canonical|=both_nonfinite(m_b,m_a);
    if(canonical){
        // A fused function can commute FP operands. Use the ordinary opcode
        // handler to preserve its NaN payload; the region charges its cycles.
        const int budget=m_icount;
        void (*volatile handler)(mb86233_device*)=&compiled_instruction<Opcode>;
        handler(this);
        m_icount=budget;
    }else execute_one<true>(std::integral_constant<u32,Opcode>{});
}

template<bool ExactFloating,u16 First,u32... Opcodes>
MB86233_INLINE void mb86233_device::private_region_entry_impl(mb86233_device *cpu)
{
    static_assert((private_instruction(Opcodes)&&...));
    constexpr int cycles=(private_cycles(Opcodes)+...);
    constexpr u32 first_opcode[]={Opcodes...};
    if(!cpu->m_private_regions_allowed || cpu->m_ppc!=First || cpu->m_r!=1 ||
       cpu->m_stall || cpu->m_icount<cycles){
        cpu->execute_one(std::integral_constant<u32,first_opcode[0]>{});
        return;
    }

    // This straight-line region has no callbacks. Publish PC and cycle
    // accounting once before returning to the next instruction boundary.
    if constexpr(ExactFloating)
        (cpu->private_step<Opcodes>(),...);
    else
        (cpu->execute_one<true>(std::integral_constant<u32,Opcodes>{}),...);
    cpu->m_pc=First+sizeof...(Opcodes);
    cpu->m_ppc=cpu->m_pc-1;
    cpu->m_icount-=cycles;
    cpu->m_compiled_instructions+=sizeof...(Opcodes)-1;
    cpu->m_block_instructions+=sizeof...(Opcodes);
    ++cpu->m_blocks_run;
}

template<u16 First,u32... Opcodes>
void mb86233_device::private_region_entry(mb86233_device *cpu)
{
    private_region_entry_impl<false,First,Opcodes...>(cpu);
}

template<u16 First,u32... Opcodes>
void mb86233_device::private_4a_resume_entry(mb86233_device *cpu)
{
    constexpr u32 first_opcode[]={Opcodes...};
    static const bool enabled=std::getenv("VF2_TGP_4A_RESUME")!=nullptr;
    if(enabled)private_region_entry<First,Opcodes...>(cpu);
    else compiled_instruction<first_opcode[0]>(cpu);
}

static const bool g_command32_regions=std::getenv("VF2_TGP_COMMAND32_REGIONS")!=nullptr;

template<u16 First,u32... Opcodes>
void mb86233_device::command32_region_entry(mb86233_device *cpu)
{
    constexpr u32 first_opcode[]={Opcodes...};
    if(g_command32_regions)private_region_entry_impl<true,First,Opcodes...>(cpu);
    else compiled_instruction<first_opcode[0]>(cpu);
}

static const bool g_native_command_dispatch=std::getenv("VF2_TGP_NATIVE_DISPATCH")!=nullptr;
static const bool g_fused_command_intake=std::getenv("VF2_TGP_FUSED_COMMAND_INTAKE")!=nullptr;
static const bool g_native_command37=std::getenv("VF2_TGP_NATIVE_COMMAND37")!=nullptr;

namespace {
constexpr int command37_setup_cycles=19;
constexpr int command37_finish_cycles=16;
constexpr int command37_fused_cycles=command37_setup_cycles+95+1+command37_finish_cycles;
}

void mb86233_device::command37_setup_entry(mb86233_device *cpu)
{
    const unsigned source=u16(cpu->m_d+cpu->m_a);
    if (!g_native_command37 || !cpu->m_private_regions_allowed || cpu->m_ppc!=0x0290 ||
        cpu->m_r!=1 || cpu->m_stall || cpu->m_icount<command37_setup_cycles || source>500) {
        compiled_instruction<0x1f5f1e10u>(cpu);
        return;
    }

    // The two preceding FIFO reads and conditional branch already ran. Copy
    // in guest order in case the bus supplies overlapping internal windows.
    const bool full=cpu->m_icount>=command37_fused_cycles;
    const auto ram=cpu->m_fast_ram;
    cpu->execute_one<true>(std::integral_constant<u32,0x1f5f1e10u>{});
    for (unsigned i=0;i<12;++i)
        ram.low[0x20+i]=ram.high[source+i];
    cpu->m_x0=u16(ram.low[0x3f]);
    cpu->m_x1=0x20;
    cpu->m_pc=0x0297;
    cpu->pcs_push();
    cpu->m_ppc=0x0bc2;
    cpu->m_pc=0x0bc3;
    cpu->m_icount-=command37_setup_cycles;
    cpu->m_compiled_instructions+=command37_setup_cycles-1;

    // Native matrix consumes 95 cycles and accounts for all instructions
    // after its entry. A rejected matrix continues at its normal entry.
    if (full && cpu->m_native_matrix_allowed) {
        cpu->m_ppc=0x0bc3;
        if (cpu->native_matrix()) {
            cpu->m_ppc=0x0bfa;
            cpu->pcs_pop();
            --cpu->m_icount;
            cpu->m_compiled_instructions+=2; // Matrix entry and the return.
            cpu->command37_finish();
            return;
        }
        cpu->m_ppc=0x0bc2;
    }
}

bool mb86233_device::command37_finish()
{
    const unsigned destination=u16(m_fast_ram.low[0x3f]);
    if (destination>500) return false;
    for (unsigned i=0;i<12;++i)
        m_fast_ram.high[destination+i]=m_fast_ram.low[0x2c+i];
    m_x0=0x38;
    m_x1=u16(destination+12);
    m_ppc=0x029b;
    m_pc=0x0035;
    m_icount-=command37_finish_cycles;
    m_compiled_instructions+=command37_finish_cycles;
    return true;
}

void mb86233_device::command37_finish_entry(mb86233_device *cpu)
{
    if (!g_native_command37 || !cpu->m_private_regions_allowed || cpu->m_ppc!=0x0297 ||
        cpu->m_r!=1 || cpu->m_stall || cpu->m_icount<command37_finish_cycles || !cpu->command37_finish()) {
        compiled_instruction<0x4200002cu>(cpu);
        return;
    }
    --cpu->m_compiled_instructions; // The dispatch already counted the entry.
}

template<u16 First>
MB86233_INLINE unsigned mb86233_device::finish_command_dispatch(int prior)
{
    static_assert(First>=0x3b&&First<=0x3e);
    if constexpr(First<=0x3b)execute_one<true>(std::integral_constant<u32,0x1c1f320cu>{});
    if constexpr(First<=0x3c)execute_one<true>(std::integral_constant<u32,0x3900003fu>{});
    if constexpr(First<=0x3d)execute_one<true>(std::integral_constant<u32,0x1f5f1e10u>{});
    m_ppc=0x3e;
    m_pc=u16(m_d);
    constexpr unsigned count=0x3f-First;
    // The validated table branches can consume one additional cycle.
    if(m_icount>prior+int(count)&&!g_tgp_command_samples&&m_pc>=0x3f&&m_pc<0xaf){
        m_ppc=m_pc;
        m_pc=u16(compiled_program()[m_pc].opcode);
        return count+1;
    }
    return count;
}

template<u16 First,u32 Opcode>
void mb86233_device::command_suffix_entry(mb86233_device *cpu)
{
    static const bool resume=std::getenv("VF2_TGP_RESUME_DISPATCH")!=nullptr;
    constexpr int count=0x3f-First;
    if(!resume||!g_native_command_dispatch||!cpu->m_private_regions_allowed||cpu->m_ppc!=First||
       cpu->m_r!=1||cpu->m_stall||cpu->m_icount<count||(count==1&&cpu->m_icount==1)){
        compiled_instruction<Opcode>(cpu);
        return;
    }
    const unsigned done=cpu->finish_command_dispatch<First>(0);
    cpu->m_icount-=int(done);
    cpu->m_compiled_instructions+=done-1;
    cpu->m_block_instructions+=done;
    ++cpu->m_blocks_run;
}

void mb86233_device::command_intake_entry(mb86233_device *cpu)
{
    const bool eligible=g_fused_command_intake&&g_native_command_dispatch&&cpu->m_private_regions_allowed&&
                        cpu->m_ppc==0x35&&cpu->m_r==1&&!cpu->m_stall;
    // Keep the RF read, including empty-FIFO stalls and timing callbacks.
    command_entry<255,0x1c1f2621u>(cpu);
    if(!eligible||cpu->m_pc!=0x36||cpu->m_icount<5)return;

    cpu->m_ppc=cpu->m_pc++;
    ++cpu->m_compiled_instructions;
    command_dispatch_entry(cpu);
}

void mb86233_device::command_dispatch_entry(mb86233_device *cpu)
{
    if(!g_native_command_dispatch||!cpu->m_private_regions_allowed||cpu->m_ppc!=0x36||
       cpu->m_r!=1||cpu->m_stall||cpu->m_icount<5){
        compiled_instruction<0x1c1f1813u>(cpu);
        return;
    }

    // The command word has already been read from RF1 with normal FIFO timing.
    // Run only register operations here and retain the exceptional subroutine.
    cpu->execute_one<true>(std::integral_constant<u32,0x1c1f1813u>{});
    cpu->execute_one<true>(std::integral_constant<u32,0x1c1f200cu>{});
    cpu->execute_one<true>(std::integral_constant<u32,0x1c1f3214u>{});
    cpu->execute_one<true>(std::integral_constant<u32,0x1f7f1e10u>{});
    cpu->m_ppc=0x3a;
    cpu->m_pc=0x3b;
    unsigned count=5;
    if(!(cpu->m_st&F_ZRD)){
        cpu->pcs_push();
        cpu->m_pc=0x1af;
    }else if(cpu->m_icount>=9){
        count+=cpu->finish_command_dispatch<0x3b>(5);
    }
    cpu->m_icount-=int(count);
    cpu->m_compiled_instructions+=count-1;
    cpu->m_block_instructions+=count;
    ++cpu->m_blocks_run;
}
#endif

template<u8 Command,u32 Opcode> void mb86233_device::command_entry(mb86233_device *cpu)
{
    if(g_tgp_command_samples){cpu->m_sample_command=Command;vf2_tgp_sample::command(Command);}
    cpu->execute_one(std::integral_constant<u32,Opcode>{});
}

bool mb86233_device::native_matrix()
{
    // Two recognized 3x4 column-major composition routines. The second keeps
    // the left operand in low RAM and adds its translation directly. Enter
    // only when the entire side-effect-free region fits the remaining slice.
    // Memory, registers, flags and the original instruction/cycle totals are
    // committed together; all other inputs retain individual DSP execution.
    static const bool extended=std::getenv("VF2_TGP_EXTENDED_MATRIX")!=nullptr;
    if(m_ppc!=0xbc3&&m_ppc!=0xbfb&&m_ppc!=0xbfc)return false;
    if(m_ppc==0xbfc&&!extended)return false;
    const bool reverse=m_ppc!=0xbc3,load_left=m_ppc==0xbfb;
    const int cycles=reverse?(load_left?100:99):95;
    const unsigned left=load_left?0x20:m_x0;
    if(m_icount<cycles||m_r!=1||m_stall||!m_fast_ram.low||!m_fast_ram.high)return false;
    if(reverse?(left>244||m_x1>500):(m_x0>500||m_x1>244))return false;
    if(m_fast_ram.low[0]!=0)return false; // the routine loads both base registers from $0
    const u32 *h=reverse?m_fast_ram.low+left:m_fast_ram.high+m_x0;
    const u32 *l=reverse?m_fast_ram.high+m_x1:m_fast_ram.low+m_x1;
    u32 *out=m_fast_ram.low+0x2c;
    if(uintptr_t(m_fast_ram.low)<uintptr_t(m_fast_ram.high+512)&&uintptr_t(m_fast_ram.high)<uintptr_t(m_fast_ram.low+256))return false;
    auto overlaps=[&](const u32 *p){return uintptr_t(p)<uintptr_t(out+12)&&uintptr_t(out)<uintptr_t(p+12);};
    if(overlaps(h)||overlaps(l))return false;
    const u32 unit=reverse?m_fast_ram.high[2]:m_fast_ram.low[2];
    if((unit&0x7f800000u)==0x7f800000u)return false;
    for(unsigned i=0;i<12;++i)if((h[i]&0x7f800000u)==0x7f800000u||(l[i]&0x7f800000u)==0x7f800000u)return false;
    u32 result[12];
    for(unsigned column=0;column<4;++column)for(unsigned row=0;row<3;++row){
        const float p0=u2f(h[row])*u2f(l[column*3]);
        const float p1=u2f(h[row+3])*u2f(l[column*3+1]);
        const float p2=u2f(h[row+6])*u2f(l[column*3+2]);
        float sum=(p0+p1)+p2;
        if(column==3)sum=reverse?sum+u2f(h[row+9]):sum+u2f(h[row+9])*u2f(unit);
        result[column*3+row]=f2u(sum);
        // Preserve the original handlers' NaN payload choices through fallback.
        if((result[column*3+row]&0x7f800000u)==0x7f800000u)return false;
    }
    const u32 final_p=f2u(u2f(h[11])*u2f(unit));
    const u32 final_a=h[11];
    std::copy(result,result+12,out);
    m_b0=m_b1=0;m_a=final_a;m_b=unit;m_d=result[11];m_p=final_p;
    if(load_left)m_x0=0x20;
    m_alu_stmask=F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
    m_alu_r1=m_d;m_alu_r2=m_p;stset_set_sz_fp(m_d);alu_update_st();
    m_ppc=reverse?0xc34:0xbf9;m_pc=reverse?0xc35:0xbfa;m_icount-=cycles;
    m_compiled_instructions+=reverse?(load_left?57:56):54; // the entry dispatch already counted one
    ++m_native_matrices;return true;
}

const mb86233_device::matrix_timing *mb86233_device::native_matrix_timing(u16 entry)
{
#if defined(VF2_TGP_AOT)
#include "vf2_tgp_matrix_timing.inc"
#else
    return nullptr;
#endif
}

void mb86233_device::save_native_matrix_state(u16 output_address,unsigned output_words,
                                                u16 other_address,unsigned other_words)
{
    auto &pending=m_pending_matrix;
    auto &saved=pending.initial;
    saved.st=m_st;saved.a=m_a;saved.b=m_b;saved.d=m_d;saved.p=m_p;
    saved.mask=m_alu_stmask;saved.set=m_alu_stset;saved.r1=m_alu_r1;saved.r2=m_alu_r2;
    saved.b0=m_b0;saved.b1=m_b1;saved.x0=m_x0;saved.x1=m_x1;
    pending.output_address=output_address;pending.output_words=output_words;
    pending.other_address=other_address;pending.other_words=other_words;
    std::copy_n(native_ram_at(output_address),output_words,saved.output);
    if(other_words)std::copy_n(native_ram_at(other_address),other_words,saved.other);
}

void mb86233_device::activate_native_matrix_pending(const matrix_timing *timing,uint64_t instructions)
{
    // The guest observes only completed instructions. Reconstruct the
    // prefix on demand since the cached remainder has no external bus I/O.
    auto &pending=m_pending_matrix;
    m_compiled_instructions=instructions;
    pending.active=true;pending.issued=0;pending.timing=timing;
    pending.revision=m_checked_revision;pending.program=m_checked_program;
    advance_native_matrix_pending();
    --m_compiled_instructions; // execute_run already counted the entry dispatch
}

bool mb86233_device::start_native_matrix_pending()
{
    const matrix_timing *timing=native_matrix_timing(m_ppc);
    if(!timing||m_icount<=0||m_icount>=timing->prefix[timing->instructions]||m_r!=1||m_stall||!m_fast_ram.low||!m_fast_ram.high)return false;
    save_native_matrix_state(0x2c,12);
    const int budget=m_icount;const uint64_t instructions=m_compiled_instructions;
    m_icount=timing->prefix[timing->instructions];
    const bool computed=native_matrix();m_icount=budget;
    if(!computed)return false;
    ++m_resumed_matrices;
    activate_native_matrix_pending(timing,instructions);
    return true;
}

void mb86233_device::advance_native_matrix_pending()
{
    auto &pending=m_pending_matrix;const auto &t=*pending.timing;
    if(m_icount<=0)return;
    const unsigned old=pending.issued;
    const unsigned target=t.prefix[old]+unsigned(m_icount);
    unsigned next=t.instructions;
    if(target<t.prefix[t.instructions])
        next=unsigned(std::lower_bound(t.prefix+old+1,t.prefix+t.instructions+1,target)-t.prefix);
    m_icount-=int(t.prefix[next]-t.prefix[old]);
    m_compiled_instructions+=next-old;pending.issued=u8(next);
    if(next==t.instructions)pending.active=false;
}

void mb86233_device::materialize_native_matrix()
{
    if(!m_pending_matrix.active)return;
    const auto &pending=m_pending_matrix;
    const auto &saved=pending.initial;const auto &t=*pending.timing;
    const unsigned issued=m_pending_matrix.issued;
    const int budget=m_cycles_running,left=m_icount;
    const bool allowed=m_native_matrix_allowed,resume=m_native_resume_allowed;
    m_pending_matrix.active=false;m_native_matrix_allowed=false;m_native_resume_allowed=false;
    std::copy_n(saved.output,pending.output_words,native_ram_at(pending.output_address));
    if(pending.other_words)
        std::copy_n(saved.other,pending.other_words,native_ram_at(pending.other_address));
    m_st=saved.st;m_a=saved.a;m_b=saved.b;m_d=saved.d;m_p=saved.p;
    m_alu_stmask=saved.mask;m_alu_stset=saved.set;m_alu_r1=saved.r1;m_alu_r2=saved.r2;
    m_b0=saved.b0;m_b1=saved.b1;m_x0=saved.x0;m_x1=saved.x1;
    m_pc=t.entry;m_icount=m_cycles_running=t.prefix[issued];
    const compiled_entry *program=compiled_program();
    // Use the originally recognized constants even if program RAM changed
    // after this prefix ran. GPIO inputs retain their current external state.
    for(unsigned i=0;i<issued;++i){m_ppc=m_pc++;program[t.entry+i].run(this);}
    m_icount=left;m_cycles_running=budget;
    m_native_matrix_allowed=allowed;m_native_resume_allowed=resume;
}

void mb86233_device::matrix_entry(mb86233_device *cpu)
{
    if(cpu->m_native_matrix_allowed){
        if(cpu->m_native_resume_allowed&&cpu->start_native_matrix_pending())return;
        if(cpu->native_matrix())return;
    }
    if(cpu->m_ppc==0xbfb)cpu->execute_one(std::integral_constant<u32,0x42000020u>{});
    else cpu->execute_one(std::integral_constant<u32,0x1c1d8000u>{});
}

template<u32 Opcode> MB86233_INLINE bool mb86233_device::block_step(u16 expected)
{
    if(m_icount<=0||m_pc!=expected)return false;
    m_ppc=m_pc++;++m_compiled_instructions;
    execute_one(std::integral_constant<u32,Opcode>{});return true;
}
template<u32 A,u32 B,u32 C,u32 D,u32 E,u32 F,u32 G,u32 H>
unsigned mb86233_device::compiled_block(mb86233_device *cpu,unsigned offset)
{
    const u16 base=u16(cpu->m_pc-offset);unsigned done=0;
#define TGP_BLOCK_CASE(N,OP) case N: if(!cpu->block_step<OP>(u16(base+N)))return done; ++done; [[fallthrough]];
    switch(offset){
        TGP_BLOCK_CASE(0,A)
        TGP_BLOCK_CASE(1,B)
        TGP_BLOCK_CASE(2,C)
        TGP_BLOCK_CASE(3,D)
        TGP_BLOCK_CASE(4,E)
        TGP_BLOCK_CASE(5,F)
        TGP_BLOCK_CASE(6,G)
        TGP_BLOCK_CASE(7,H)
        default:break;
    }
#undef TGP_BLOCK_CASE
    return done;
}
const mb86233_device::compiled_block_entry *mb86233_device::compiled_blocks(){
#if defined(VF2_TGP_AOT)
#include "vf2_tgp_blocks.inc"
#else
    return nullptr;
#endif
}

const mb86233_device::compiled_entry *mb86233_device::compiled_program()
{
#if defined(VF2_TGP_AOT)
#include "vf2_tgp.inc"
#else
	return nullptr;
#endif
}

bool mb86233_device::precompiled()const
{
	return compiled_program()!=nullptr&&std::getenv("VF2_TGP_INTERP")==nullptr;
}

void mb86233_device::execute_run()
{
    execute_run(nullptr,nullptr);
}

void mb86233_device::execute_run(const u32 *mapped, const uint64_t *mapped_revision)
{
    if(g_tgp_command_samples){
        vf2_tgp_sample::command(m_sample_command);
        execute_run_impl(mapped,mapped_revision);
        vf2_tgp_sample::leave();
    }else execute_run_impl(mapped,mapped_revision);
}

namespace {
struct tgp_slice_options {
    bool sample_pc=std::getenv("VF2_TGP_PC_SAMPLES")!=nullptr;
#if defined(VF2_TGP_AOT)
    bool interpreter=std::getenv("VF2_TGP_INTERP")!=nullptr;
    bool resume_native=std::getenv("VF2_TGP_RESUMABLE_MATRIX")!=nullptr;
    bool matrix_native=std::getenv("VF2_TGP_NATIVE_MATRIX")!=nullptr;
    bool single=std::getenv("VF2_TGP_BLOCKS")==nullptr||std::getenv("VF2_TGP_SINGLE")!=nullptr;
    bool guarded=std::getenv("VF2_TGP_FETCH_GUARDS")!=nullptr;
    bool private_regions=std::getenv("VF2_NO_TGP_PRIVATE_REGIONS")==nullptr;
    bool reference_counts=std::getenv("VF2_TGP_COUNTER_REFERENCE")!=nullptr;
    bool native_jit=std::getenv("VF2_TGP_NATIVE_JIT")!=nullptr;
    bool packed_handlers=std::getenv("VF2_TGP_PACKED_HANDLERS")!=nullptr;
#if defined(VF2_TGP_PROFILED_REGIONS)
    bool profiled_regions=std::getenv("VF2_TGP_HOT_REGIONS")!=nullptr;
#else
    static constexpr bool profiled_regions=false;
#endif
#if defined(DAYTONA_TGP_COMMAND36_REGIONS)
    bool daytona_fused_dispatch=std::getenv("DAYTONA_TGP_FUSED_DISPATCH")!=nullptr;
    bool daytona_repeat_moves=std::getenv("DAYTONA_TGP_REPEAT_MOVES")!=nullptr;
    bool daytona_cached_dispatch=std::getenv("DAYTONA_TGP_CACHED_DISPATCH")!=nullptr;
    bool daytona_trace_cache=std::getenv("DAYTONA_TGP_TRACE_CACHED_DISPATCH")!=nullptr;
    bool daytona_command36=[] {
        const char *value=std::getenv("DAYTONA_TGP_COMMAND36");
        return value&&value[0]=='1'&&value[1]=='\0';
    }();
#endif
    bool cached_entry=std::getenv("VF2_TGP_CACHED_ENTRY")&&!sample_pc&&!interpreter&&
        resume_native&&matrix_native&&single&&!guarded&&private_regions&&!reference_counts&&
        !native_jit&&!g_tgp_command_samples;
#endif
};
}

void mb86233_device::execute_run_impl(const u32 *mapped, const uint64_t *mapped_revision)
{
    // These launch options share one initialization check per DSP slice.
    static const tgp_slice_options options;
    const bool sample_pc=options.sample_pc;
#if defined(VF2_TGP_AOT)
    const auto run_direct_handlers = [&](const compiled_entry *entries) {
        uint32_t instructions=0;
        while(m_icount>0) {
            m_ppc=m_pc++;
            ++instructions;
            entries[m_ppc&0xfff].run(this);
        }
        m_compiled_instructions+=instructions;
    };
#if defined(DAYTONA_TGP_COMMAND36_REGIONS)
    // Only the command table chosen after a complete uploaded-program
    // comparison is cached. Hooks and live matrix continuations still
    // enter through the general dispatcher.
    if(m_cached_daytona_dispatch && mapped && mapped==m_checked_program &&
       *mapped_revision && *mapped_revision==m_checked_revision &&
       m_block_program_matches && m_fast_ram_checked && m_fast_ram.low && m_fast_ram.high &&
       !on_instruction && !m_pending_matrix.active) {
        m_private_regions_allowed=options.single&&options.private_regions;
        m_native_matrix_allowed=options.matrix_native;
        m_native_resume_allowed=options.matrix_native&&options.resume_native;
        if(options.daytona_trace_cache&&!m_cached_daytona_reported) {
            std::fprintf(stderr,"Daytona DSP cached dispatch hit\n");
            m_cached_daytona_reported=true;
        }
        run_direct_handlers(m_cached_daytona_dispatch);
        return;
    }
#endif
    const compiled_entry *program=options.interpreter?nullptr:compiled_program();
    const auto run_compiled_handlers=[&] {
        // This counter is diagnostic; update it once after the slice.
        uint32_t instructions=0;
        if(options.packed_handlers){
            // Keep opcodes in the validation table and use half the ARMv7
            // stride for dispatch, including entry through the cached path.
            static const auto handlers=[] {
                const auto *entries=compiled_program();
                std::array<decltype(compiled_entry::run),4096> packed{};
                for(unsigned pc=0;pc<packed.size();++pc)packed[pc]=entries[pc].run;
                return packed;
            }();
            while(m_icount>0){m_ppc=m_pc++;++instructions;handlers[m_ppc&0xfff](this);}
            m_compiled_instructions+=instructions;
        }else run_direct_handlers(program);
    };
#if defined(VF2_TGP_PROFILED_REGIONS)
    const auto profiled_handlers=[]() -> const compiled_entry * {
        static const auto profiled_program=[] {
            std::array<compiled_entry,4096> table{};
            std::copy_n(compiled_program(),table.size(),table.begin());
            const auto install=[&]<u16 Pc,u32... Words>(){
                static_assert((private_instruction(Words)&&...));
                static_assert(((Words>>26<0x10||Words>>26>0x1f||private_register(Words>>24))&&...));
                constexpr compiled_entry expected[]={{Words,&compiled_instruction<Words>}...};
                if(std::equal(std::begin(expected),std::end(expected),table.begin()+Pc,
                              [](const compiled_entry &a,const compiled_entry &b){
                                  return a.opcode==b.opcode&&a.run==b.run;
                              }))
                    table[Pc].run=&private_region_entry<Pc,Words...>;
            };
#define VF2_PROFILED_REGION(pc,...) install.template operator()<pc,__VA_ARGS__>();
            VF2_TGP_PROFILED_REGIONS(VF2_PROFILED_REGION)
#undef VF2_PROFILED_REGION
            return table;
        }();
        return profiled_program.data();
    };
    static const bool cache_profiled=std::getenv("VF2_TGP_CACHE_PROFILED_ENTRY")!=nullptr;
    // A stable mapped image already selected these private handlers. A different
    // program or pending continuation still enters normal validation.
    if(cache_profiled&&options.profiled_regions&&options.cached_entry&&!options.packed_handlers&&
#if defined(DAYTONA_TGP_COMMAND36_REGIONS)
       !options.daytona_command36&&
#endif
       program&&mapped&&mapped==m_checked_program&&m_block_program_matches&&
       *mapped_revision&&*mapped_revision==m_checked_revision&&!on_instruction&&
       m_fast_ram_checked&&m_private_regions_allowed&&m_native_resume_allowed&&!m_pending_matrix.active){
        run_direct_handlers(profiled_handlers());
        return;
    }
#endif
    // A previous slice validated this immutable mapped image and installed
    // the exact capabilities needed by the ordinary compiled-handler loop.
    // Any changed capability, pending continuation, or hook uses normal entry.
    if(!options.profiled_regions&&options.cached_entry&&program&&mapped&&mapped==m_checked_program&&m_block_program_matches&&
       *mapped_revision&&*mapped_revision==m_checked_revision&&!on_instruction&&
       m_private_regions_allowed&&m_native_resume_allowed&&!m_pending_matrix.active){
        run_compiled_handlers();
        return;
    }
#endif
    if(!m_fast_ram_checked){
        if(!std::getenv("VF2_TGP_SLOW_RAM"))m_fast_ram=m_bus->direct_internal_ram();
        if(!m_fast_ram.low||!m_fast_ram.high)m_fast_ram={};
        m_fast_ram_checked=true;
    }
#if defined(VF2_TGP_AOT)
    const u32 *flat=nullptr;
    uint64_t revision=0;
    if(program&&!options.guarded&&!on_instruction){
        flat=mapped?mapped:m_bus->program_data();
        if(flat)revision=mapped?*mapped_revision:m_bus->program_revision();
    }
    if(revision&&(revision!=m_checked_revision||flat!=m_checked_program)){
        m_block_program_matches=true;
        for(unsigned i=0;i<4096;i++)if(flat[i]!=program[i].opcode){m_block_program_matches=false;break;}
        m_checked_revision=revision;m_checked_program=flat;
    }
    const bool trusted=revision&&m_block_program_matches;
    m_private_regions_allowed=trusted&&options.single&&options.private_regions&&!sample_pc&&m_fast_ram.low;
    m_native_matrix_allowed=trusted&&options.matrix_native&&!sample_pc;
    m_native_resume_allowed=m_native_matrix_allowed&&options.resume_native&&(mapped||m_bus->isolated_internal_ram());
    if(m_pending_matrix.active){
        if(!m_native_resume_allowed||revision!=m_pending_matrix.revision||flat!=m_pending_matrix.program)
            materialize_native_matrix();
        else advance_native_matrix_pending();
        if(m_icount<=0)return;
    }
    const compiled_block_entry *blocks=trusted&&!options.single&&!sample_pc?compiled_blocks():nullptr;
    if(trusted&&!blocks){
#if defined(VF2_TGP_PROFILED_REGIONS)
        if(options.profiled_regions&&m_private_regions_allowed&&!options.native_jit&&
           !options.packed_handlers&&!options.reference_counts&&!g_tgp_command_samples){
            program=profiled_handlers();
        }
#endif
#if defined(DAYTONA_TGP_COMMAND36_REGIONS)
        if(options.daytona_command36&&!sample_pc&&!options.native_jit&&!options.reference_counts&&
           !options.packed_handlers&&m_fast_ram.low&&m_fast_ram.high){
            // This alternate AOT table is selected only after verifying the entire
            // uploaded DSP image and callback-free instruction fetches.
            static const auto command36=[] {
                std::array<compiled_entry,4096> table{};
                std::copy_n(compiled_program(),table.size(),table.begin());
                const auto region=[]<u16 First,u32... Words>() {
                    static_assert((private_instruction(Words)&&...));
                    static_assert(((Words>>26<0x10||Words>>26>0x1f||
                                    private_register(Words>>24))&&...));
                    constexpr int cycles=(private_cycles(Words)+...);
                    constexpr u32 first[]={Words...};
                    return +[](mb86233_device *cpu){
                        if(cpu->m_ppc!=First||cpu->m_r!=1||cpu->m_stall||cpu->m_icount<cycles){
                            cpu->execute_one(std::integral_constant<u32,first[0]>{});
                            return;
                        }
                        (cpu->private_step<Words>(),...);
                        cpu->m_pc=First+sizeof...(Words);
                        cpu->m_ppc=cpu->m_pc-1;
                        cpu->m_icount-=cycles;
                        cpu->m_compiled_instructions+=sizeof...(Words)-1;
                        cpu->m_block_instructions+=sizeof...(Words);
                        ++cpu->m_blocks_run;
                    };
                };
                const auto repeat=[]<u16 Address,u32 Word>() {
                    return +[](mb86233_device *cpu){
                        // Retain every external read and its precise guest timestamp.
                        // Collapse repeated handler dispatch only while the original
                        // instruction leaves the PC here and the slice can continue.
                        constexpr auto word=std::integral_constant<u32,Word>{};
                        unsigned count=1;
                        cpu->execute_one(word);
                        if(cpu->m_ppc==Address){
                            while(cpu->m_icount>0&&cpu->m_pc==Address){
                                cpu->m_ppc=cpu->m_pc++;
                                cpu->execute_one(word);
                                ++count;
                            }
                        }
                        cpu->m_compiled_instructions+=count-1;
                        if(count>1){
                            cpu->m_block_instructions+=count;
                            ++cpu->m_blocks_run;
                        }
                    };
                };
#define DAYTONA_REGION(first, ...) table[first].run=region.template operator()<first,__VA_ARGS__>();
#define DAYTONA_REPEAT(address, word) table[address].run=repeat.template operator()<address,word>();
                DAYTONA_TGP_COMMAND36_REGIONS(DAYTONA_REGION,DAYTONA_REPEAT)
                const auto intake=[]<u16 First>(){
                    static_assert(First==0x04d||First==0x054);
                    constexpr int minimum=First==0x04d?7:4;
                    constexpr u32 first_opcode=First==0x04d?0x1c1dc07fu:0x1c1f3214u;
                    return +[](mb86233_device *cpu){
                        if(cpu->m_ppc!=First||cpu->m_r!=1||cpu->m_stall||cpu->m_icount<minimum){
                            cpu->execute_one(std::integral_constant<u32,first_opcode>{});
                            return;
                        }
                        int done=0;
                        if constexpr(First==0x04d){
                            cpu->execute_one<true>(std::integral_constant<u32,0x1c1dc07fu>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x3c000014u>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x1c1f1815u>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x1c1f320cu>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x1c1f2014u>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x1f7db800u>{});
                            cpu->m_ppc=0x053;
                            cpu->m_pc=0x054;
                            cpu->execute_one<true>(std::integral_constant<u32,0xfe0000a1u>{});
                            done=7;
                        }
                        if((First==0x054||cpu->m_pc==0x054)&&cpu->m_icount-done>=4){
                            cpu->execute_one<true>(std::integral_constant<u32,0x1c1f3214u>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x39000058u>{});
                            cpu->execute_one<true>(std::integral_constant<u32,0x1f5db800u>{});
                            cpu->m_ppc=0x057;
                            cpu->m_pc=0x058;
                            cpu->execute_one<true>(std::integral_constant<u32,0xbf624019u>{});
                            done+=4;
                            // All in-range entries are validated unconditional branches.
                            if(cpu->m_icount>done&&cpu->m_pc>=0x058&&cpu->m_pc<=0x0a0){
                                cpu->m_ppc=cpu->m_pc;
                                cpu->m_pc=u16(compiled_program()[cpu->m_pc].opcode);
                                ++done;
                            }
                        }
                        cpu->m_icount-=done;
                        cpu->m_compiled_instructions+=done-1;
                        cpu->m_block_instructions+=done;
                        ++cpu->m_blocks_run;
                    };
                };
                constexpr std::array<u32,11> intake_words{
                    0x1c1dc07f, 0x3c000014, 0x1c1f1815, 0x1c1f320c, 0x1c1f2014,
                    0x1f7db800, 0xfe0000a1, 0x1c1f3214, 0x39000058, 0x1f5db800,
                    0xbf624019
                };
                const bool exact_intake=
                    std::equal(intake_words.begin(),intake_words.end(),table.begin()+0x04d,
                               [](u32 word,const compiled_entry &entry){return entry.opcode==word;})&&
                    std::all_of(table.begin()+0x058,table.begin()+0x0a1,
                                [](const compiled_entry &entry){return entry.opcode>>16==0xbf60;});
                if(options.daytona_fused_dispatch&&exact_intake){
                    table[0x04d].run=intake.template operator()<0x04d>();
                    table[0x054].run=intake.template operator()<0x054>();
                }
                if(options.daytona_repeat_moves&&table[0x0ea].opcode==0x1c1343a1u&&
                   table[0x0f1].opcode==0x1c0f43a1u){
                    table[0x0ea].run=&compiled_repeat_move<0x1c1343a1u,true>;
                    table[0x0f1].run=&compiled_repeat_move<0x1c0f43a1u,true>;
                }
#undef DAYTONA_REPEAT
#undef DAYTONA_REGION
                return table;
            }();
            program=command36.data();
            if(options.daytona_cached_dispatch&&mapped)
                m_cached_daytona_dispatch=program;
        }
#endif
        // The bus guarantees this revision cannot change within run(). The
        // complete image was checked above, so individual native handlers need
        // neither virtual instruction fetches nor repeated opcode guards.
        if(options.native_jit&&vf2_tgp_native&&!sample_pc&&!g_tgp_command_samples&&!options.reference_counts){
            uint32_t instructions=0;
            while(m_icount>0){
                if(vf2_tgp_native(*this))continue;
                m_ppc=m_pc++;++instructions;program[m_ppc&0xfff].run(this);
            }
            m_compiled_instructions+=instructions;return;
        }
        if(sample_pc){
            uint32_t instructions=0;
            while(m_icount>0){m_ppc=m_pc++;vf2_tgp_sample::instruction(m_ppc,m_sample_command);++instructions;program[m_ppc&0xfff].run(this);}
            m_compiled_instructions+=instructions;
            vf2_tgp_sample::leave();
        }else if(options.reference_counts){
            while(m_icount>0){m_ppc=m_pc++;++m_compiled_instructions;program[m_ppc&0xfff].run(this);}
        }else{
            run_compiled_handlers();
        }
        return;
    }
#endif
	while(m_icount>0){
#if defined(VF2_TGP_AOT)
        if(blocks&&!on_instruction){
            unsigned done=blocks[(m_pc&0xfff)>>3].run(this,m_pc&7);
            if(done){++m_blocks_run;m_block_instructions+=done;continue;}
        }
#endif
		m_ppc=m_pc;
		if(sample_pc)vf2_tgp_sample::instruction(m_ppc,m_sample_command);
		if(on_instruction)on_instruction(m_pc);
		u16 fetched=m_pc;u32 opcode=m_bus->prog_r(m_pc++);
#if defined(VF2_TGP_AOT)
		const compiled_entry *entry=program?program+(fetched&0xfff):nullptr;
		if(entry&&entry->opcode==opcode){m_compiled_instructions++;entry->run(this);}
		else {m_interpreted_instructions++;execute_one(opcode);}
#else
		(void)fetched;execute_one(opcode);
#endif
	}
    if(sample_pc)vf2_tgp_sample::leave();
}
#undef MB86233_INLINE

#undef VF2_TGP_HELPER

#if defined(VF2_TGP_AOT)
template<u32 Opcode,bool TrustedRepeat> void mb86233_device::compiled_repeat_move(mb86233_device *cpu)
{
    constexpr bool high_to_low=Opcode==0x1c1343a1u;
    constexpr bool low_to_high=Opcode==0x1c0f43a1u;
    constexpr bool ram_fill=Opcode==0x1c0f4200u;
    constexpr bool a_fill=Opcode==0x1c1c21a1u;
    static_assert(high_to_low||low_to_high||ram_fill||a_fill);
    // A trusted, unobserved slice can take consecutive pure MOVs at once.
    // Keep each word's forward read/write order even if RAM windows alias.
    if((TrustedRepeat||cpu->m_native_matrix_allowed)&&cpu->m_icount>0&&cpu->m_r!=1&&
       !cpu->m_stall&&cpu->m_fast_ram.low&&cpu->m_fast_ram.high){
        const unsigned remaining=cpu->m_r?cpu->m_r:256;
        const unsigned count=std::min(unsigned(cpu->m_icount),remaining);
        const unsigned x0=cpu->m_x0,x1=cpu->m_x1;
        constexpr unsigned dest_limit=high_to_low||a_fill?256:512;
        constexpr unsigned source_limit=high_to_low?512:256;
        if(count>=2&&x1+count<=dest_limit&&(!(high_to_low||low_to_high)||x0+count<=source_limit)){
            const auto ram=cpu->m_fast_ram;
            if constexpr(high_to_low){
                for(unsigned i=0;i<count;++i)ram.low[x1+i]=ram.high[x0+i];
            }else if constexpr(low_to_high){
                for(unsigned i=0;i<count;++i)ram.high[x1+i]=ram.low[x0+i];
            }else if constexpr(ram_fill){
                for(unsigned i=0;i<count;++i)ram.high[x1+i]=ram.low[0];
            }else{
                std::fill_n(ram.low+x1,count,cpu->m_a);
            }
            if constexpr(high_to_low||low_to_high)cpu->m_x0+=count;
            cpu->m_x1+=count;
            cpu->m_icount-=int(count);
            cpu->m_compiled_instructions+=count-1; // The dispatch already counted one MOV.
            cpu->m_r=count==remaining?1:u8(remaining-count);
            if constexpr(TrustedRepeat){
                ++cpu->m_blocks_run;
                cpu->m_block_instructions+=count;
            }
            if(count!=remaining)cpu->m_pc=cpu->m_ppc;
            return;
        }
    }
    compiled_instruction<Opcode>(cpu);
}
#endif

#if defined(VF2_TGP_AOT)
template<unsigned FirstColumn,unsigned SecondColumn>
bool mb86233_device::native_rotation()
{
    auto *cpu=this;
    constexpr int cycles=33;
    constexpr unsigned instructions=20;
    if(cpu->m_icount>=cycles&&cpu->m_r==1&&
        !cpu->m_stall&&cpu->m_fast_ram.low&&cpu->m_fast_ram.high&&cpu->m_bus->isolated_internal_ram()&&
        cpu->m_b0==0&&cpu->m_b1==0x200&&cpu->m_x0==cpu->m_x1&&cpu->m_x0<=503){
        const auto finite=[](u32 bits){return (bits&0x7f800000u)!=0x7f800000u;};
        const u32 *trig=cpu->m_fast_ram.low+0xc;
        const u32 cosine=trig[0],sine=trig[1];
        u32 *matrix=cpu->m_fast_ram.high+cpu->m_x0;
        const bool overlap=uintptr_t(trig)<uintptr_t(matrix+9)&&uintptr_t(matrix)<uintptr_t(trig+2);
        if(!overlap&&finite(cosine)&&finite(sine)&&
            std::all_of(matrix+FirstColumn,matrix+FirstColumn+3,finite)&&
            std::all_of(matrix+SecondColumn,matrix+SecondColumn+3,finite)){
            u32 first[3],second[3],last_product=0;
            const float c=u2f(cosine),s=u2f(sine);
            for(unsigned row=0;row<3;++row){
                const float x=u2f(matrix[FirstColumn+row]),y=u2f(matrix[SecondColumn+row]);
                const float xc=x*c,ys=y*s,xs=x*s,yc=y*c;
                first[row]=f2u(xc-ys);
                second[row]=f2u(xs+yc);
                last_product=f2u(yc);
            }
            if(std::all_of(first,first+3,finite)&&std::all_of(second,second+3,finite)){
                cpu->m_a=matrix[SecondColumn+2];
                cpu->m_b=cosine;
                cpu->m_d=cpu->m_alu_r1=second[2];
                cpu->m_p=cpu->m_alu_r2=last_product;
                cpu->m_alu_stmask=F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
                cpu->stset_set_sz_fp(cpu->m_d);
                cpu->alu_update_st();
                std::copy_n(first,3,matrix+FirstColumn);
                std::copy_n(second,3,matrix+SecondColumn);
                cpu->m_ppc+=instructions-1;
                cpu->m_pc=cpu->m_ppc+1;
                cpu->m_icount-=cycles;
                cpu->m_compiled_instructions+=instructions-1; // entry already counted
                ++cpu->m_native_rotations;
                return true;
            }
        }
    }
    return false;
}

template<unsigned FirstColumn,unsigned SecondColumn>
bool mb86233_device::start_native_rotation_pending()
{
    const matrix_timing *timing=native_matrix_timing(m_ppc);
    if(!timing||m_icount<=0||m_icount>=33||m_r!=1||m_stall||!m_fast_ram.low||
       !m_fast_ram.high||m_x0>500)return false;
    save_native_matrix_state(0x200+m_x0,9);
    const int budget=m_icount;
    const uint64_t instructions=m_compiled_instructions;
    m_icount=33;
    const bool computed=native_rotation<FirstColumn,SecondColumn>();
    m_icount=budget;
    if(!computed)return false;
    ++m_resumed_rotations;
    activate_native_matrix_pending(timing,instructions);
    return true;
}

template<unsigned FirstColumn,unsigned SecondColumn,u32 Opcode>
void mb86233_device::rotation_entry(mb86233_device *cpu)
{
    static const bool enabled=std::getenv("VF2_TGP_NATIVE_ROTATION")!=nullptr;
    static const bool resume=std::getenv("VF2_TGP_RESUMABLE_ROTATION")!=nullptr;
    if(enabled&&cpu->m_native_matrix_allowed){
        if(cpu->m_icount>=33){
            if(cpu->native_rotation<FirstColumn,SecondColumn>())return;
        }else if(resume&&cpu->m_native_resume_allowed&&
                 cpu->start_native_rotation_pending<FirstColumn,SecondColumn>())return;
    }
    cpu->execute_one(std::integral_constant<u32,Opcode>{});
}

#endif

#if defined(VF2_TGP_AOT)
template<bool WriteMatrix>
bool mb86233_device::native_translation()
{
    constexpr int cycles=WriteMatrix?37:31;
    constexpr unsigned instructions=WriteMatrix?24:18;
    if(m_icount<cycles||m_r!=1||m_stall||!m_fast_ram.low||!m_fast_ram.high||
       !m_bus->isolated_internal_ram()||m_fast_ram.low[0]!=0)return false;

    const unsigned index=WriteMatrix?u16(m_fast_ram.low[0x3f]):m_x0;
    if(index>500)return false;
    u32 *matrix=m_fast_ram.high+index;
    u32 *low=m_fast_ram.low;
    // Both banks must be independent for the combined writes to preserve the
    // original load order and for prefix reconstruction to be safe.
    if(uintptr_t(low)<uintptr_t(matrix+12)&&uintptr_t(matrix)<uintptr_t(low+256))return false;

    const auto finite=[](u32 bits){return (bits&0x7f800000u)!=0x7f800000u;};
    if(!std::all_of(low+2,low+6,finite)||!std::all_of(matrix,matrix+12,finite))return false;
    const float unit=u2f(low[2]),x=u2f(low[3]),y=u2f(low[4]),z=u2f(low[5]);
    u32 result[3],last_product=0;
    for(unsigned row=0;row<3;++row){
        const float first=u2f(matrix[row])*x;
        const float second=u2f(matrix[row+3])*y;
        const float third=u2f(matrix[row+6])*z;
        const float fourth=u2f(matrix[row+9])*unit;
        result[row]=f2u(((first+second)+third)+fourth);
        last_product=f2u(fourth);
    }
    if(!std::all_of(result,result+3,finite))return false;

    m_a=matrix[11];m_b=low[2];
    m_d=m_alu_r1=result[2];m_p=m_alu_r2=last_product;
    m_alu_stmask=F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
    stset_set_sz_fp(m_d);
    alu_update_st();
    m_b0=0;
    std::copy_n(result,3,low+6);
    if constexpr(WriteMatrix){
        m_b1=0;m_x0=m_x1=index;
        std::copy_n(result,3,matrix+9);
    }
    m_ppc+=instructions-1;m_pc=m_ppc+1;
    m_icount-=cycles;
    m_compiled_instructions+=instructions-1;
    ++m_native_translations;
    return true;
}

template<bool WriteMatrix>
bool mb86233_device::start_native_translation_pending()
{
    constexpr int cycles=WriteMatrix?37:31;
    const matrix_timing *timing=native_matrix_timing(m_ppc);
    if(!timing||m_icount<=0||m_icount>=cycles||m_r!=1||m_stall||
       !m_fast_ram.low||!m_fast_ram.high)return false;
    if constexpr(WriteMatrix){
        const unsigned index=u16(m_fast_ram.low[0x3f]);
        if(index>500)return false;
        save_native_matrix_state(6,3,0x209+index,3);
    }else{
        save_native_matrix_state(6,3);
    }
    const int budget=m_icount;
    const uint64_t instructions=m_compiled_instructions;
    m_icount=cycles;
    const bool computed=native_translation<WriteMatrix>();
    m_icount=budget;
    if(!computed)return false;
    ++m_resumed_translations;
    activate_native_matrix_pending(timing,instructions);
    return true;
}

template<bool WriteMatrix,u32 Opcode>
void mb86233_device::translation_entry(mb86233_device *cpu)
{
    constexpr int cycles=WriteMatrix?37:31;
    static const bool enabled=std::getenv("VF2_TGP_NATIVE_TRANSLATION")!=nullptr;
    if(enabled&&cpu->m_native_matrix_allowed){
        if(cpu->m_icount>=cycles){
            if(cpu->native_translation<WriteMatrix>())return;
        }else if(cpu->m_native_resume_allowed&&cpu->start_native_translation_pending<WriteMatrix>()){
            return;
        }
    }
    cpu->execute_one(std::integral_constant<u32,Opcode>{});
}
#endif
