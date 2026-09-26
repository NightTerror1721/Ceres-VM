#pragma once

#include <ceres/core/base/types.h>
#include <array>

// The machine's event scheduler (plan/v2 SPEC 3.3): a device that will act on its own at some moment - a timer
// running out, an alarm coming due - asks to be called then, in CPU cycles, instead of being called every
// instruction to count down. The CPU loop looks at one number per instruction, nextCycle(), and only when the
// clock reaches it does anything else happen.
namespace ceres::vm
{
	class IODevice;

	// "Nothing scheduled", for Scheduler::nextCycle().
	inline constexpr u64 NoScheduledEvent = ~u64{ 0 };

	class Scheduler
	{
	public:
		// A few devices with an event or two each: a flat array scanned on change beats a heap at this size.
		static inline constexpr usize MaxEvents = 64;

	private:
		struct Event
		{
			IODevice* device = nullptr;
			u64 cycle = 0;
			u32 tag = 0;
		};

		std::array<Event, MaxEvents> _events{};
		usize _count = 0;
		u64 _ownNext = NoScheduledEvent;
		// The earliest cycle in _events, kept where the CPU loop reads it: beside the engine's own counter once
		// the engine is built (bindTo), so the check it makes every instruction touches nothing else.
		u64* _next = &_ownNext;
		const u64* _clock = nullptr;            // the CPU's cycle counter (ExecutionEngine), what now() reads

		void recomputeNext() noexcept;

	public:
		Scheduler() = default;
		Scheduler(const Scheduler&) = delete;
		Scheduler(Scheduler&&) = delete;
		Scheduler& operator=(const Scheduler&) = delete;
		Scheduler& operator=(Scheduler&&) = delete;

	public:
		// The engine points the scheduler at its counter, and gives it the word to keep the next event's cycle in,
		// when it is built; until then the clock reads 0.
		void bindTo(const u64* cycles, u64* next) noexcept
		{
			*next = *_next;
			_next = next;
			_clock = cycles;
		}
		u64 now() const noexcept { return _clock != nullptr ? *_clock : 0; }

		// Calls `device.onEvent(tag, cycle)` once the clock reaches `cycle` (at once, at the next look, if it
		// already has). A device has at most one event per tag: scheduling a tag again moves it.
		void schedule(IODevice& device, u64 cycle, u32 tag) noexcept;
		// Drops the device's event with that tag, if it has one.
		void cancel(IODevice& device, u32 tag) noexcept;
		// Drops every event of the device: it is being detached.
		void cancelAll(IODevice& device) noexcept;
		// When the device's event with that tag is due, or NoScheduledEvent.
		u64 cycleOf(const IODevice& device, u32 tag) const noexcept;

		u64 nextCycle() const noexcept { return *_next; }
		bool empty() const noexcept { return _count == 0; }

		// Calls every event due at or before `now`, earliest first. A device may schedule again from its
		// onEvent - a periodic timer does - and an event it schedules at or before `now` runs in this call.
		void service(u64 now);
	};
}
