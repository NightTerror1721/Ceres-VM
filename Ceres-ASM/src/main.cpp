#include <iostream>
#include "vm/ceresvm.h"
#include "vm/devices.h"

int main()
{
	using namespace ceres;
	using namespace ceres::vm;

	CeresVM vm{};

	SystemControlDevice systemControlDevice{
		[&vm]() { vm.shutdown(); }, // Shutdown callback
		[&vm]() { vm.shutdown(); }  // Reset callback (for simplicity, we just shut down the VM on reset as well)
	};
	systemControlDevice.attachTo(vm.io());

	TerminalDevice terminalDevice{};
	terminalDevice.attachTo(vm.io());

	// For demonstration purposes, we'll create a simple program in memory that writes "Hello, World!" to the terminal and then halts.

	constexpr u8 OutPort = default_ports::TERM_OUT;
	std::vector<Instruction> programInstructions = {
		Instruction::MOVI(0, 'H'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'e'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'l'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'l'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'o'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, ','), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, ' '), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'W'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'o'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'r'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'l'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 'd'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, '!'), Instruction::OUT(0, OutPort),
		Instruction::MOVI(0, 0x01), Instruction::OUT(0, default_ports::SYS_CONTROL) // Send shutdown command to system control port to halt the VM.
	};

	std::span<const u8> programBytes = Instruction::asBytes(programInstructions);

	ProgramHeader header{};
	header.magic = ProgramHeader::MagicNumber;
	header.version = ProgramHeader::CurrentVersion;
	header.flags = 0;
	header.entryPoint = Memory::UnrestrictedSegmentStart.value(); // Entry point at the start of the unrestricted segment
	header.textSize = static_cast<u32>(programBytes.size());
	header.rodataSize = 0;
	header.dataSize = 0;
	header.bssSize = 0;
	header.minimumStack = 1024; // Minimum stack size of 1 KiB

	Program program = Program::make(header, programBytes, {}, {});

	if (auto result = vm.loadProgram(program); !result)
	{
		std::cerr << "Failed to load program: " << result.error() << std::endl;
		return 1;
	}

	if (auto result = vm.run(); !result)
	{
		std::cerr << "Failed to run program: " << result.error() << std::endl;
		return 1;
	}

}
