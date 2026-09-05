#pragma once

#include "execution_engine.h"
#include "interrupt_controller.h"
#include "bios.h"
#include "program.h"
#include <expected>
#include <string>

namespace ceres::vm
{
	class CeresVM
	{
	private:
		Memory _memory;
		InterruptController _interrupts;
		IOPorts _ioPorts;
		BIOS _bios;
		ExecutionEngine _engine;

		bool _isPoweredOn = false;

	public:
		explicit CeresVM(usize memorySize = Memory::DefaultSize) :
			_memory(memorySize),
			_ioPorts(_memory, _interrupts),
			_engine(_memory, _ioPorts, _interrupts)
		{}

		CeresVM(const CeresVM&) = delete;
		CeresVM(CeresVM&&) = delete;
		~CeresVM() = default;

		CeresVM& operator=(const CeresVM&) = delete;
		CeresVM& operator=(CeresVM&&) = delete;

	public:
		std::expected<void, std::string> loadProgram(const Program& program) noexcept;

		std::expected<void, std::string> run() noexcept;

	public:
		constexpr bool isPoweredOn() const noexcept { return _isPoweredOn; }
		constexpr void shutdown() noexcept { _isPoweredOn = false; }

		IOPorts& io() noexcept { return _ioPorts; }
		InterruptController& interrupts() noexcept { return _interrupts; }

		// Exposed so a test or a debugger can set up and inspect machine state directly,
		// without going through a full Program.
		Memory& memory() noexcept { return _memory; }
		const Memory& memory() const noexcept { return _memory; }

		ExecutionEngine& engine() noexcept { return _engine; }
		const ExecutionEngine& engine() const noexcept { return _engine; }
	};
}
