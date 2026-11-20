// SPDX-License-Identifier: GPL-3.0+

#pragma once

// (AI-assisted) Detects EE "pure wait loops": loops that only read RAM through gp/sp, do
// arithmetic and branch, so every pass computes the same thing until something outside the EE
// changes memory. Outside changes (DMA completion, interrupt handlers, IOP/SIF transfers) only
// happen at scheduled events, so the recompiler can skip straight to the next event instead of
// executing the spin. Example (GTA San Andreas, renderer waiting for DMA, ~70% of EE events):
//     lw v1,0x10f8(gp); lw v0,0x10fc(gp); bnel v1,v0,...; lbu v0,0x10f0(gp); ...
//     lw v0,-0x3828(gp); beq v0,zero,skip; jalr v0; ...; bne s0,zero,<loop start>
// A guarded indirect call (jalr through a pointer loaded from gp) is allowed; the caller must
// then check at run time that every such pointer slot is zero before skipping.
// Header-only and free of PCSX2 types so it can be tested on a PC. Needs proper console testing.

#include <cstdint>

namespace PS4IdleLoop
{
	constexpr int MAX_INSTRUCTIONS = 32;
	constexpr int MAX_SLOTS = 2;

	struct Result
	{
		bool ok = false;
		int slot_count = 0;
		int16_t slot_offsets[MAX_SLOTS] = {};
		const char* reject = "not analysed";
	};

	namespace detail
	{
		constexpr uint32_t Op(uint32_t i) { return i >> 26; }
		constexpr uint32_t Rs(uint32_t i) { return (i >> 21) & 31; }
		constexpr uint32_t Rt(uint32_t i) { return (i >> 16) & 31; }
		constexpr uint32_t Rd(uint32_t i) { return (i >> 11) & 31; }
		constexpr uint32_t Funct(uint32_t i) { return i & 63; }

		enum class Kind
		{
			Plain,
			Load,
			Branch,
			BranchLikely,
			Call,
			Invalid,
		};

		struct Decoded
		{
			Kind kind = Kind::Invalid;
			uint32_t reads = 0; // register mask
			int write = 0; // 0 = none
			int32_t branch_offset = 0; // in instructions, relative to the delay slot
		};

		inline Decoded Decode(uint32_t i)
		{
			Decoded d;
			const uint32_t op = Op(i), rs = Rs(i), rt = Rt(i), rd = Rd(i), fn = Funct(i);
			const int32_t offset = static_cast<int16_t>(i & 0xffff);
			switch (op)
			{
				case 0x00: // SPECIAL
					if (fn == 0x0f) // sync
						d.kind = Kind::Plain;
					else if (fn == 0x09) // jalr
					{
						d.kind = Kind::Call;
						d.reads = 1u << rs;
						d.write = static_cast<int>(rd);
					}
					else if (fn == 0x00 || fn == 0x02 || fn == 0x03 || fn == 0x38 || fn == 0x3a || fn == 0x3b ||
							 fn == 0x3c || fn == 0x3e || fn == 0x3f) // immediate shifts
					{
						d.kind = Kind::Plain;
						d.reads = 1u << rt;
						d.write = static_cast<int>(rd);
					}
					else if (fn == 0x04 || fn == 0x06 || fn == 0x07 || fn == 0x14 || fn == 0x16 || fn == 0x17 ||
							 (fn >= 0x20 && fn <= 0x27) || fn == 0x2a || fn == 0x2b || (fn >= 0x2c && fn <= 0x2f))
					{
						d.kind = Kind::Plain;
						d.reads = (1u << rs) | (1u << rt);
						d.write = static_cast<int>(rd);
					}
					break;
				case 0x01: // REGIMM: bltz, bgez, bltzl, bgezl (no linking variants)
					if (rt <= 3)
					{
						d.kind = (rt >= 2) ? Kind::BranchLikely : Kind::Branch;
						d.reads = 1u << rs;
						d.branch_offset = offset;
					}
					break;
				case 0x04: case 0x05: case 0x14: case 0x15: // beq, bne, beql, bnel
					d.kind = (op >= 0x14) ? Kind::BranchLikely : Kind::Branch;
					d.reads = (1u << rs) | (1u << rt);
					d.branch_offset = offset;
					break;
				case 0x06: case 0x07: case 0x16: case 0x17: // blez, bgtz, blezl, bgtzl
					if (rt == 0)
					{
						d.kind = (op >= 0x16) ? Kind::BranchLikely : Kind::Branch;
						d.reads = 1u << rs;
						d.branch_offset = offset;
					}
					break;
				case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x18: case 0x19:
					d.kind = Kind::Plain; // addi(u), slti(u), andi, ori, xori, daddi(u)
					d.reads = 1u << rs;
					d.write = static_cast<int>(rt);
					break;
				case 0x0f: // lui
					d.kind = Kind::Plain;
					d.write = static_cast<int>(rt);
					break;
				case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37: // lb lh lw lbu lhu lwu ld
					if (rs == 28 || rs == 29) // gp / sp: RAM, never a hardware register
					{
						d.kind = Kind::Load;
						d.reads = 1u << rs;
						d.write = static_cast<int>(rt);
					}
					break;
				default:
					break;
			}
			d.reads &= ~1u; // $zero
			return d;
		}
	} // namespace detail

