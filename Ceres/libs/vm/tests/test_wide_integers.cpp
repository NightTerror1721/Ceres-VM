// The 64-bit integer instructions (plan/v2 SPEC 6): results, flags (SPEC 6.3), faults and cycles (SPEC 6.4), with the
// edge cases - a carry across the two words, INT64_MIN / -1, shifts by 0, 31, 32 and 63. The load from a latched device
// register is in tests/e2e/test_wide_machine.cpp, where there is a device.
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <limits>
#include <utility>
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	constexpr u64 Min64 = static_cast<u64>(std::numeric_limits<i64>::min());
	constexpr u64 Max64 = static_cast<u64>(std::numeric_limits<i64>::max());
	constexpr u64 All = ~u64{ 0 };

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
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		void setPair(u8 pair, u64 value)
		{
			_vm.engine().setRegister(static_cast<u8>(pair * 2), static_cast<u32>(value));
			_vm.engine().setRegister(static_cast<u8>(pair * 2 + 1), static_cast<u32>(value >> 32));
		}
		u64 pair(u8 pair) const
		{
			const auto& regs = _vm.engine().registers();
			return static_cast<u64>(regs.getValue(pair * 2u)) | (static_cast<u64>(regs.getValue(pair * 2u + 1u)) << 32);
		}
		void setReg(u8 index, u32 value) { _vm.engine().setRegister(index, value); }
		u32 reg(u8 index) const { return _vm.engine().registers().getValue(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
		u32 pc() const { return _vm.engine().programCounter().value(); }
		Memory& memory() { return _vm.memory(); }
		ExecutionEngine& engine() { return _vm.engine(); }
		CeresVM& vm() { return _vm; }
	};

	struct Flags
	{
		bool z = false, s = false, c = false, o = false;
		bool operator==(const Flags&) const = default;
	};

	Flags flagsOf(const Machine& m) { return { m.flags().zero(), m.flags().sign(), m.flags().carry(), m.flags().overflow() }; }

	// Runs one instruction on x2 and x3 (and r8, r9 for the 32-bit operands), and gives x1 and the flags.
	struct Outcome
	{
		u64 value;
		Flags flags;
	};

	Outcome run(Instruction instruction, u64 a, u64 b = 0, u32 r8 = 0, u32 r9 = 0)
	{
		Machine m{ instruction };
		m.setPair(1, 0xDEADBEEFDEADBEEFu);
		m.setPair(2, a);
		m.setPair(3, b);
		m.setReg(8, r8);
		m.setReg(9, r9);
		m.step();
		return { m.pair(1), flagsOf(m) };
	}

	std::string renderFlags(const Flags& f) { return std::format("z{} s{} c{} o{}", f.z, f.s, f.c, f.o); }
}

TEST(wide_integers, add64_carries_across_the_words_and_sets_the_64_bit_flags)
{
	auto r = run(Instruction::ADD64(1, 2, 3), 0x00000000FFFFFFFFu, 1);
	CHECK_EQ(r.value, u64{ 0x100000000 });
	CHECK_EQ_FMT(r.flags, (Flags{ false, false, false, false }), renderFlags);

	r = run(Instruction::ADD64(1, 2, 3), All, 1);
	CHECK_EQ(r.value, u64{ 0 });
	CHECK_EQ_FMT(r.flags, (Flags{ true, false, true, false }), renderFlags);

	r = run(Instruction::ADD64(1, 2, 3), Max64, 1);
	CHECK_EQ(r.value, Min64);
	CHECK_EQ_FMT(r.flags, (Flags{ false, true, false, true }), renderFlags);
}

