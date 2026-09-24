#pragma once

#include "execution_engine.h"
#include "interrupt_controller.h"
#include "bios.h"
#include <ceres/core/format/program.h>
#include <atomic>
#include <optional>
#include <expected>
#include <string>

namespace ceres::vm
{
	using namespace fmt;

	class CeresVM
	{
	private:
		Memory _memory;
		InterruptController _interrupts;
		MmioBus _mmioBus;
		BIOS _bios;
		ExecutionEngine _engine;

		// Atomic because it is the machine's only stop signal, and the thread that sets it is not
		// always the thread running the step loop: a debugger's "pause" arrives from whatever
		// thread is servicing the editor while run() is still spinning.
		std::atomic<bool> _isPoweredOn{ false };

		// A reset asked for by the program (the system control device's command 2). It cannot happen
		// in the middle of the store that asks for it, so it stops the step loop like a shutdown and
		// is carried out between two instructions, by run() or by a host that drives step() itself.
		std::atomic<bool> _resetRequested{ false };

		// What loadProgram() placed, kept so a reset can put the image back as it was loaded.
		std::optional<Program> _program;

	public:
		explicit CeresVM(usize memorySize = Memory::DefaultSize) :
			_memory(memorySize),
			_mmioBus(_memory, _interrupts),
			_engine(_memory, _mmioBus, _interrupts)
		{}

		CeresVM(const CeresVM&) = delete;
		CeresVM(CeresVM&&) = delete;
		~CeresVM() = default;

		CeresVM& operator=(const CeresVM&) = delete;
		CeresVM& operator=(CeresVM&&) = delete;

	public:
		std::expected<void, std::string> loadProgram(const Program& program) noexcept;

		std::expected<void, std::string> run() noexcept;

		// Powers the machine on without running the step loop, for a caller that drives step()
		// itself — a debugger, which has to decide between one instruction and the next. Without
		// this, shutdown() would have nothing to switch off and a program writing to the system
		// control port would look like it had done nothing at all.
		std::expected<void, std::string> powerOn() noexcept;

	public:
		// This is only a stop signal; it does not publish any associated state between threads.
		bool isPoweredOn() const noexcept { return _isPoweredOn.load(std::memory_order_relaxed); }
		void shutdown() noexcept { _isPoweredOn.store(false, std::memory_order_relaxed); }

		// Stops the step loop like shutdown(), and asks for the machine to start over once it has.
		void requestReset() noexcept
		{
			_resetRequested.store(true, std::memory_order_relaxed);
			// Release: a step loop that sees the power-off also sees the request (restartIfRequested).
			_isPoweredOn.store(false, std::memory_order_release);
		}

		bool isResetRequested() const noexcept { return _resetRequested.load(std::memory_order_relaxed); }

		// Carries out a requested reset: the loaded image is put back (.data as it was loaded, .bss
		// cleared, the vectors bound again), the CPU starts from the reset vector, pending interrupts
		// are dropped, every device's reset() runs, and the machine is powered on again. True when it
		// restarted; false when no reset was asked for (or there is no program to restart).
		bool restartIfRequested() noexcept;

		MmioBus& io() noexcept { return _mmioBus; }
		InterruptController& interrupts() noexcept { return _interrupts; }

		// Exposed so a test or a debugger can set up and inspect machine state directly,
		// without going through a full Program.
		Memory& memory() noexcept { return _memory; }
		const Memory& memory() const noexcept { return _memory; }

		ExecutionEngine& engine() noexcept { return _engine; }
		const ExecutionEngine& engine() const noexcept { return _engine; }
	};
}
