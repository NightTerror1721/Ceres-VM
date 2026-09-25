#pragma once

#include <ceres/vm/mmio_bus.h>

namespace ceres::devices
{
	using namespace vm;


	// A real DMA engine, not the pseudo-DMA the port opcodes used to be: SRC/DST/LEN/CMD are
	// ordinary registers, and completion is a tick later rather than instantaneous - modelled the
	// same way TimerDevice already models a delay, so a program can either poll STATUS or wait for
	// the interrupt. It moves memory the VM already knows how to move
	// (Memory::copyBytesUnchecked): RAM to RAM today, and RAM to or from a device's own MMIO window
	// once a device chooses to expose one, since both are just addresses in the same space.
	class DmaController : public IODevice
	{
	public:
		static inline constexpr Address SourceRegister = Address(0x00);      // Write: source physical address
		static inline constexpr Address DestinationRegister = Address(0x04); // Write: destination physical address
		static inline constexpr Address LengthRegister = Address(0x08);      // Write: bytes to move
		static inline constexpr Address CommandRegister = Address(0x0C);     // Write: 1 starts the transfer latched above
		static inline constexpr Address StatusRegister = Address(0x10);      // Read: Busy / Done bits
		static inline constexpr Address TransferredRegister = Address(0x14); // Read: bytes the last completed transfer actually moved

		static inline constexpr u32 CommandStart = 1;
		static inline constexpr u32 StatusBusy = 1u << 0;
		static inline constexpr u32 StatusDone = 1u << 1;

		// Third user interrupt: UserInterrupt0 is the timer's, UserInterrupt1 the terminal's.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt2;

	private:
		u32 _source = 0;
		u32 _destination = 0;
		u32 _length = 0;
		u32 _status = 0;
		u32 _transferred = 0;
		bool _pending = false;

	public:
		DmaController() = default;
		DmaController(const DmaController&) = delete;
		DmaController(DmaController&&) = delete;
		~DmaController() override = default;

		DmaController& operator=(const DmaController&) = delete;
		DmaController& operator=(DmaController&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Dma, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Dma);
		}

	public:
		// A transfer lands on the tick after it was armed.
		u64 ticksUntilEvent() const noexcept override { return _pending ? 1 : vm::NoDeviceEvent; }

		void advance(u64 ticks) override;

		// A reset drops a transfer that was armed but has not landed yet.
		void reset() override;

		// Must be unconditionally true, not "return _pending": MmioBus only re-reads needsTick() when
		// the topology changes (attach/detach), not every instruction, so a device that flipped this
		// on the fly could arm a transfer that then never sees the tick() that lands it.
		bool needsTick() const noexcept override { return true; }

		// Arms on the CMD write; the actual copy happens on the next tick(), one instruction later -
		// never on the same step that requested it, so a program relying on the interrupt (rather
		// than busy-polling STATUS) always sees a real handoff instead of an already-finished copy.
		void tick() override;

	public:
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
	};
}
