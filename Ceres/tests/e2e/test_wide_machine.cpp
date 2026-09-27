// The 64-bit instructions with the rest of the machine (plan/v2 SPEC 6): an assembled program that uses them, and a
// 64-bit load from a device register pair that latches.
#include "framework.h"
#include "assemble_helper.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::testing;

namespace
{
	u64 pairOf(const CeresVM& vm, u8 pair)
	{
		const auto& regs = vm.engine().registers();
		return static_cast<u64>(regs.getValue(pair * 2u)) | (static_cast<u64>(regs.getValue(pair * 2u + 1u)) << 32);
	}
}

TEST(wide_machine, an_assembled_program_computes_in_64_bits)
{
	const AssembleResult assembled = assembleSource(
		"@data\r\n"
		"global let a: u64 = 0x00000001FFFFFFFF\r\n"
		"global let b: i64 = -3\r\n"
		"global let out: u64 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldv x1, a\r\n"
		"    ldv x2, b\r\n"
		"    add64 x3, x1, x2\r\n"          // 0x1FFFFFFFC
		"    stv out, x3\r\n"
		"    ldrd x4, [out]\r\n"            // the same back
		"    li64 x5, 0x7FFFFFFFFFFFFFFF\r\n"
		"    shr64 x5, x5, 60\r\n"          // 7
		"    idiv64 x6, x2, x5\r\n"         // -3 / 7 = 0
		"    li r0, 0\r\n"
		"    cmp64 x2, x1\r\n"
		"    jge .done\r\n"                 // -3 < a, signed: not taken
		"    li r0, 1\r\n"
		".done:\r\n"
		"    halt\r\n");
	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	CeresVM vm{};
	CHECK(vm.loadProgram(assembled.program.value()).has_value());
	for (usize i = 0; i < 100 && !vm.engine().isHalted(); ++i)
		vm.engine().step();

	CHECK(vm.engine().isHalted());
	CHECK_EQ(pairOf(vm, 3), u64{ 0x1FFFFFFFC });
	CHECK_EQ(pairOf(vm, 4), u64{ 0x1FFFFFFFC });
	CHECK_EQ(pairOf(vm, 5), u64{ 7 });
	CHECK_EQ(pairOf(vm, 6), u64{ 0 });
	CHECK_EQ(vm.engine().registers().getValue(0), 1u);
}

TEST(wide_machine, ldrd_reads_the_timers_latched_cycle_count_whole)
{
	// Two 32-bit accesses, low first: reading CyclesLow latches CyclesHigh, so the pair is one instant even when the count
	// carries into its high word between the two.
	CeresVM vm{};
	TimerDevice timer{};
	timer.attachTo(vm.io());

	const Address entry = Memory::UnrestrictedSegmentStart;
	vm.memory().writeUnchecked<u32>(entry, Instruction::LDRD(1, 12, 0).raw());
	vm.memory().writeUnchecked<u32>(0_addr, entry.value());
	vm.engine().reset();
	vm.engine().setRegister(12, 0xFF010000u);
	vm.engine().setCycles(0xFFFFFFFEu);

	const u64 before = vm.engine().cycles();
	vm.engine().step();
	const u64 after = vm.engine().cycles();
	const u64 read = pairOf(vm, 1);
	CHECK(read >= before && read <= after);
	CHECK_EQ(read >> 32, u64{ 1 });
	CHECK_EQ(after - before, u64{ 9 });   // 1 + two device accesses of 4

	timer.detachFrom(vm.io());
}
