#include <ceres/devices/system/timer.h>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Ticks",           RegisterAccess::Read,      0x0, true,  "The low word of the CPU cycles so far; also latches the high word." },
			{ 0x04, "Clock",           RegisterAccess::Read,      0x0, false, "Seconds since the epoch." },
			{ 0x08, "Command",         RegisterAccess::Write,     0x0, false, "Arms the timer to fire after that many cycles; 0 disarms it." },
			{ 0x0C, "Millis",          RegisterAccess::Read,      0x0, false, "Milliseconds since the machine started (wraps every 49 days)" },
			{ 0x10, "NanosLow",        RegisterAccess::Read,      0x0, true,  "The low word of the nanoseconds since the machine started; also latches the high word." },
			{ 0x14, "NanosHigh",       RegisterAccess::Read,      0x0, false, "The high word latched by the last read of NanosLowRegister." },
			{ 0x18, "NanosResolution", RegisterAccess::Read,      0x0, false, "The smallest step the nanosecond clock is seen to take, in nanoseconds." },
			{ 0x1C, "HaltClock",       RegisterAccess::Read,      0x0, false, "Cycles per second while the CPU is halted; 0 when time only moves by events." },
			{ 0x20, "AlarmLow",        RegisterAccess::ReadWrite, 0x0, false, "The low word of the alarm instant, in nanoseconds on NanosLow's clock (reads 0 when disarmed)" },
			{ 0x24, "AlarmHigh",       RegisterAccess::ReadWrite, 0x0, false, "The high word; writing it arms the alarm at high:low (0:0 disarms)" },
			{ 0x28, "TicksHigh",       RegisterAccess::Read,      0x0, false, "The high word of the cycle count latched by the last read of TicksRegister." },
		};
	}

	u64 TimerDevice::now() const noexcept
	{
		const Scheduler* clock = scheduler();
		return clock != nullptr ? clock->now() : 0;
	}

	u64 TimerDevice::ticks() const noexcept
	{
		return now();
	}

	void TimerDevice::arm(u64 cyclesFromNow, bool periodic) noexcept
	{
		_periodic = periodic && cyclesFromNow > 0;
		_period = cyclesFromNow;
		_deadline = cyclesFromNow > 0 ? now() + cyclesFromNow : 0;
		if (Scheduler* events = scheduler())
		{
			if (_deadline != 0)
				events->schedule(*this, _deadline, CountdownEvent);
			else
				events->cancel(*this, CountdownEvent);
		}
	}

	TimerDevice::State TimerDevice::captureState() const noexcept
	{
		const u64 clock = now();
		const u64 remaining = _deadline == 0 ? 0 : _deadline > clock ? _deadline - clock : 1;
		return State{ clock, remaining, _periodic, _period, _nanosHigh, _ticksHigh, _alarmNanos, _alarmLow };
	}

	void TimerDevice::reset()
	{
		_deadline = 0;
		_periodic = false;
		_period = 0;
		_nanosHigh = 0;                                   // nothing latched yet, as at power-on
		_ticksHigh = 0;
		_alarmNanos = 0;
		_alarmLow = 0;
		if (Scheduler* events = scheduler())
			events->cancelAll(*this);
		_started = std::chrono::steady_clock::now();      // "since the machine started" starts again
	}

	void TimerDevice::restoreState(const State& state) noexcept
	{
		_deadline = 0;
		arm(state.remaining, false);
		_periodic = state.periodic;
		_period = state.period;
		_nanosHigh = state.nanosHigh;
		_ticksHigh = state.ticksHigh;
		_alarmNanos = state.alarmNanos;
		_alarmLow = state.alarmLow;
		syncAlarm();                                      // its cycle is worked out again from the clock
	}

	void TimerDevice::syncAlarm()
	{
		Scheduler* events = scheduler();
		if (_alarmNanos == 0)
		{
			if (events != nullptr)
				events->cancel(*this, AlarmEvent);
			return;
		}
		const u64 now = liveNanos();
		if (now >= _alarmNanos)
		{
			_alarmNanos = 0;
			if (events != nullptr)
				events->cancel(*this, AlarmEvent);
			raiseInterrupt(AlarmInterrupt);
			return;
		}
		if (events == nullptr)
			return;
		const u64 left = _alarmNanos - now;
		const u64 hz = _haltClockHz;
		u64 cycles = AlarmPollCycles;
		if (hz != 0)
			cycles = (left / 1'000'000'000ull) * hz + ((left % 1'000'000'000ull) * hz + 999'999'999ull) / 1'000'000'000ull;
		events->schedule(*this, events->now() + (cycles == 0 ? 1 : cycles), AlarmEvent);
	}

	void TimerDevice::onEvent(u32 tag, u64 cycle)
	{
		if (tag == AlarmEvent)
		{
			syncAlarm();
			return;
		}
		if (tag != CountdownEvent || _deadline == 0)
			return;
		raiseInterrupt(Interrupt);
		if (!_periodic)
		{
			_deadline = 0;
			return;
		}
		_deadline = cycle + _period;
		if (Scheduler* events = scheduler())
			events->schedule(*this, _deadline, CountdownEvent);
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

	u32 TimerDevice::read(Address offset)
	{
		// The cycle count is 64 bits and read as two words, like the nanosecond one: the low read takes
		// the count and keeps its high half, so the pair is one moment however many cycles pass between
		// the two reads. A 32-bit count wraps in 43 s at 100 MHz.
		if (offset == TicksRegister)
		{
			const u64 ticks = now();
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

	void TimerDevice::write(Address offset, u32 value)
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
			syncAlarm();                       // one already past fires now
			return;
		}

		if (offset != CommandRegister)
			return;

		const bool periodic = (value & 0x80000000u) != 0;
		const u64 count = value & 0x7FFFFFFFu;

		arm(count, periodic && count > 0);
	}

	const RegisterMap& TimerDevice::registers() const
	{
		static constexpr RegisterMap map{ "timer", Registers };
		return map;
	}
}
