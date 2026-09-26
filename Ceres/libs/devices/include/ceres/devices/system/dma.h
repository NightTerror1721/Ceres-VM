#pragma once

#include <ceres/vm/mmio_bus.h>

namespace ceres::devices
{
	using namespace vm;


	// A real DMA engine, not the pseudo-DMA the port opcodes used to be: SRC/DST/LEN/CMD are
	// ordinary registers, and a transfer takes time - one cycle for every 8 bytes (plan/v2 SPEC 5.7) - and
	// lands on its own event on the machine's scheduler, as the timer's countdown does, so a program can
	// either poll STATUS or wait for the interrupt. It moves memory the VM already knows how to move
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
		// How many cycles a transfer of `length` bytes takes: one for every 8, and at least one, so it never
		// lands on the instruction that started it.
		static constexpr u64 cyclesFor(u32 length) noexcept { return length == 0 ? 1 : (u64{ length } + 7) / 8; }

		// Whether a transfer has been started and has not landed yet.
		bool isPending() const noexcept { return _pending; }

		// A reset drops a transfer that was armed but has not landed yet.
		void reset() override;

		// The transfer's event: the copy happens here, all at once, and the interrupt is raised.
		void onEvent(u32 tag, u64 cycle) override;

	public:
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;
	};
}