TEST(wide_integers, sub64_neg64_and_cmp64_borrow_the_way_sub_does)
{
	auto r = run(Instruction::SUB64(1, 2, 3), 0x100000000u, 1);
	CHECK_EQ(r.value, u64{ 0xFFFFFFFF });
	CHECK_EQ_FMT(r.flags, (Flags{ false, false, false, false }), renderFlags);

	r = run(Instruction::SUB64(1, 2, 3), 0, 1);
	CHECK_EQ(r.value, All);
	CHECK_EQ_FMT(r.flags, (Flags{ false, true, true, false }), renderFlags);

	r = run(Instruction::SUB64(1, 2, 3), Min64, 1);
	CHECK_EQ(r.value, Max64);
	CHECK_EQ_FMT(r.flags, (Flags{ false, false, false, true }), renderFlags);

	r = run(Instruction::NEG64(1, 2), 1);
	CHECK_EQ(r.value, All);
	CHECK_EQ_FMT(r.flags, (Flags{ false, true, true, false }), renderFlags);
	r = run(Instruction::NEG64(1, 2), 0);
	CHECK_EQ_FMT(r.flags, (Flags{ true, false, false, false }), renderFlags);
	r = run(Instruction::NEG64(1, 2), Min64);
	CHECK_EQ(r.value, Min64);
	CHECK(r.flags.o);

	// cmp64 keeps only the flags, and the existing jumps read them: -1 < 1 signed, above it unsigned.
	r = run(Instruction::CMP64(2, 3), All, 1);
	CHECK_EQ(r.value, u64{ 0xDEADBEEFDEADBEEF });
	CHECK(r.flags.s != r.flags.o);    // signed less
	CHECK(!r.flags.c && !r.flags.z);  // unsigned above
	r = run(Instruction::CMP64(2, 3), 0x100000005u, 0x100000005u);
	CHECK(r.flags.z);
	r = run(Instruction::CMP64(2, 3), 0x100000000u, 0xFFFFFFFFu);   // differ only across the words
	CHECK(!r.flags.z && !r.flags.c);
}

