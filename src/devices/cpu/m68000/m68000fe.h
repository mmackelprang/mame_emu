// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    m68000fe.h

    Front-end for the m68000 DRC recompiler.

    Boundary-L skeleton: this is the platform-neutral decode frontend that
    walks a basic block from a start PC and turns each opcode into an
    opcode_desc, reusing the GENERATED descriptor table (m68000-drcdesc.ipp,
    the interpreter's decode truth) rather than re-deriving decode.  It emits
    NO UML; it only describes.  The native UML emission that consumes these
    descriptions lands at boundary M.

***************************************************************************/
#ifndef MAME_CPU_M68000_M68000FE_H
#define MAME_CPU_M68000_M68000FE_H

#pragma once

#include "m68000.h"

#include "cpu/drcfe.h"

#include <bitset>
#include <cassert>


//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// The m68000 only needs a handful of register categories tracked at this
// boundary; reserve a small fixed pool of bit positions (a static_assert in
// reset() guards the bound).  The bit positions mirror the drc_reg category
// enum order in m68000-drcdesc.ipp.
class m68000_device::opcode_desc : public opcode_desc_base<opcode_desc, 32>
{
public:
	enum
	{
		REG_BIT_DN  = 0,    // data register category
		REG_BIT_AN  = 1,    // address register category
		REG_BIT_CCR = 2,    // condition codes
		REG_BIT_SR  = 3,    // full status register (supervisor)
		REG_BIT_USP = 4,    // user stack pointer
		REG_BIT_PC  = 5,    // program counter
		REG_BIT_SP  = 6,    // active stack pointer (A7)

		REG_BIT_COUNT
	};

	offs_t          physpc;                 // physical PC of this opcode
	u16             opword;                 // copy of the opcode word
	u8              cycles;                 // number of cycles needed to execute (boundary M fills this)

	// Translate a drc_reg category bitmask (from the descriptor table) into
	// regin / regout bits.  The DRC_REG_* values are powers of two in the
	// same category order as the REG_BIT_* positions above.
	void set_regs_used(u16 drc_reg_mask)     { set_reg_bits(regin, drc_reg_mask); }
	void set_regs_modified(u16 drc_reg_mask) { set_reg_bits(regout, drc_reg_mask); }

	void reset(offs_t curpc, bool in_delay_slot)
	{
		static_assert(REG_BIT_COUNT <= 32);

		opcode_desc_base::reset(curpc, in_delay_slot);

		physpc = curpc;
		opword = 0;
		cycles = 0;
	}

private:
	static void set_reg_bits(regmask &mask, u16 drc_reg_mask)
	{
		// DRC_REG_DN..DRC_REG_SP == bit 0..6 of the category mask; map each set
		// category bit to its fixed REG_BIT_* position.
		for (unsigned i = 0; i < REG_BIT_COUNT; i++)
			if (drc_reg_mask & (1u << i))
				mask.set(i);
	}
};


class m68000_device::frontend : public drc_frontend_base<opcode_desc>
{
public:
	// construction/destruction
	frontend(m68000_device *m68k, u32 window_start, u32 window_end, u32 max_sequence);
	~frontend();

	opcode_desc const *describe_code(offs_t startpc);

private:
	bool describe(opcode_desc &desc, opcode_desc const *prev);

	// extension-word byte count implied by one EA mode + operand size (best-effort)
	static unsigned ea_ext_bytes(drc_ea ea, drc_size size);

	// internal state
	m68000_device *m_m68k;
};

#endif // MAME_CPU_M68000_M68000FE_H