	// start: loop start (the back branch target). end: address after the back branch's delay slot.
	// fetch(address) returns the 32-bit instruction there.
	template <typename Fetch>
	Result Analyze(uint32_t start, uint32_t end, Fetch fetch)
	{
		using namespace detail;
		Result result;
		if (end <= start || ((end - start) & 3) != 0)
		{
			result.reject = "bad range";
			return result;
		}
		const int n = static_cast<int>((end - start) / 4);
		if (n < 2 || n > MAX_INSTRUCTIONS)
		{
			result.reject = "loop size";
			return result;
		}

		uint32_t raw[MAX_INSTRUCTIONS];
		Decoded code[MAX_INSTRUCTIONS];
		bool is_delay[MAX_INSTRUCTIONS] = {};
		uint32_t written = 0;
		for (int i = 0; i < n; i++)
		{
			raw[i] = fetch(start + 4 * i);
			code[i] = Decode(raw[i]);
			if (code[i].kind == Kind::Invalid)
			{
				result.reject = "instruction with side effects";
				return result;
			}
			if (code[i].write)
				written |= 1u << code[i].write;
		}
		if (written & ((1u << 28) | (1u << 29)))
		{
			result.reject = "gp/sp written";
			return result;
		}

		// The last branch must close the loop back to its start.
		const int back = n - 2;
		if ((code[back].kind != Kind::Branch && code[back].kind != Kind::BranchLikely) || back + 1 + code[back].branch_offset != 0)
		{
			result.reject = "no back branch";
			return result;
		}

		// Successors per instruction; -1 = none. Edges to index 0 end the iteration; edges past the
		// end or out of the loop exit it. Backward branches out of the loop are rejected.
		int succ[MAX_INSTRUCTIONS][2];
		for (int i = 0; i < n; i++)
			succ[i][0] = succ[i][1] = -1;
		for (int i = 0; i < n; i++)
		{
			const Decoded& d = code[i];
			if (d.kind == Kind::Branch || d.kind == Kind::BranchLikely || d.kind == Kind::Call)
			{
				if (i + 1 >= n || is_delay[i])
				{
					result.reject = "branch at end or in delay slot";
					return result;
				}
				const Decoded& slot = code[i + 1];
				if (slot.kind == Kind::Branch || slot.kind == Kind::BranchLikely || slot.kind == Kind::Call)
				{
					result.reject = "branch in delay slot";
					return result;
				}
				is_delay[i + 1] = true;
			}
		}
		for (int i = 0; i < n; i++)
		{
			const Decoded& d = code[i];
			auto target_index = [&](int t) -> int {
				if (t < 0)
					return -2; // backwards out of the loop
				if (t >= n)
					return -1; // exits forward
				return t;
			};
			if (d.kind == Kind::Branch || d.kind == Kind::Call)
			{
				succ[i][0] = i + 1;
			}
			else if (d.kind == Kind::BranchLikely)
			{
				succ[i][0] = i + 1; // taken: delay slot runs
				succ[i][1] = (i + 2 < n) ? i + 2 : -1; // not taken: skips it
			}
			else if (is_delay[i])
			{
				const Decoded& br = code[i - 1];
				if (br.kind == Kind::Call)
					succ[i][0] = (i + 1 < n) ? i + 1 : -1; // callee returns after the delay slot
				else
				{
					const int t = target_index(i + br.branch_offset);
					if (t == -2)
					{
						result.reject = "branch out of the loop backwards";
						return result;
					}
					if (t > 0 && is_delay[t])
					{
						result.reject = "jump into a delay slot";
						return result;
					}
					succ[i][0] = t;
					if (br.kind == Kind::Branch)
						succ[i][1] = (i + 1 < n) ? i + 1 : -1;
				}
			}
			else
				succ[i][0] = (i + 1 < n) ? i + 1 : -1;
		}
		// Reaching definitions: per instruction and register, a mask of defining instructions;
		// bit 63 = value from before this pass (loop-carried).
		constexpr uint64_t ENTRY = 1ull << 63;
		uint64_t in[MAX_INSTRUCTIONS][32] = {};
		bool reached[MAX_INSTRUCTIONS] = {};
		for (int r = 0; r < 32; r++)
			in[0][r] = ENTRY;
		reached[0] = true;
		bool changed = true;
		for (int guard = 0; changed && guard < 4 * MAX_INSTRUCTIONS; guard++)
		{
			changed = false;
			for (int i = 0; i < n; i++)
			{
				if (!reached[i])
					continue;
				uint64_t out[32];
				for (int r = 0; r < 32; r++)
					out[r] = in[i][r];
				if (code[i].write)
					out[code[i].write] = 1ull << i;
				for (int s = 0; s < 2; s++)
				{
					const int t = succ[i][s];
					if (t <= 0) // exit, or back to the start (end of this pass)
						continue;
					for (int r = 0; r < 32; r++)
					{
						const uint64_t merged = in[t][r] | out[r];
						if (merged != in[t][r] || !reached[t])
							changed = true;
						in[t][r] = merged;
					}
					reached[t] = true;
				}
			}
		}
		if (!reached[back])
		{
			result.reject = "back branch unreachable";
			return result;
		}

		for (int i = 0; i < n; i++)
		{
			if (!reached[i])
				continue;
			for (int r = 1; r < 32; r++)
			{
				if (!(code[i].reads & (1u << r)) || !(written & (1u << r)))
					continue;
				if (in[i][r] & ENTRY)
				{
					result.reject = "value carried between passes";
					return result;
				}
			}
			if (code[i].kind == Kind::Call)
			{
				const int r = static_cast<int>(Rs(raw[i]));
				const uint64_t defs = in[i][r];
				if (r == 0 || (defs & ENTRY) || defs == 0)
				{
					result.reject = "call target not loaded in the loop";
					return result;
				}
				for (int k = 0; k < n; k++)
				{
					if (!(defs & (1ull << k)))
						continue;
					if (Op(raw[k]) != 0x23 || Rs(raw[k]) != 28)
					{
						result.reject = "call target not a gp pointer";
						return result;
					}
					const int16_t offset = static_cast<int16_t>(raw[k] & 0xffff);
					bool known = false;
					for (int s = 0; s < result.slot_count; s++)
						known |= result.slot_offsets[s] == offset;
					if (!known)
					{
						if (result.slot_count == MAX_SLOTS)
						{
							result.reject = "too many call slots";
							return result;
						}
						result.slot_offsets[result.slot_count++] = offset;
					}
				}
			}
		}

		result.ok = true;
		result.reject = nullptr;
		return result;
	}
} // namespace PS4IdleLoop