TEST(wide_integers, the_products_keep_all_64_bits_and_clear_carry_and_overflow)
{
	auto r = run(Instruction::MULL(1, 8, 9), 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
	CHECK_EQ(r.value, u64{ 0xFFFFFFFE00000001 });
	CHECK_EQ_FMT(r.flags, (Flags{ false, true, false, false }), renderFlags);

	r = run(Instruction::IMULL(1, 8, 9), 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
	CHECK_EQ(r.value, u64{ 1 });
	r = run(Instruction::IMULL(1, 8, 9), 0, 0, 0x80000000u, 2);
	CHECK_EQ(r.value, u64{ 0xFFFFFFFF00000000 });
	CHECK(r.flags.s);

	r = run(Instruction::MUL64(1, 2, 3), 0x100000001u, 0x100000001u);
	CHECK_EQ(r.value, u64{ 0x200000001 });
	r = run(Instruction::MUL64(1, 2, 3), 0x100000000u, 0x100000000u);
	CHECK_EQ(r.value, u64{ 0 });
	CHECK_EQ_FMT(r.flags, (Flags{ true, false, false, false }), renderFlags);
}

TEST(wide_integers, the_divisions_and_int64_min_over_minus_one)
{
	CHECK_EQ(run(Instruction::DIV64(1, 2, 3), 0xFFFFFFFFFFFFFFF0u, 0x10).value, u64{ 0x0FFFFFFFFFFFFFFF });
	CHECK_EQ(run(Instruction::MOD64(1, 2, 3), 0x100000007u, 0x100000000u).value, u64{ 7 });
	CHECK_EQ(run(Instruction::IDIV64(1, 2, 3), static_cast<u64>(i64{ -100 }), 7).value, static_cast<u64>(i64{ -14 }));
	CHECK_EQ(run(Instruction::IMOD64(1, 2, 3), static_cast<u64>(i64{ -100 }), 7).value, static_cast<u64>(i64{ -2 }));

	auto r = run(Instruction::IDIV64(1, 2, 3), Min64, All);
	CHECK_EQ(r.value, Min64);
	CHECK_EQ_FMT(r.flags, (Flags{ false, true, false, true }), renderFlags);
	r = run(Instruction::IMOD64(1, 2, 3), Min64, All);
	CHECK_EQ(r.value, u64{ 0 });
	CHECK(r.flags.z && !r.flags.o);
}

TEST(wide_integers, a_zero_divisor_traps_like_div_and_leaves_the_destination)
{
	for (const Instruction instruction : { Instruction::DIV64(1, 2, 3), Instruction::IDIV64(1, 2, 3), Instruction::MOD64(1, 2, 3),
		Instruction::IMOD64(1, 2, 3) })
	{
		Machine m{ instruction };
		m.setPair(1, 0x1234);
		m.setPair(2, 77);
		m.setPair(3, 0);
		m.step();
		CHECK(m.flags().trap());
		CHECK_EQ(m.pair(1), u64{ 0x1234 });
		CHECK_EQ(m.pc(), Memory::UnrestrictedSegmentStart.value() + 4);
	}
}

TEST(wide_integers, shifts_by_0_31_32_and_63)
{
	const u64 value = 0x8000000180000001u;
	// By 0: the value unchanged, and so are the flags (set here by a compare first).
	{
		Machine m{ Instruction::CMP(0, 0), Instruction::SHL64I(1, 2, 0) };
		m.setPair(2, value);
		m.step(2);
		CHECK_EQ(m.pair(1), value);
		CHECK(m.flags().zero());
	}
	auto r = run(Instruction::SHL64I(1, 2, 31), value);
	CHECK_EQ(r.value, u64{ 0xC000000080000000 });
	CHECK(!r.flags.c);                              // bit 33 went out last
	r = run(Instruction::SHL64I(1, 2, 32), value);
	CHECK_EQ(r.value, u64{ 0x8000000100000000 });
	CHECK(r.flags.c);                               // bit 32
	r = run(Instruction::SHL64I(1, 2, 63), value);
	CHECK_EQ(r.value, u64{ 0x8000000000000000 });
	CHECK(!r.flags.c);                              // bit 1
	r = run(Instruction::SHR64I(1, 2, 32), value);
	CHECK_EQ(r.value, u64{ 0x80000001 });
	CHECK(r.flags.c);                               // bit 31
	r = run(Instruction::SHR64I(1, 2, 63), value);
	CHECK_EQ(r.value, u64{ 1 });
	r = run(Instruction::SAR64I(1, 2, 32), value);
	CHECK_EQ(r.value, u64{ 0xFFFFFFFF80000001 });
	CHECK(r.flags.s);
	r = run(Instruction::SAR64I(1, 2, 63), value);
	CHECK_EQ(r.value, All);
	r = run(Instruction::SAR64I(1, 2, 31), value);
	CHECK_EQ(r.value, u64{ 0xFFFFFFFF00000003 });

	// The register forms take the low six bits of the count: 64 is 0, 65 is 1.
	CHECK_EQ(run(Instruction::SHL64(1, 2, 8), value, 0, 65).value, value << 1);
	CHECK_EQ(run(Instruction::SHR64(1, 2, 8), value, 0, 32).value, u64{ 0x80000001 });
	CHECK_EQ(run(Instruction::SAR64(1, 2, 8), value, 0, 64).value, value);
}

TEST(wide_integers, sxt64_the_bit_counts_and_mov64)
{
	CHECK_EQ(run(Instruction::SXT64(1, 8), 0, 0, 0x80000000u).value, u64{ 0xFFFFFFFF80000000 });
	CHECK_EQ(run(Instruction::SXT64(1, 8), 0, 0, 0x7FFFFFFFu).value, u64{ 0x7FFFFFFF });

	const auto count = [](Instruction instruction, u64 value)
	{
		Machine m{ instruction };
		m.setPair(2, value);
		m.step();
		return m.reg(5);
	};
	CHECK_EQ(count(Instruction::CLZ64(5, 2), 0), 64u);
	CHECK_EQ(count(Instruction::CTZ64(5, 2), 0), 64u);
	CHECK_EQ(count(Instruction::POPCNT64(5, 2), 0), 0u);
	CHECK_EQ(count(Instruction::CLZ64(5, 2), 0x100000000u), 31u);
	CHECK_EQ(count(Instruction::CTZ64(5, 2), 0x100000000u), 32u);
	CHECK_EQ(count(Instruction::POPCNT64(5, 2), All), 64u);

	CHECK_EQ(run(Instruction::MOV64(1, 2), 0x0123456789ABCDEFu).value, u64{ 0x0123456789ABCDEF });
}

TEST(wide_integers, ldrd_and_strd_take_the_low_word_first_aligned_to_four)
{
	const u32 buffer = 0x00100004u;   // aligned to 4, not to 8
	Machine m{
		Instruction::STRD(8, 2, 0),        // [r8] = x2
		Instruction::LDRD(1, 8, 0),        // x1 = [r8]
		Instruction::STRDX(8, 2, 9),       // [r8 + r9] = x2
		Instruction::LDRDX(3, 8, 9),       // x3 = [r8 + r9]
	};
	m.setPair(2, 0x1122334455667788u);
	m.setReg(8, buffer);
	m.setReg(9, 16);
	m.step(4);
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(buffer)), 0x55667788u);
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(buffer + 4)), 0x11223344u);
	CHECK_EQ(m.pair(1), u64{ 0x1122334455667788 });
	CHECK_EQ(m.pair(3), u64{ 0x1122334455667788 });

	// A pair whose even register is the base: both words are read before either is written.
	Machine own{ Instruction::LDRD(4, 8, 0) };
	own.memory().writeUnchecked<u32>(Address(buffer), 0xAAAAAAAAu);
	own.memory().writeUnchecked<u32>(Address(buffer + 4), 0xBBBBBBBBu);
	own.setReg(8, buffer);
	own.step();
	CHECK_EQ(own.pair(4), u64{ 0xBBBBBBBBAAAAAAAA });
}

