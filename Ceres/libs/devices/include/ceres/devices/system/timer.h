#pragma once

#include <ceres/vm/mmio_bus.h>
#include <chrono>
#include <functional>

namespace ceres::devices
{
	using namespace vm;


	// Gives the machine a sense of time, and with it the asynchronous interrupt source it never
	// had. Until now HALT suspended the machine for good, because nothing could ever wake it.
	//
	// Time is counted in CPU cycles (plan/v2 SPEC 3.2) rather than wall clock, so a program behaves the
	// same on every run and on every machine: the count is the engine's own, and the countdown is an event
	// on the machine's scheduler. RTC_TIME is the one exception: it reports real seconds, and nothing
	// depends on it.
	class TimerDevice : public IODevice
	{
	public:
		static inline constexpr Address TicksRegister = Address(0x00);   // Read: the low word of the CPU cycles so far; also latches the high word
		static inline constexpr Address ClockRegister = Address(0x04);   // Read: seconds since the epoch
		static inline constexpr Address CommandRegister = Address(0x08); // Write: fire after N cycles, 0 disarms
		static inline constexpr Address MillisRegister = Address(0x0C);  // Read: milliseconds since the machine started (wraps every 49 days)
		static inline constexpr Address NanosLowRegister = Address(0x10);  // Read: the low word of the nanoseconds since the machine started; also latches the high word
		static inline constexpr Address NanosHighRegister = Address(0x14); // Read: the high word latched by the last read of NanosLowRegister
		static inline constexpr Address NanosResolutionRegister = Address(0x18); // Read: the smallest step the nanosecond clock is seen to take, in nanoseconds
		static inline constexpr Address HaltClockRegister = Address(0x1C); // Read: cycles per second while the CPU is halted; 0 when time only moves by events
		static inline constexpr Address AlarmLowRegister = Address(0x20);  // Read/write: the low word of the alarm instant, in nanoseconds on NanosLow's clock (reads 0 when disarmed)
		static inline constexpr Address AlarmHighRegister = Address(0x24); // Read/write: the high word; writing it arms the alarm at high:low (0:0 disarms)
		static inline constexpr Address TicksHighRegister = Address(0x28); // Read: the high word of the tick count latched by the last read of TicksRegister

		// Which interrupt the timer requests when it expires. The first user interrupt, so it needs
		// STI to be delivered and cannot surprise a program that never asked for it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt0;

		// Which interrupt the real-time alarm requests when its instant comes: its own, so a handler
		// never has to ask which of the two fired. See AlarmLowRegister.
		static inline constexpr InterruptNumber AlarmInterrupt = InterruptNumber::UserInterrupt8;

		// The scheduler tags of the timer's two events.
		static inline constexpr u32 CountdownEvent = 0;
		static inline constexpr u32 AlarmEvent = 1;
		// With the halted clock off (0) the alarm cannot be turned into cycles, and a running machine looks
		// at the host clock for it once every this many cycles instead.
		static inline constexpr u32 AlarmPollCycles = 16384;

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
			u32 ticksHigh = 0;   // the half of the tick count that the last TicksRegister read latched
			u64 alarmNanos = 0;  // the armed alarm instant, 0 when disarmed
			u32 alarmLow = 0;    // the low word written, waiting for the high one
		};

		// Where the real-time clock register gets its answer. The default is the host's wall clock,
		// which is the one thing in this machine that is not deterministic - so a debugger that
		// replays execution replaces it with a recording.
		using ClockSource = std::function<u32()>;

		// The same for the nanosecond counter, which does not fit a word: the whole 64-bit count.
		using NanosSource = std::function<u64()>;

	private:
		u64 _deadline = 0;    // the cycle the countdown fires on; 0 means disarmed
		bool _periodic = false;
		u64 _period = 0;
		ClockSource _clockSource;
		ClockSource _millisSource;
		NanosSource _nanosSource;
		u32 _nanosHigh = 0;                 // latched by a read of the low word, so the pair is one instant
		u32 _ticksHigh = 0;                 // the same for the tick count: a read of TicksRegister latches it
		u32 _nanosResolution = 0;           // measured on first use, 0 until then
		u32 _haltClockHz = static_cast<u32>(vm::DefaultHaltClockHz);   // what HaltClockRegister reports
		u64 _alarmNanos = 0;                // the alarm instant on the nanosecond clock, 0 when disarmed
		u32 _alarmLow = 0;                  // AlarmLowRegister's word, taken when the high one arms
		std::chrono::steady_clock::time_point _started = std::chrono::steady_clock::now();

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
		bool isArmed() const noexcept { return _deadline != 0; }

