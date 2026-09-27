// The dma (DmaController): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- The DMA controller's transfer count -------------------------------------------------------

TEST(dma, a_dma_transfer_reports_how_many_bytes_it_moved)
{
	Machine m{ Instruction::NOP() };

	DmaController dma{};
	dma.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "HELLO");

	dma.write(DmaController::SourceRegister, SourceBuffer);
	dma.write(DmaController::DestinationRegister, DestinationBuffer);
	dma.write(DmaController::LengthRegister, 5);
	dma.write(DmaController::CommandRegister, DmaController::CommandStart);

	CHECK_EQ(dma.read(DmaController::StatusRegister) & DmaController::StatusBusy, DmaController::StatusBusy);

	// Five bytes take one cycle: the copy lands on that cycle's event, run before the next instruction.
	m.step(1);
	CHECK_EQ(dma.read(DmaController::StatusRegister) & DmaController::StatusBusy, DmaController::StatusBusy);
	m.step(1);

	CHECK_EQ(dma.read(DmaController::StatusRegister) & DmaController::StatusDone, DmaController::StatusDone);
	CHECK_EQ(dma.read(DmaController::TransferredRegister), u32{ 5 });
	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 5), std::string{ "HELLO" });
}

TEST(dma, a_dma_transfer_past_the_end_of_memory_is_clamped_not_fatal)
{
	Machine m{ Instruction::NOP() };

	DmaController dma{};
	dma.attachTo(m.vm().io());

	// The source sits in the last four bytes of RAM, but 100 are asked for: the copy is clamped to 4.
	const u32 lastBytes = static_cast<u32>(m.memory().size()) - 4u;
	fill(m.memory(), lastBytes, "WXYZ");

	dma.write(DmaController::SourceRegister, lastBytes);
	dma.write(DmaController::DestinationRegister, DestinationBuffer);
	dma.write(DmaController::LengthRegister, 100);
	dma.write(DmaController::CommandRegister, DmaController::CommandStart);

	m.step(static_cast<usize>(DmaController::cyclesFor(100)) + 1);   // 13 cycles of nops, then the event

	CHECK_EQ(dma.read(DmaController::StatusRegister) & DmaController::StatusDone, DmaController::StatusDone);
	CHECK_EQ(dma.read(DmaController::TransferredRegister), u32{ 4 });
	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 4), std::string{ "WXYZ" });
}

TEST(dma, a_dma_transfer_moves_between_ram_and_vram)
{
	// RAM to VRAM and back (plan/v2 SPEC 5.7). The store into the VRAM marks its page, as a CPU store would, and a
	// transfer that runs past the end of the VRAM is clamped the way one past the end of the RAM is.
	Machine m{ Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP() };

	DmaController dma{};
	dma.attachTo(m.vm().io());
	fill(m.memory(), SourceBuffer, "PIXELS");

	auto transfer = [&](u32 from, u32 to, u32 length)
	{
		dma.write(DmaController::SourceRegister, from);
		dma.write(DmaController::DestinationRegister, to);
		dma.write(DmaController::LengthRegister, length);
		dma.write(DmaController::CommandRegister, DmaController::CommandStart);
		m.step(2);
	};

	const u32 vramAt = Vram::BaseValue + 3 * Vram::PageSize + 8;
	transfer(SourceBuffer, vramAt, 6);
	CHECK_EQ(dma.read(DmaController::TransferredRegister), u32{ 6 });
	CHECK(m.vm().vram().written(3));
	CHECK_EQ(m.vm().vram().writtenPages(), usize{ 1 });

	transfer(vramAt, DestinationBuffer, 6);
	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 6), std::string{ "PIXELS" });

	const u32 vramEnd = Vram::BaseValue + static_cast<u32>(m.vm().vram().size());
	transfer(SourceBuffer, vramEnd - 2, 6);
	CHECK_EQ(dma.read(DmaController::TransferredRegister), u32{ 2 });

	transfer(SourceBuffer, 0x90000000u, 6);   // the empty region: nothing moves
	CHECK_EQ(dma.read(DmaController::TransferredRegister), u32{ 0 });
}