TEST(wide_integers, ldrdp_reads_relative_to_the_instruction)
{
	Machine m{ Instruction::LDRDP(1, 8), Instruction::HALT(), Instruction(0x89ABCDEFu), Instruction(0x01234567u) };
	m.step();
	CHECK_EQ(m.pair(1), u64{ 0x0123456789ABCDEF });
}

TEST(wide_integers, a_misaligned_ldrd_is_an_alignment_fault)
{
	Machine m{ Instruction::LDRD(1, 8, 2) };
	m.memory().writeUnchecked<u32>(Address(static_cast<u32>(InterruptNumber::AlignmentFault) * Address::Size), 0x2000u);
	m.setReg(8, 0x00100000u);
	m.setPair(1, 5);
	m.step();
	CHECK_EQ(m.pc(), 0x2000u);
	CHECK_EQ(m.pair(1), u64{ 5 });
}

TEST(wide_integers, cycles_as_spec_6_4_has_them)
{
	const auto cost = [](Instruction instruction, u32 base = 0x00100000u)
	{
		Machine m{ instruction };
		m.setReg(8, base);
		m.setPair(3, 1);
		const u64 before = m.engine().cycles();
		m.step();
		return m.engine().cycles() - before;
	};
	CHECK_EQ(cost(Instruction::ADD64(1, 2, 3)), u64{ 2 });
	CHECK_EQ(cost(Instruction::CMP64(2, 3)), u64{ 2 });
	CHECK_EQ(cost(Instruction::SHL64I(1, 2, 3)), u64{ 2 });
	CHECK_EQ(cost(Instruction::CLZ64(4, 2)), u64{ 2 });
	CHECK_EQ(cost(Instruction::MULL(1, 8, 9)), u64{ 4 });
	CHECK_EQ(cost(Instruction::MUL64(1, 2, 3)), u64{ 6 });
	CHECK_EQ(cost(Instruction::DIV64(1, 2, 3)), u64{ 40 });
	CHECK_EQ(cost(Instruction::IMOD64(1, 2, 3)), u64{ 40 });
	CHECK_EQ(cost(Instruction::SXT64(1, 8)), u64{ 1 });
	CHECK_EQ(cost(Instruction::MOV64(1, 2)), u64{ 1 });
	CHECK_EQ(cost(Instruction::LDRD(1, 8, 0)), u64{ 3 });                // RAM: one access
	CHECK_EQ(cost(Instruction::STRD(8, 2, 0)), u64{ 3 });
	CHECK_EQ(cost(Instruction::LDRD(1, 8, 0), 0xFF010000u), u64{ 9 });   // a device: two, 4 each
}
