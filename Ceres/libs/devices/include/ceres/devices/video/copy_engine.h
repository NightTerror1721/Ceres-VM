#pragma once

// The GPU's copy engine (plan/v2 SPEC 7.1 V1, 7.3 0x280-0x2BF): copies a run of bytes, or fills one with a 32-bit
// pattern, anywhere in RAM or VRAM, while the CPU goes on. It costs GPU cycles (cost_model.h): the bytes change when
// the work is done, on the CPU cycle the GPU's time comes to, and then Status clears busy and the GPU raises
// interrupt 34 if IrqEnable bit 2 is set. A run that is not wholly in RAM or wholly in VRAM is a fault: nothing moves,
// Status bit 1 is set and the GPU reports it (FaultCode 1, FaultAddress, interrupt 35).

#include <ceres/core/base/types.h>
#include <ceres/vm/memory.h>
#include <ceres/vm/vram.h>

#include <span>

namespace ceres::devices::video
{
	class CopyEngine
	{
	public:
		static inline constexpr u32 SrcRegister = 0x280;        // RW: where a copy reads
		static inline constexpr u32 DstRegister = 0x284;        // RW: where it writes
		static inline constexpr u32 LengthRegister = 0x288;     // RW: bytes
		static inline constexpr u32 FillValueRegister = 0x28C;  // RW: the pattern a fill repeats, little-endian
		static inline constexpr u32 CommandRegister = 0x290;    // W: 1 copy, 2 fill
		static inline constexpr u32 StatusRegister = 0x294;     // R: bit 0 busy, bit 1 the last command faulted
		static inline constexpr u32 FirstRegister = 0x280;
		static inline constexpr u32 LastRegister = 0x2BC;

		static inline constexpr u32 CommandCopy = 1;
		static inline constexpr u32 CommandFill = 2;
		static inline constexpr u32 StatusBusy = 1u << 0;
		static inline constexpr u32 StatusError = 1u << 1;

		// What a command asks of the GPU: how long it takes, or the address that makes it fault.
		struct Start
		{
			bool started = false;
			u64 gpuCycles = 0;
			bool fault = false;
			u32 faultAddress = 0;
		};

	private:
		u32 _src = 0, _dst = 0, _length = 0, _fill = 0;
		u32 _status = 0;
		u32 _command = 0;   // the one running

	public:
		void reset() noexcept { *this = CopyEngine{}; }

		static constexpr bool handles(u32 offset) noexcept { return offset >= FirstRegister && offset <= LastRegister; }
		u32 read(u32 offset) const noexcept;
		// A write; a command is checked against the RAM and the VRAM and, when it can run, started.
		Start write(u32 offset, u32 value, vm::Memory& memory, const vm::Vram& vram) noexcept;
		// The GPU's time for the running command has come: do it.
		void finish(vm::Memory& memory, vm::Vram& vram) noexcept;

		bool busy() const noexcept { return (_status & StatusBusy) != 0; }
	};
}
