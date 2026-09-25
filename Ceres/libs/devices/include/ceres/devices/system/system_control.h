#pragma once

// The system control device: shutdown, reset, the exit status and what the machine knows about itself.

#include <ceres/vm/mmio_bus.h>
#include <atomic>
#include <functional>

namespace ceres::devices
{
	using namespace vm;

	class SystemControlDevice : public IODevice
	{
	public:
		using ShutdownCallback = std::function<void()>;
		using ResetCallback = std::function<void()>;
		// Told the new value of the features register whenever a program writes it, so the host can
		// pass on to the engine the settings the engine itself has to act on.
		using FeaturesCallback = std::function<void(u32)>;
		// How the device reaches the engine's stack limit, which it does not own: the host passes the
		// engine's stackLimit() and setProgramStackLimit().
		using StackLimitGetter = std::function<u32()>;
		using StackLimitSetter = std::function<void(u32)>;
		// The last memory fault's data address and access, read from the engine (faultAddress/faultAccess).
		using FaultInfoGetter = std::function<u32()>;
		// Where the loader put the program's arguments (CeresVM::argumentBlock): 0 argc, 1 argv, 2 envp.
		using ArgumentInfoGetter = std::function<u32(u32)>;

		// Write-only: writing specific commands to this register triggers system control actions.
		// The low byte is the command; the next byte is the exit status a shutdown reports.
		static inline constexpr Address CommandRegister = Address(0x00);
		// Read-only: how many bytes of RAM the machine has.
		static inline constexpr Address MemorySizeRegister = Address(0x04);
		// Read/write: switches for behaviour that is off by default, so a program that never asks
		// keeps the machine it always had.
		static inline constexpr Address FeaturesRegister = Address(0x08);
		// Read/write: the lowest address the program's stack may reach. A push below it raises
		// StackOverflow. It starts at the end of the loaded image; a program raises it over its heap as
		// the heap grows, and cannot lower it below the image. All-ones when the host connected no engine.
		static inline constexpr Address StackLimitRegister = Address(0x0C);
		// Read-only: the data address of the last memory fault (AlignmentFault, MemoryFault, PageFault),
		// and its access - 1 read, 2 write, 3 instruction fetch in bits 0-7, the size in bytes in 8-31 (a block
		// instruction's chunk runs to a page) -
		// so a fault handler can say "store word to 0x00000801" and not only where the instruction was.
		static inline constexpr Address FaultAddressRegister = Address(0x10);
		static inline constexpr Address FaultAccessRegister = Address(0x14);
		// Read-only: what main() was started with - argc, and the addresses of the null-terminated argv and envp
		// arrays the loader placed at the top of the program's stack - so a library reaches the environment
		// (getenv) without main passing it on. 0, 0, 0 when the host connected no machine to it.
		static inline constexpr Address ArgumentCountRegister = Address(0x18);
		static inline constexpr Address ArgumentVectorRegister = Address(0x1C);
		static inline constexpr Address EnvironmentRegister = Address(0x20);
		// Read-only: why the last memory fault happened - a vm::FaultReason (plan/v2 SPEC 5.4): 5 for a device
		// register reached by anything but an aligned 32-bit access, 6 for a block instruction that touched a device.
		static inline constexpr Address FaultReasonRegister = Address(0x2C);

		static inline constexpr u32 CommandShutdown = 0x01;
		static inline constexpr u32 CommandReset = 0x02;

		// Division by zero raises the DivisionByZero interrupt (number 4) instead of only setting
		// the Trap flag. The handler returns to the instruction after the division, whose
		// destination was left as it was.
		static inline constexpr u32 FeatureDivisionFault = 1u << 0;
		// A float division by zero gives what IEEE 754 says instead of trapping: fdiv and frecipe +-infinity
		// (NaN for 0/0), fmod NaN, frsqrte of a zero +-infinity. The integer divisions are not changed.
		static inline constexpr u32 FeatureIeeeDivide = 1u << 1;

	private:
		ShutdownCallback _shutdownCallback;
		ResetCallback _resetCallback;
		FeaturesCallback _featuresCallback;
		StackLimitGetter _stackLimitGetter;
		StackLimitSetter _stackLimitSetter;
		FaultInfoGetter _faultAddressGetter;
		FaultInfoGetter _faultAccessGetter;
		FaultInfoGetter _faultReasonGetter;
		ArgumentInfoGetter _argumentGetter;
		u32 _features = 0;
		std::atomic<u8> _exitCode{ 0 };

	public:
		explicit SystemControlDevice(ShutdownCallback shutdownCallback = {}, ResetCallback resetCallback = {}) :
			_shutdownCallback(std::move(shutdownCallback)),
			_resetCallback(std::move(resetCallback))
		{}

		SystemControlDevice(const SystemControlDevice&) = delete;
		SystemControlDevice(SystemControlDevice&&) = delete;
		~SystemControlDevice() override = default;

		SystemControlDevice& operator=(const SystemControlDevice&) = delete;
		SystemControlDevice& operator=(SystemControlDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::SystemControl, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::SystemControl);
		}

		void setShutdownCallback(ShutdownCallback callback)
		{
			_shutdownCallback = std::move(callback);
		}

		void setResetCallback(ResetCallback callback)
		{
			_resetCallback = std::move(callback);
		}

		// A reset switches every feature off again, the way the machine powers on.
		void reset() override;

		void setFeaturesCallback(FeaturesCallback callback)
		{
			_featuresCallback = std::move(callback);
		}

		void setStackLimitHandlers(StackLimitGetter getter, StackLimitSetter setter);

		void setFaultInfoHandlers(FaultInfoGetter address, FaultInfoGetter access, FaultInfoGetter reason);

		void setArgumentHandler(ArgumentInfoGetter getter)
		{
			_argumentGetter = std::move(getter);
		}

		// The status the program shut the machine down with: the second byte of the word it wrote
		// (0 for a plain byte write, which is what every program written before this existed does).
		u8 exitCode() const noexcept { return _exitCode.load(std::memory_order_relaxed); }

		u32 features() const noexcept { return _features; }

	private:
		// A byte write carries only the command; a halfword or a word carries the exit status
		// above it.
		void command(u32 value);

		bool readable(Address offset, u32& value) const;

	public:
		u32 read(Address offset) override { u32 v; return readable(offset, v) ? v : 0xFFFFFFFF; }
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;
	};
}
