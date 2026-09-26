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