		// Arms the timer directly, for a host that wants a heartbeat without the program asking: it fires
		// `cyclesFromNow` cycles from now. A timer not attached to a bus keeps the arming but has no clock to fire on.
		void arm(u64 cyclesFromNow, bool periodic = false) noexcept;

		State captureState() const noexcept;

		// A reset disarms the timer, as the engine restarts its count: a program that is starting
		// over must not be interrupted by what the last one armed.
		void reset() override;

		// Puts the timer back as it was captured, against the clock as it stands: restore the engine's cycle
		// count first, so the countdown lands on the cycle it would have.
		void restoreState(const State& state) noexcept;

		void setClockSource(ClockSource source) { _clockSource = std::move(source); }
		void clearClockSource() { _clockSource = nullptr; }

		// The millisecond counter reads the host's steady clock, which makes it as non-deterministic
		// as the seconds register, and a debugger replaces it in the same way.
		void setMillisSource(ClockSource source) { _millisSource = std::move(source); }
		void clearMillisSource() { _millisSource = nullptr; }

		// The nanosecond counter is a 64-bit count of the host's steady clock since the machine started.
		// It is read in two halves: reading the low word latches the high word, so the two reads give
		// one instant however much time passes between them. A debugger replaces the source with a
		// recording, as it does for the others.
		void setNanosSource(NanosSource source) { _nanosSource = std::move(source); }
		void clearNanosSource() { _nanosSource = nullptr; }

		// What NanosResolutionRegister answers. Left to itself the device looks at how far apart two
		// reads of the host clock ever are; a test or a debugger that fakes the clock says what it fakes.
		void setNanosResolution(u32 nanoseconds) noexcept { _nanosResolution = nanoseconds; }

		// What HaltClockRegister answers: the rate the host runs a halted machine's clock at
		// (ExecutionEngine::setHaltClock), set by whoever sets that. A program divides a real-time wait
		// by it to arm the timer for a halt: at the default 100 MHz, 16 ms is 1 600 000 cycles.
		void setHaltClockRate(u32 hz) noexcept { _haltClockHz = hz; }
		u32 haltClockRate() const noexcept { return _haltClockHz; }

		u64 alarmNanos() const noexcept { return _alarmNanos; }

	private:
		// The host clock the alarm is kept against: the nanosecond count's own, since the machine
		// started. Never a debugger's recording - a recording is replayed read by read, and the alarm
		// looking at it would use up reads the program made - so the alarm is real time even there.
		u64 liveNanos() const noexcept
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _started).count());
		}

		// Looks at the host clock: an instant that has come fires, and one still ahead is turned into the
		// cycle it falls on at the halted clock's rate, rounded up, and scheduled. A halted machine sleeps to
		// that cycle and is on time; a running one gets there sooner, as it runs faster than the halted clock,
		// and looks again - so the alarm is never early, and late by no more than the last look is short.
		void syncAlarm();
		u64 now() const noexcept;

		// The smallest step the host clock is seen to take between two reads, in nanoseconds. A clock
		// that advances in 100 ns steps (the usual one on Windows) shows a run of equal readings and
		// then a jump of 100; one that reads every nanosecond shows whatever a read costs. Either way
		// this is what a program can rely on as the least it can tell apart. Never 0.
		static u32 measureNanosResolution() noexcept;

	public:
		// The countdown running out (it re-arms from the cycle it was due on, when periodic, so a period
		// never drifts) or the alarm's time to look at the host clock.
		void onEvent(u32 tag, u64 cycle) override;

	public:
		u32 read(Address offset) override;


		// Writing N to the command register fires the timer N cycles later. Writing 0 disarms
		// it. The high bit asks for a periodic timer that re-arms itself after each expiry.
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;

	};
}
