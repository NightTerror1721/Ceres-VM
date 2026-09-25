#include <ceres/devices/system/timer.h>

namespace ceres::devices
{
	void TimerDevice::arm(u64 ticksFromNow, bool periodic) noexcept
	{
		_remaining = ticksFromNow;
		_periodic = periodic;
		_period = ticksFromNow;
	}

	void TimerDevice::reset()
	{
		_ticks = 0;
		_remaining = 0;
		_periodic = false;
		_period = 0;
		_nanosHigh = 0;                                   // nothing latched yet, as at power-on
		_ticksHigh = 0;
		_alarmNanos = 0;
		_alarmLow = 0;
		_alarmPoll = 0;
		_alarmTick = 0;
		_started = std::chrono::steady_clock::now();      // "since the machine started" starts again
	}

	void TimerDevice::restoreState(const State& state) noexcept
	{
		_ticks = state.ticks;
		_remaining = state.remaining;
		_periodic = state.periodic;
		_period = state.period;
		_nanosHigh = state.nanosHigh;
		_ticksHigh = state.ticksHigh;
		_alarmNanos = state.alarmNanos;
		_alarmLow = state.alarmLow;
		_alarmTick = _ticks;                              // its tick is worked out again at the next look
	}

	void TimerDevice::syncAlarm()
	{
		if (_alarmNanos == 0)
			return;
		const u64 now = liveNanos();
		if (now >= _alarmNanos)
		{
			_alarmNanos = 0;
			raiseInterrupt(AlarmInterrupt);
			return;
		}
		const u64 left = _alarmNanos - now;
		const u64 hz = _haltClockHz;
		const u64 ticks = (left / 1'000'000'000ull) * hz + ((left % 1'000'000'000ull) * hz + 999'999'999ull) / 1'000'000'000ull;
		_alarmTick = _ticks + (ticks == 0 ? 1 : ticks);
	}

	u32 TimerDevice::measureNanosResolution() noexcept
	{
		using Clock = std::chrono::steady_clock;
		u64 least = ~u64{ 0 };
		auto last = Clock::now();
		for (int i = 0; i < 20000; ++i)
		{
			const auto now = Clock::now();
			const u64 step = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
			if (step != 0 && step < least)
				least = step;
			last = now;
		}
		if (least == ~u64{ 0 })
			return 1000000;   // it never moved: nothing better than a millisecond can be claimed
		return least > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(least);
	}

	void TimerDevice::tick()
	{
		++_ticks;

		if (_alarmNanos != 0 && ++_alarmPoll >= AlarmPollTicks)
		{
			_alarmPoll = 0;
			syncAlarm();
		}

		if (_remaining == 0)
			return;

		if (--_remaining == 0)
		{
			raiseInterrupt(Interrupt);
			if (_periodic)
				_remaining = _period;
		}
	}

	u64 TimerDevice::ticksUntilEvent() const noexcept
	{
		u64 nearest = _remaining != 0 ? _remaining : vm::NoDeviceEvent;
		if (_alarmNanos != 0 && _haltClockHz != 0)
		{
			const u64 alarm = _alarmTick > _ticks ? _alarmTick - _ticks : 1;
			if (alarm < nearest)
				nearest = alarm;
		}
		return nearest;
	}

	void TimerDevice::advance(u64 ticks)
	{
		_ticks += ticks;
		syncAlarm();                   // a halted machine's time passed: the alarm's instant may have come
		if (_remaining == 0)
			return;
		if (ticks < _remaining)
		{
			_remaining -= ticks;
			return;
		}
		raiseInterrupt(Interrupt);
		_remaining = _periodic ? _period : 0;
	}

	u32 TimerDevice::readUnsignedWord(Address offset)
	{
		// The tick count is 64 bits and read as two words, like the nanosecond one: the low read takes
		// the count and keeps its high half, so the pair is one moment however many ticks pass between
		// the two reads. A 32-bit count wraps in 43 s at the usual rate.
		if (offset == TicksRegister)
		{
			const u64 ticks = _ticks;
			_ticksHigh = static_cast<u32>(ticks >> 32);
			return static_cast<u32>(ticks);
		}

		if (offset == TicksHighRegister)
			return _ticksHigh;

		if (offset == HaltClockRegister)
			return _haltClockHz;

		if (offset == ClockRegister)
		{
			if (_clockSource)
				return _clockSource();
			return static_cast<u32>(std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
		}

		if (offset == MillisRegister)
		{
			if (_millisSource)
				return _millisSource();
			return static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - _started).count());
		}

		if (offset == NanosLowRegister)
		{
			const u64 now = _nanosSource
				? _nanosSource()
				: static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - _started).count());
			_nanosHigh = static_cast<u32>(now >> 32);
			return static_cast<u32>(now);
		}

		if (offset == NanosHighRegister)
			return _nanosHigh;

		// The armed instant, 0:0 when disarmed - so a program can tell the two apart, even for an
		// instant in the first 4.29 s. A low word written and not yet armed does not show.
		if (offset == AlarmLowRegister)
			return static_cast<u32>(_alarmNanos);

		if (offset == AlarmHighRegister)
			return static_cast<u32>(_alarmNanos >> 32);

		if (offset == NanosResolutionRegister)
		{
			if (_nanosResolution == 0)
				_nanosResolution = measureNanosResolution();
			return _nanosResolution;
		}

		return 0xFFFFFFFF;
	}

	void TimerDevice::writeWord(Address offset, u32 value)
	{
		// The alarm: an absolute instant on the nanosecond clock, written low word first - the high
		// word is what arms it, so the two halves are one instant. When the instant comes the alarm
		// raises AlarmInterrupt once and disarms; one already past fires at once. 0:0 disarms it.
		if (offset == AlarmLowRegister)
		{
			_alarmLow = value;
			return;
		}
		if (offset == AlarmHighRegister)
		{
			_alarmNanos = (static_cast<u64>(value) << 32) | _alarmLow;
			_alarmLow = 0;                     // spent: a later write of the high word alone cannot reuse it
			_alarmPoll = 0;
			syncAlarm();                       // one already past fires now
			return;
		}

		if (offset != CommandRegister)
			return;

		const bool periodic = (value & 0x80000000u) != 0;
		const u64 count = value & 0x7FFFFFFFu;

		arm(count, periodic && count > 0);
	}
}
