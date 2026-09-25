#pragma once

// What the device tests share: a machine loaded with a handful of instructions and the BIOS, stepped by hand,
// and the helpers to reach a device from such a program - build its base address in AT (r13), then name a
// register by its offset.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/vm/bios.h>
#include <ceres/devices/devices.h>
#include <ceres/core/format/memory_map.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;

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

			BIOS bios{};
			bios.initializeMemory(_vm.memory());
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		CeresVM& vm() noexcept { return _vm; }
		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
		Address pc() const { return _vm.engine().programCounter(); }
		Memory& memory() { return _vm.memory(); }

		// Installs a handler at a fixed spot and points an interrupt vector at it.
		void installHandler(InterruptNumber number, Address at, std::initializer_list<Instruction> handler)
		{
			usize offset = 0;
			for (Instruction instruction : handler)
			{
				_vm.memory().writeUnchecked<u32>(at + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(number) * Address::Size), at.value());
		}
	};

	[[maybe_unused]] constexpr u32 EntryPoint = Memory::UnrestrictedSegmentStart.value();

	// Where AT (r13) is loaded from, every time a test needs to reach a device: the two words - LUI then ORI - a
	// real program would spend to build the same 32-bit address.
	constexpr u8 Base = 13;
	constexpr u16 Hi(Address address) noexcept { return static_cast<u16>(address.value() >> 16); }
	constexpr u16 Lo(Address address) noexcept { return static_cast<u16>(address.value() & 0xFFFF); }
	[[maybe_unused]] inline Instruction LoadBase(Address address) noexcept { return Instruction::LUI(Base, Hi(address)); }
	[[maybe_unused]] inline Instruction LoadBaseLow(Address address) noexcept { return Instruction::ORI(Base, Base, Lo(address)); }
	[[maybe_unused]] inline u16 Off(Address registerOffset) noexcept { return static_cast<u16>(registerOffset.value()); }

	[[maybe_unused]] constexpr Address TimerBase = default_mmio::Timer;
	[[maybe_unused]] constexpr Address TerminalBase = default_mmio::Terminal;
	[[maybe_unused]] constexpr Address DiskBase = default_mmio::Disk;
	[[maybe_unused]] constexpr Address FramebufferBase = default_mmio::Framebuffer;

	// Two RAM buffers a test fills and reads back.
	[[maybe_unused]] constexpr u32 SourceBuffer = 0x1000;
	[[maybe_unused]] constexpr u32 DestinationBuffer = 0x2000;

	[[maybe_unused]] inline void fill(Memory& memory, u32 address, std::string_view bytes)
	{
		for (u32 i = 0; i < bytes.size(); ++i)
			memory.writeUnchecked<u8>(Address(address + i), static_cast<u8>(bytes[i]));
	}

	[[maybe_unused]] inline std::string readBack(Memory& memory, u32 address, u32 size)
	{
		std::string out;
		for (u32 i = 0; i < size; ++i)
			out.push_back(static_cast<char>(memory.readUnchecked<u8>(Address(address + i))));
		return out;
	}
}
