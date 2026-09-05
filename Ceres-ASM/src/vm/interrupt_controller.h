#pragma once

#include "common/types.h"
#include "interrupts.h"
#include <atomic>
#include <optional>

namespace ceres::vm
{
	// Lets a device ask for an interrupt.
	//
	// Devices used to be entirely passive: the machine read from them and wrote to them, and they
	// could never speak first. That is why HALT suspended the machine for good — there was no
	// asynchronous source able to wake it.
	//
	// Pending requests are a 64-bit mask, one bit per interrupt number, so a request raised twice
	// before it is delivered is delivered once. Atomic because a device may be driven from another
	// thread; the terminal already keeps its input buffer that way.
	class InterruptController
	{
	private:
		std::atomic<u64> _pending{ 0 };

	public:
		InterruptController() = default;
		InterruptController(const InterruptController&) = delete;
		InterruptController(InterruptController&&) = delete;
		~InterruptController() = default;

		InterruptController& operator=(const InterruptController&) = delete;
		InterruptController& operator=(InterruptController&&) = delete;

	public:
		void raise(InterruptNumber interruptNumber) noexcept
		{
			_pending.fetch_or(bitOf(interruptNumber), std::memory_order_release);
		}

		bool hasPending() const noexcept
		{
			return _pending.load(std::memory_order_acquire) != 0;
		}

		// Lowest-numbered pending request, left pending. The engine peeks first because a masked
		// interrupt has to stay queued until the program enables interrupts.
		std::optional<InterruptNumber> peek() const noexcept
		{
			const u64 pending = _pending.load(std::memory_order_acquire);
			if (pending == 0)
				return std::nullopt;

			for (u8 number = 0; number < InterruptNumberCount; ++number)
			{
				if ((pending & (u64{ 1 } << number)) != 0)
					return static_cast<InterruptNumber>(number);
			}
			return std::nullopt;
		}

		void clear(InterruptNumber interruptNumber) noexcept
		{
			_pending.fetch_and(~bitOf(interruptNumber), std::memory_order_release);
		}

		void clearAll() noexcept { _pending.store(0, std::memory_order_release); }

	private:
		static constexpr u64 bitOf(InterruptNumber interruptNumber) noexcept
		{
			return u64{ 1 } << static_cast<u8>(interruptNumber);
		}
	};
}
