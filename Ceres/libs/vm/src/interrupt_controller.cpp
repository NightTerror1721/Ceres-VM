#include <ceres/vm/interrupt_controller.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ceres::vm
{
#ifdef _WIN32
	// A condition variable, and every other wait the C++ library offers here, sleeps in steps of the
	// scheduler's tick - 15.6 ms, and timeBeginPeriod does not change that for them - so a halted machine
	// asleep until an event 2 ms away woke 15 ms later. A high-resolution waitable timer (Windows 10 1803
	// and later) keeps to about half a millisecond, and waiting on it together with an event that raise()
	// sets still wakes the machine the moment a device asks.
	InterruptController::InterruptController()
	{
		_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset: one wake per SetEvent
		_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (_timer == nullptr)
			_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);   // an older Windows: to the scheduler's tick
	}

	InterruptController::~InterruptController()
	{
		if (_timer != nullptr)
			CloseHandle(_timer);
		if (_wakeEvent != nullptr)
			CloseHandle(_wakeEvent);
	}

	void InterruptController::wakeSleepers() noexcept
	{
		if (_wakeEvent != nullptr)
			SetEvent(_wakeEvent);
	}

	bool InterruptController::waitForRaise(u64 seen, std::chrono::steady_clock::time_point deadline) noexcept
	{
		// Registered before the count is looked at, and raise() counts before it looks for sleepers: either
		// the raise sees this sleeper and sets the event, or this sees the raise. An event set for a raise
		// already seen only makes the next wait return once for nothing, and the loop waits again.
		_sleepers.fetch_add(1);
		const u64 pokes = _pokes.load();
		bool raised = _raises.load() != seen;
		while (!raised && _pokes.load() == pokes)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				break;
			const long long left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
			// Without the event (CreateEventW failed) a raise cannot interrupt the wait: it waits a
			// millisecond at a time instead, and the loop looks at the count between them.
			const long long step = _wakeEvent != nullptr ? left : (left < 1'000'000 ? left : 1'000'000);
			HANDLE handles[2] = { _timer, _wakeEvent };
			LARGE_INTEGER due;
			due.QuadPart = -(step / 100 > 0 ? step / 100 : 1);   // relative, in 100 ns units
			if (_timer != nullptr && SetWaitableTimer(_timer, &due, 0, nullptr, nullptr, FALSE))
				WaitForMultipleObjects(_wakeEvent != nullptr ? 2 : 1, handles, FALSE, INFINITE);
			else if (_wakeEvent != nullptr)
				WaitForSingleObject(_wakeEvent, static_cast<DWORD>(step / 1'000'000 + 1));
			else
				Sleep(1);
			raised = _raises.load() != seen;
		}
		_sleepers.fetch_sub(1);
		return raised;
	}
#else
	InterruptController::InterruptController() = default;
	InterruptController::~InterruptController() = default;

	void InterruptController::wakeSleepers() noexcept
	{
		const std::lock_guard lock{ _sleepMutex };
		_wake.notify_all();
	}

	bool InterruptController::waitForRaise(u64 seen, std::chrono::steady_clock::time_point deadline) noexcept
	{
		std::unique_lock lock{ _sleepMutex };
		_sleepers.fetch_add(1);
		const u64 pokes = _pokes.load();
		_wake.wait_until(lock, deadline, [&] { return _raises.load() != seen || _pokes.load() != pokes; });
		_sleepers.fetch_sub(1);
		return _raises.load() != seen;
	}
#endif
}
