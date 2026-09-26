#include <ceres/devices/system/timer.h>
#include <chrono>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.7), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "CyclesLow",        RegisterAccess::Read,      0x0, true,  "CPU cycles since the start, low word; latches the high word." },
			{ 0x04, "CyclesHigh",       RegisterAccess::Read,      0x0, false, "The high word latched by the last read of CyclesLow." },
			{ 0x08, "Countdown",        RegisterAccess::ReadWrite, 0x0, false, "Cycles until the timer's interrupt; 0 disarms; reads what is left." },
			{ 0x0C, "CountdownControl", RegisterAccess::ReadWrite, 0x0, false, "Bit 0 periodic: re-arms with the last value written to Countdown." },
			{ 0x10, "NanosLow",         RegisterAccess::Read,      0x0, true,  "Nanoseconds since the start, low word; latches the high word." },
			{ 0x14, "NanosHigh",        RegisterAccess::Read,      0x0, false, "The high word latched by the last read of NanosLow." },
			{ 0x18, "Millis",           RegisterAccess::Read,      0x0, false, "Milliseconds since the start (32 bits, wraps)." },
			{ 0x1C, "Rtc",              RegisterAccess::Read,      0x0, false, "Seconds since 1970, low word: the start value plus the machine's time." },
			{ 0x20, "AlarmLow",         RegisterAccess::ReadWrite, 0x0, false, "The alarm instant in nanoseconds, low word (reads 0 when disarmed)." },
			{ 0x24, "AlarmHigh",        RegisterAccess::ReadWrite, 0x0, false, "The high word; writing it arms the alarm at high:low (0:0 disarms)." },
			{ 0x28, "CpuClockHz",       RegisterAccess::Read,      0x0, false, "The CPU clock, in cycles per second." },
		};

		constexpr u64 NanosPerSecond = 1'000'000'000;
	}

	TimerDevice::TimerDevice() :
		_rtcStart(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count())
	{}

	u64 TimerDevice::now() const noexcept
	{
		const Scheduler* clock = scheduler();
		return clock != nullptr ? clock->now() : 0;
	}

	u64 TimerDevice::cycles() const noexcept
	{
		return now();
	}

	u64 TimerDevice::clockHz() const noexcept
	{
		const Scheduler* clock = scheduler();
		return clock != nullptr ? clock->clockHz() : DefaultCpuClockHz;
	}

	// In two parts, so the product never overflows: whole seconds of cycles, then the rest.
	u64 TimerDevice::nanos() const noexcept
	{
		const u64 count = now();
		const u64 hz = clockHz();
		return (count / hz) * NanosPerSecond + (count % hz) * NanosPerSecond / hz;
	}

	i64 TimerDevice::rtc() const noexcept
	{
		return _rtcStart + static_cast<i64>(now() / clockHz());
	}

	u64 TimerDevice::cycleAtNanos(u64 instant) const noexcept
	{
		const u64 hz = clockHz();
		return (instant / NanosPerSecond) * hz + ((instant % NanosPerSecond) * hz + NanosPerSecond - 1) / NanosPerSecond;
	}

	void TimerDevice::syncCountdown() noexcept
	{
		if (Scheduler* events = scheduler())
		{
			if (_deadline != 0)
				events->schedule(*this, _deadline, CountdownEvent);
			else
				events->cancel(*this, CountdownEvent);
		}
	}

	void TimerDevice::arm(u64 cyclesFromNow, bool periodic) noexcept
	{
		_periodic = periodic;
		_period = cyclesFromNow;
		_deadline = cyclesFromNow > 0 ? now() + cyclesFromNow : 0;
		syncCountdown();
	}

	TimerDevice::State TimerDevice::captureState() const noexcept
	{
		const u64 clock = now();
		const u64 remaining = _deadline == 0 ? 0 : _deadline > clock ? _deadline - clock : 1;
		return State{ clock, remaining, _periodic, _period, _nanosHigh, _cyclesHigh, _alarmNanos, _alarmLow };
	}

	void TimerDevice::reset()
	{
		_deadline = 0;
		_periodic = false;
		_period = 0;
		_nanosHigh = 0;                                   // nothing latched yet, as at power-on
		_cyclesHigh = 0;
		_alarmNanos = 0;
		_alarmLow = 0;
		if (Scheduler* events = scheduler())
			events->cancelAll(*this);
	}

	void TimerDevice::restoreState(const State& state) noexcept
	{
		_deadline = state.remaining > 0 ? now() + state.remaining : 0;
		syncCountdown();
		_periodic = state.periodic;
		_period = state.period;
		_nanosHigh = state.nanosHigh;
		_cyclesHigh = state.cyclesHigh;
		_alarmNanos = state.alarmNanos;
		_alarmLow = state.alarmLow;
		syncAlarm();
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
		if (nanos() >= _alarmNanos)
		{
			_alarmNanos = 0;
			if (events != nullptr)
				events->cancel(*this, AlarmEvent);
			raiseInterrupt(AlarmInterrupt);
			return;
		}
		if (events != nullptr)
			events->schedule(*this, cycleAtNanos(_alarmNanos), AlarmEvent);
	}

	void TimerDevice::onEvent(u32 tag, u64 cycle)
	{
		if (tag == AlarmEvent)
		{
			syncAlarm();                                  // its cycle is the first at or after the instant: it fires
			return;
		}
		if (tag != CountdownEvent || _deadline == 0)
			return;
		raiseInterrupt(Interrupt);
		_deadline = _periodic && _period != 0 ? cycle + _period : 0;
		syncCountdown();
	}

	u32 TimerDevice::read(Address offset)
	{
		// The 64-bit counts are read as two words: the low read takes the count and keeps its high half,
		// so the pair is one moment however much passes between the two reads.
		if (offset == CyclesLowRegister)
		{
			const u64 count = now();
			_cyclesHigh = static_cast<u32>(count >> 32);
			return static_cast<u32>(count);
		}
		if (offset == CyclesHighRegister)
			return _cyclesHigh;

		if (offset == CountdownRegister)
		{
			const u64 clock = now();
			if (_deadline == 0)
				return 0;
			const u64 left = _deadline > clock ? _deadline - clock : 1;   // due, and runs before the next instruction
			return left > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(left);
		}
		if (offset == CountdownControlRegister)
			return _periodic ? ControlPeriodic : 0;

		if (offset == NanosLowRegister)
		{
			const u64 instant = nanos();
			_nanosHigh = static_cast<u32>(instant >> 32);
			return static_cast<u32>(instant);
		}
		if (offset == NanosHighRegister)
			return _nanosHigh;

		if (offset == MillisRegister)
			return static_cast<u32>(nanos() / 1'000'000);

		if (offset == RtcRegister)
			return static_cast<u32>(rtc());

		// The armed instant, 0:0 when disarmed - so a program can tell the two apart, even for an
		// instant in the first 4.29 s. A low word written and not yet armed does not show.
		if (offset == AlarmLowRegister)
			return static_cast<u32>(_alarmNanos);
		if (offset == AlarmHighRegister)
			return static_cast<u32>(_alarmNanos >> 32);

		if (offset == CpuClockHzRegister)
			return static_cast<u32>(clockHz());

		return 0;
	}

	void TimerDevice::write(Address offset, u32 value)
	{
		// Countdown: N cycles from now, and the period a periodic countdown re-arms with; 0 disarms it.
		if (offset == CountdownRegister)
		{
			_period = value;
			_deadline = value != 0 ? now() + value : 0;
			syncCountdown();
			return;
		}
		if (offset == CountdownControlRegister)
		{
			_periodic = (value & ControlPeriodic) != 0;
			return;
		}

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
		}
	}

	const RegisterMap& TimerDevice::registers() const
	{
		static constexpr RegisterMap map{ "timer", Registers };
		return map;
	}
}
