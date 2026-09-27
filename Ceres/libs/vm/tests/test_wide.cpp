// The 64-bit instructions in the machine (plan/v2 SPEC 6): the words it refuses and why (SPEC 5.4).
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	class Machine
	{
	private:
		CeresVM _vm;

	public:
		explicit Machine(std::initializer_list<Instruction> program)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;
			usize offset = 0;
			for (Instruction instruction : program)
			{
				_vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			// A handler for the illegal instruction, so the fault is taken and its PC can be seen.
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(InterruptNumber::IllegalInstruction) * Address::Size), Handler);
			_vm.memory().writeUnchecked<u32>(Address(Handler), Instruction::HALT().raw());
			_vm.engine().reset();
		}

		static constexpr u32 Handler = 0x2000u;

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		Address pc() const { return _vm.engine().programCounter(); }
		ExecutionEngine& engine() { return _vm.engine(); }
	};

	// What the machine did with one word: whether it took IllegalInstruction, and the reason it gave.
	struct Refusal
	{
		bool illegal = false;
		u32 reason = 0;
		u32 at = 0;
	};

	Refusal run(Instruction word)
	{
		Machine m{ word };
		Refusal refusal;
		m.engine().setInterruptObserver([&](InterruptNumber n, Address pc, bool entered)
		{
			if (entered && n == InterruptNumber::IllegalInstruction)
			{
				refusal.illegal = true;
				refusal.at = pc.value();
			}
		});
		m.step();
		refusal.reason = m.engine().faultReason();
		return refusal;
	}

	constexpr u32 reasonOf(FaultReason reason) { return static_cast<u32>(reason); }

	// A word with its fields as they are stored, valid or not.
	Instruction word(Opcode opcode, u8 rd, u8 rs, u8 rt = 0, u8 low = 0) { return Instruction::make(opcode, rd, rs, rt, low); }
}

TEST(wide, an_opcode_nobody_has_is_an_unknown_opcode)
{
	for (const u32 opcode : { 0x0Fu, 0x4Fu, 0x7Fu, 0x8Cu, 0xA4u, 0xB3u, 0xEEu, 0xFFu })
	{
		const Refusal refusal = run(Instruction(opcode << 24));
		CHECK(refusal.illegal);
		CHECK_EQ(refusal.reason, reasonOf(FaultReason::UnknownOpcode));
		CHECK_EQ(refusal.at, Memory::UnrestrictedSegmentStart.value());   // at the word itself
	}
}

TEST(wide, an_odd_pair_or_x7_is_a_register_pair)
{
	for (const Instruction candidate : { word(Opcode::ADD64, 1, 2, 4), word(Opcode::MOV64, 14, 2),
		word(Opcode::CMP64, 0, 2, 7), word(Opcode::FMOVD, 3, 2), word(Opcode::MTFD, 2, 14),
		word(Opcode::STRD, 2, 5, 8) })
	{
		const Refusal refusal = run(candidate);
		CHECK(refusal.illegal);
		CHECK_EQ(refusal.reason, reasonOf(FaultReason::RegisterPair));
	}
}

TEST(wide, a_subfield_no_instruction_has_is_a_bad_subfield)
{
	for (const Instruction candidate : { word(Opcode::FCVT, 2, 4, 0, 14), word(Opcode::FCVT, 2, 4, 0, 15),
		word(Opcode::SHI64, 2, 4, 0, 0xC5), word(Opcode::BITS64, 1, 2, 0, 3),
		word(Opcode::FUNARYD, 2, 4, 0, 6) })
	{
		const Refusal refusal = run(candidate);
		CHECK(refusal.illegal);
		CHECK_EQ(refusal.reason, reasonOf(FaultReason::BadSubfield));
	}
}

TEST(wide, int_past_the_vector_table_says_bad_subfield)
{
	const Refusal refusal = run(Instruction::INT(200));
	CHECK(refusal.illegal);
	CHECK_EQ(refusal.reason, reasonOf(FaultReason::BadSubfield));
}
