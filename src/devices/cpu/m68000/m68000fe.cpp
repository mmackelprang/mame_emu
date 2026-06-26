// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    m68000fe.cpp

    Front-end for the m68000 DRC recompiler (boundary-L skeleton).

    describe() consumes the GENERATED descriptor table (s_drc_desc_table[],
    from m68000-drcdesc.ipp) rather than re-deriving decode: it fetches the
    opcode word, finds the matching row by (opword & mask) == value, and fills
    the opcode_desc's length / register-use / flow flags from that row.  It
    emits NO UML.

***************************************************************************/

#include "emu.h"
#include "m68000fe.h"

#include "cpu/drcfe.ipp"


//**************************************************************************
//  M68000 FRONTEND
//**************************************************************************

//-------------------------------------------------
//  frontend - constructor
//-------------------------------------------------

m68000_device::frontend::frontend(m68000_device *m68k, u32 window_start, u32 window_end, u32 max_sequence)
	: drc_frontend_base(m68k->space_config(AS_PROGRAM)->page_shift(), window_start, window_end, max_sequence)
	, m_m68k(m68k)
{
}

m68000_device::frontend::~frontend()
{
}

m68000_device::opcode_desc const *m68000_device::frontend::describe_code(offs_t startpc)
{
	return do_describe_code(
			[this] (opcode_desc &desc, opcode_desc const *prev) { return describe(desc, prev); },
			startpc);
}


//-------------------------------------------------
//  describe - build a description of a single
//  instruction from the generated descriptor table
//-------------------------------------------------

bool m68000_device::frontend::describe(opcode_desc &desc, opcode_desc const *prev)
{
	// fetch the opcode word from the opcodes space without side effects (mirrors
	// state_import's machine().disable_side_effects() wrapping at m68000.cpp).
	// 68000 opcodes are 16-bit big-endian and word-aligned.
	u16 opword;
	{
		auto dis = m_m68k->machine().disable_side_effects();
		opword = m_m68k->m_s_opcodes->read_word(desc.physpc);
	}
	desc.opword = opword;

	// Look up the matching descriptor row.  A linear scan is fine for the
	// skeleton; boundary M may index s_drc_desc_table[] by opcode for speed.
	const drc_desc *row = nullptr;
	for (const drc_desc &candidate : s_drc_desc_table)
	{
		if ((opword & candidate.mask) == candidate.value)
		{
			row = &candidate;
			break;
		}
	}

	// An unmatched opword is an invalid/illegal opcode: leave the description
	// empty (the base will mark it invalid / will-cause-exception via the false
	// return) and end the sequence here.
	if (row == nullptr)
		return false;

	// --- instruction length (best-effort for the descriptor dump) ---
	// 68000 length = 2 (the opcode word) + the extension words implied by the
	// matched descriptor's source + destination EA modes and operand size.  This
	// is best-effort at boundary L (the descriptor dump is NOT on the execution
	// hot path); exact length is boundary M's concern.  ALWAYS produce a nonzero
	// even length (min 2) so the frontend's assert(desc->length > 0) holds.
	unsigned length = 2;
	length += ea_ext_bytes(drc_ea(row->src_ea), drc_size(row->size));
	length += ea_ext_bytes(drc_ea(row->dst_ea), drc_size(row->size));
	if (length < 2)
		length = 2;
	if (length & 1)
		length += 1;            // keep it even (extension words are whole words)
	desc.length = u8(length);

	// --- cycle cost ---
	// 0 at boundary L: the per-instruction cycle cost is boundary M's concern
	// (it comes from the same generated decode truth, emitted as a UML icount
	// decrement).  The dispatcher charges cycles via the interpreter, not here.
	desc.cycles = 0;

	// --- register use ---
	desc.set_regs_used(row->reg_read);
	desc.set_regs_modified(row->reg_write);

	// --- control flow ---
	if (row->flow & DRC_FLOW_BRANCH)
	{
		if (row->flow & DRC_FLOW_CONDITIONAL)
			desc.set_is_conditional_branch();
		if (row->flow & DRC_FLOW_UNCONDITION)
			desc.set_is_unconditional_branch();
	}
	if (row->flow & DRC_FLOW_ENDS_BLOCK)
		desc.set_end_sequence();
	if (row->can_fault)
		desc.set_can_cause_exception();

	// Conservatively end the sequence at anything boundary M will route to a
	// cfunc -- a faulting opcode, or one that touches the supervisor SR / USP /
	// CCR operand forms -- so the skeleton's block walk stops there.
	if (row->can_fault
			|| row->src_ea == DRC_EA_SR || row->dst_ea == DRC_EA_SR
			|| row->src_ea == DRC_EA_USP || row->dst_ea == DRC_EA_USP
			|| row->src_ea == DRC_EA_CCR || row->dst_ea == DRC_EA_CCR)
		desc.set_end_sequence();

	return true;
}


//-------------------------------------------------
//  ea_ext_bytes - extension-word byte count implied
//  by one EA mode + operand size (best-effort)
//-------------------------------------------------

unsigned m68000_device::frontend::ea_ext_bytes(drc_ea ea, drc_size size)
{
	switch (ea)
	{
	// register-direct and simple indirect modes carry no extension words
	case DRC_EA_NONE:
	case DRC_EA_DREG:
	case DRC_EA_AREG:
	case DRC_EA_AIND:
	case DRC_EA_AINC:
	case DRC_EA_ADEC:
	case DRC_EA_REGL:   // the movem register-list word is the operand word, not an EA ext here
	case DRC_EA_CCR:
	case DRC_EA_SR:
	case DRC_EA_USP:
		return 0;

	// 16-bit displacement / brief-extension index modes: one extension word
	case DRC_EA_DISP:
	case DRC_EA_INDX:
	case DRC_EA_PCDIS:
	case DRC_EA_PCIDX:
	case DRC_EA_ABSW:
		return 2;

	// 32-bit absolute: two extension words
	case DRC_EA_ABSL:
		return 4;

	// immediate: byte/word fit in one extension word, long needs two
	case DRC_EA_IMM:
		return (size == DRC_SIZE_L) ? 4 : 2;

	// branch displacement: rel16 needs an extension word, rel8 is in-opcode.
	// The descriptor does not split rel8/rel16, so assume one word (the dump's
	// best-effort; exact resolution is boundary M's concern).
	case DRC_EA_DISPL:
		return 2;

	default:
		return 2;
	}
}
