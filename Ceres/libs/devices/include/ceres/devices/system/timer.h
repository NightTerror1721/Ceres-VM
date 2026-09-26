#pragma once

#include <ceres/vm/mmio_bus.h>

namespace ceres::devices
{
	using namespace vm;


	// Gives the machine a sense of time, and with it the asynchronous interrupt source it never
	// had. Until now HALT suspended the machine for good, because nothing could ever wake it.
	//
	// Time is the machine's own (plan/v2 SPEC 3 and 5.7): CPU cycles, and nanoseconds, milliseconds and the
	// real-time clock worked out from them at the CPU clock, so a program behaves the same on every run and on
	// every host. The countdown and the alarm are events on the machine's scheduler, and a halted machine
	// jumps straight to them.
	class TimerDevice : public IODevice
	{
	public:
		static inline constexpr Address CyclesLowRegister = Address(0x00);        // R: CPU cycles since the start, low word; latches the high word
		static inline constexpr Address CyclesHighRegister = Address(0x04);       // R: the high word latched by the last read of CyclesLow
		static inline constexpr Address CountdownRegister = Address(0x08);        // RW: cycles until the timer's interrupt; 0 disarms; reads what is left
		static inline constexpr Address CountdownControlRegister = Address(0x0C); // RW: bit 0 periodic (re-arms with the last value written to Countdown)
		static inline constexpr Address NanosLowRegister = Address(0x10);         // R: nanoseconds since the start, low word; latches the high word
		static inline constexpr Address NanosHighRegister = Address(0x14);        // R: the high word latched by the last read of NanosLow
		static inline constexpr Address MillisRegister = Address(0x18);           // R: milliseconds since the start (32 bits, wraps)
		static inline constexpr Address RtcRegister = Address(0x1C);              // R: seconds since 1970 (low word): the start value plus the machine's time
		static inline constexpr Address AlarmLowRegister = Address(0x20);         // RW: the alarm instant in nanoseconds, low word (reads 0 when disarmed)
		static inline constexpr Address AlarmHighRegister = Address(0x24);        // RW: the high word; writing it arms the alarm at high:low (0:0 disarms)
		static inline constexpr Address CpuClockHzRegister = Address(0x28);       // R: the CPU clock, cycles per second (as SystemControl's)

		static inline constexpr u32 ControlPeriodic = 1u << 0;

		// Which interrupt the countdown requests when it runs out. The first user interrupt, so it needs
		// STI to be delivered and cannot surprise a program that never asked for it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt0;

		// Which interrupt the alarm requests when its instant comes: its own, so a handler never has to ask
		// which of the two fired. (SPEC 5.7 gives it 17, which the terminal holds until the new map of F4.3.)
		static inline constexpr InterruptNumber AlarmInterrupt = InterruptNumber::UserInterrupt8;

		// The scheduler tags of the timer's two events.
		static inline constexpr u32 CountdownEvent = 0;
		static inline constexpr u32 AlarmEvent = 1;

		// Everything the timer remembers. Exposed so a debugger can put the whole machine back
		// where it was: without the timer, a restored snapshot would keep counting from wherever
		// the live run had got to and fire its interrupt at the wrong moment.
		struct State
		{
			u64 cycles = 0;      // the cycle count when it was taken; the engine's to restore, not the timer's
			u64 remaining = 0;   // cycles until the countdown fires, 0 when disarmed
			bool periodic = false;
			u64 period = 0;
			u32 nanosHigh = 0;   // the half of the nanosecond count that the last low read latched
			u32 cyclesHigh = 0;  // the half of the cycle count that the last CyclesLow read latched
			u64 alarmNanos = 0;  // the armed alarm instant, 0 when disarmed
			u32 alarmLow = 0;    // the low word written, waiting for the high one
		};

	private:
		u64 _deadline = 0;    // the cycle the countdown fires on; 0 means disarmed
		bool _periodic = false;
		u64 _period = 0;      // the last value written to Countdown: what a periodic countdown re-arms with
		i64 _rtcStart;        // seconds since 1970 when the machine started (--rtc, or the host's clock)
		u32 _nanosHigh = 0;   // latched by a read of the low word, so the pair is one instant
		u32 _cyclesHigh = 0;  // the same for the cycle count
		u64 _alarmNanos = 0;  // the alarm instant on the nanosecond clock, 0 when disarmed
		u32 _alarmLow = 0;    // AlarmLowRegister's word, taken when the high one arms

	public:
		TimerDevice();
		TimerDevice(const TimerDevice&) = delete;
		TimerDevice(TimerDevice&&) = delete;
		~TimerDevice() override = default;

		TimerDevice& operator=(const TimerDevice&) = delete;
		TimerDevice& operator=(TimerDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Timer, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Timer);
		}

		// The CPU cycles since the machine started: the engine's count, 0 while the timer is not attached.
		u64 cycles() const noexcept;
		// The CPU clock the scheduler keeps, DefaultCpuClockHz while the timer is not attached.
		u64 clockHz() const noexcept;
		// The nanoseconds since the machine started: the cycles at the CPU clock.
		u64 nanos() const noexcept;
		bool isArmed() const noexcept { return _deadline != 0; }

		// Arms the countdown directly, for a host that wants a heartbeat without the program asking: it fires
		// `cyclesFromNow` cycles from now. A timer not attached to a bus keeps the arming but has no clock to fire on.
		void arm(u64 cyclesFromNow, bool periodic = false) noexcept;

		// The real-time clock's start: seconds since 1970 at cycle 0. The host's clock when the timer is made,
		// or what --rtc says. A reset starts the machine's time again, and the clock with it.
		void setRtcStart(i64 secondsSinceEpoch) noexcept { _rtcStart = secondsSinceEpoch; }
		i64 rtcStart() const noexcept { return _rtcStart; }
		// Seconds since 1970 now: the start plus the machine's time.
		i64 rtc() const noexcept;

		State captureState() const noexcept;

		// A reset disarms the countdown and the alarm, as the engine restarts its count: a program that is
		// starting over must not be interrupted by what the last one armed.
		void reset() override;

		// Puts the timer back as it was captured, against the clock as it stands: restore the engine's cycle
		// count first, so the countdown and the alarm land on the cycles they would have.
		void restoreState(const State& state) noexcept;

		u64 alarmNanos() const noexcept { return _alarmNanos; }

	private:
		u64 now() const noexcept;
		// The first cycle at or after `nanos`: where an alarm for that instant is scheduled.
		u64 cycleAtNanos(u64 nanos) const noexcept;
		// Schedules the armed alarm on its cycle, or fires it at once when its instant has come.
		void syncAlarm();
		// Schedules the countdown on _deadline, or drops it when disarmed.
		void syncCountdown() noexcept;

	public:
		// The countdown running out (it re-arms from the cycle it was due on, when periodic, so a period
		// never drifts) or the alarm's instant coming.
		void onEvent(u32 tag, u64 cycle) override;

	public:
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;

	};
}
