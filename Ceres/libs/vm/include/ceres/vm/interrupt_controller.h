#pragma once

#include <ceres/core/isa/interrupts.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace ceres::vm
{
	using namespace isa;

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

		// A halted machine sleeps on these until something raises a request (see waitForRaise). The
		// count moves on every raise, so a sleeper can tell "raised since I looked" from "still pending
		// from before" - a masked request stays pending and must not keep waking it.
		std::atomic<u64> _raises{ 0 };
		std::atomic<u32> _sleepers{ 0 };
#ifdef _WIN32
		// Windows: an event raise() sets and a high-resolution waitable timer, as HANDLEs, so this header
		// needs no <windows.h> (interrupt_controller.cpp says why not a condition variable).
		void* _wakeEvent = nullptr;
		void* _timer = nullptr;
#else
		std::mutex _sleepMutex;
		std::condition_variable _wake;
#endif

	public:
		InterruptController();
		InterruptController(const InterruptController&) = delete;
		InterruptController(InterruptController&&) = delete;
		~InterruptController();

		InterruptController& operator=(const InterruptController&) = delete;
		InterruptController& operator=(InterruptController&&) = delete;

	public:
		void raise(InterruptNumber interruptNumber) noexcept
		{
			_pending.fetch_or(bitOf(interruptNumber), std::memory_order_release);
			_raises.fetch_add(1);
			// Only a halted machine waits, so the lock is paid only when someone sleeps. The count
			// and the sleeper count are sequentially consistent, so either this raise sees the sleeper
			// or the sleeper sees the new count before it sleeps.
			if (_sleepers.load() != 0)
				wakeSleepers();
		}

		// How many requests have been raised so far. Read before deciding to sleep, and handed to
		// waitForRaise, so a raise that happens in between still wakes the sleeper.
		u64 raiseCount() const noexcept { return _raises.load(); }

		// Sleeps until a request is raised after raiseCount() returned `seen`, or until `deadline`, which
		// it keeps to about half a millisecond. True when it was woken by a raise.
		bool waitForRaise(u64 seen, std::chrono::steady_clock::time_point deadline) noexcept;

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

		// The whole pending set as one word, so a debugger restoring a snapshot can put back a
		// request that had been raised but not yet delivered. Nothing else should need these.
		u64 pendingMask() const noexcept { return _pending.load(std::memory_order_acquire); }
		void restorePendingMask(u64 mask) noexcept { _pending.store(mask, std::memory_order_release); }

	private:
		void wakeSleepers() noexcept;

		static constexpr u64 bitOf(InterruptNumber interruptNumber) noexcept
		{
			return u64{ 1 } << static_cast<u8>(interruptNumber);
		}
	};
}
