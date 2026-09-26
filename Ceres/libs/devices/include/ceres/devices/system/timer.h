#pragma once

#include <ceres/vm/mmio_bus.h>
#include <functional>

namespace ceres::devices
{
	using namespace vm;


	// Gives the machine a sense of time, and with it the asynchronous interrupt source it never
	// had. Until now HALT suspended the machine for good, because nothing could ever wake it.
	//
	// Time is the machine's own: CPU cycles (plan/v2 SPEC 3.2), and nanoseconds worked out from them at the
	// CPU clock, so a program behaves the same on every run and on every host. The countdown and the alarm are
	// events on the machine's scheduler, and a halted machine jumps straight to them. ClockRegister is the one
	// exception: it reports the host's real seconds, and nothing depends on it.
	class TimerDevice : public IODevice
	{
	public:
		static inline constexpr Address TicksRegister = Address(0x00);   // Read: the low word of the CPU cycles so far; also latches the high word
		static inline constexpr Address ClockRegister = Address(0x04);   // Read: seconds since the epoch
		static inline constexpr Address CommandRegister = Address(0x08); // Write: fire after N cycles, 0 disarms
		static inline constexpr Address MillisRegister = Address(0x0C);  // Read: milliseconds since the machine started (wraps every 49 days)
		static inline constexpr Address NanosLowRegister = Address(0x10);  // Read: the low word of the nanoseconds since the machine started; also latches the high word
		static inline constexpr Address NanosHighRegister = Address(0x14); // Read: the high word latched by the last read of NanosLowRegister
		static inline constexpr Address NanosResolutionRegister = Address(0x18); // Read: the nanoseconds one CPU cycle lasts, rounded up
		static inline constexpr Address HaltClockRegister = Address(0x1C); // Read: the CPU clock, in cycles per second, running or halted
		static inline constexpr Address AlarmLowRegister = Address(0x20);  // Read/write: the low word of the alarm instant, in nanoseconds on NanosLow's clock (reads 0 when disarmed)
		static inline constexpr Address AlarmHighRegister = Address(0x24); // Read/write: the high word; writing it arms the alarm at high:low (0:0 disarms)
		static inline constexpr Address TicksHighRegister = Address(0x28); // Read: the high word of the cycle count latched by the last read of TicksRegister

		// Which interrupt the timer requests when it expires. The first user interrupt, so it needs
		// STI to be delivered and cannot surprise a program that never asked for it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt0;

		// Which interrupt the alarm requests when its instant comes: its own, so a handler never has to ask
		// which of the two fired. See AlarmLowRegister.
		static inline constexpr InterruptNumber AlarmInterrupt = InterruptNumber::UserInterrupt8;

		// The scheduler tags of the timer's two events.
		static inline constexpr u32 CountdownEvent = 0;
		static inline constexpr u32 AlarmEvent = 1;

		// Everything the timer remembers. Exposed so a debugger can put the whole machine back
		// where it was: without the timer, a restored snapshot would keep counting from wherever
		// the live run had got to and fire its interrupt at the wrong moment.
		struct State
		{
			u64 ticks = 0;       // the cycle count when it was taken; the engine's to restore, not the timer's
			u64 remaining = 0;   // cycles until the countdown fires, 0 when disarmed
			bool periodic = false;
			u64 period = 0;
			u32 nanosHigh = 0;   // the half of the nanosecond count that the last low read latched
			u32 ticksHigh = 0;   // the half of the cycle count that the last TicksRegister read latched
			u64 alarmNanos = 0;  // the armed alarm instant, 0 when disarmed
			u32 alarmLow = 0;    // the low word written, waiting for the high one
		};

		// Where the real-time clock register gets its answer. The default is the host's wall clock,
		// which is the one thing in this machine that is not deterministic - so a debugger that
		// replays execution replaces it with a recording.
		using ClockSource = std::function<u32()>;

	private:
		u64 _deadline = 0;    // the cycle the countdown fires on; 0 means disarmed
		bool _periodic = false;
		u64 _period = 0;
		u64 _clockHz = DefaultCpuClockHz;   // what turns cycles into nanoseconds
		ClockSource _clockSource;
		u32 _nanosHigh = 0;                 // latched by a read of the low word, so the pair is one instant
		u32 _ticksHigh = 0;                 // the same for the cycle count: a read of TicksRegister latches it
		u32 _nanosResolution = 0;           // 0: the length of a cycle
		u64 _alarmNanos = 0;                // the alarm instant on the nanosecond clock, 0 when disarmed
		u32 _alarmLow = 0;                  // AlarmLowRegister's word, taken when the high one arms

	public:
		TimerDevice() = default;
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
		u64 ticks() const noexcept;
		// The nanoseconds since the machine started: the cycles at the CPU clock.
		u64 nanos() const noexcept;
		bool isArmed() const noexcept { return _deadline != 0; }

		// Arms the timer directly, for a host that wants a heartbeat without the program asking: it fires
		// `cyclesFromNow` cycles from now. A timer not attached to a bus keeps the arming but has no clock to fire on.
		void arm(u64 cyclesFromNow, bool periodic = false) noexcept;

		State captureState() const noexcept;

		// A reset disarms the timer and the alarm, as the engine restarts its count: a program that is
		// starting over must not be interrupted by what the last one armed.
		void reset() override;

		// Puts the timer back as it was captured, against the clock as it stands: restore the engine's cycle
		// count first, so the countdown and the alarm land on the cycles they would have.
		void restoreState(const State& state) noexcept;

		void setClockSource(ClockSource source) { _clockSource = std::move(source); }
		void clearClockSource() { _clockSource = nullptr; }

		// What NanosResolutionRegister answers when a test says so; by default the length of one cycle.
		void setNanosResolution(u32 nanoseconds) noexcept { _nanosResolution = nanoseconds; }

		u64 clockHz() const noexcept { return _clockHz; }
		u64 alarmNanos() const noexcept { return _alarmNanos; }

	private:
		u64 now() const noexcept;
		// The first cycle at or after `nanos`: where an alarm for that instant is scheduled.
		u64 cycleAtNanos(u64 nanos) const noexcept;
		// Schedules the armed alarm on its cycle, or fires it at once when its instant has come.
		void syncAlarm();

	public:
		// The countdown running out (it re-arms from the cycle it was due on, when periodic, so a period
		// never drifts) or the alarm's instant coming.
		void onEvent(u32 tag, u64 cycle) override;

	public:
		u32 read(Address offset) override;

		// Writing N to the command register fires the timer N cycles later. Writing 0 disarms
		// it. The high bit asks for a periodic timer that re-arms itself after each expiry.
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;

	};
}
