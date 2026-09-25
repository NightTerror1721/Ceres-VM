#pragma once

#include <ceres/vm/mmio_bus.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <print>
#include <span>

namespace ceres::devices
{
	using namespace vm;


	// Gives the machine a sense of time, and with it the asynchronous interrupt source it never
	// had. Until now HALT suspended the machine for good, because nothing could ever wake it.
	//
	// Time is counted in executed instructions rather than wall clock, so a program behaves the
	// same on every run and on every machine. RTC_TIME is the one exception: it reports real
	// seconds, and nothing depends on it.
	class TimerDevice : public IODevice
	{
	public:
		static inline constexpr Address TicksRegister = Address(0x00);   // Read: the low word of the ticks so far; also latches the high word
		static inline constexpr Address ClockRegister = Address(0x04);   // Read: seconds since the epoch
		static inline constexpr Address CommandRegister = Address(0x08); // Write: fire after N ticks, 0 disarms
		static inline constexpr Address MillisRegister = Address(0x0C);  // Read: milliseconds since the machine started (wraps every 49 days)
		static inline constexpr Address NanosLowRegister = Address(0x10);  // Read: the low word of the nanoseconds since the machine started; also latches the high word
		static inline constexpr Address NanosHighRegister = Address(0x14); // Read: the high word latched by the last read of NanosLowRegister
		static inline constexpr Address NanosResolutionRegister = Address(0x18); // Read: the smallest step the nanosecond clock is seen to take, in nanoseconds
		static inline constexpr Address HaltClockRegister = Address(0x1C); // Read: ticks per second while the CPU is halted; 0 when time only moves by events
		static inline constexpr Address AlarmLowRegister = Address(0x20);  // Read/write: the low word of the alarm instant, in nanoseconds on NanosLow's clock (reads 0 when disarmed)
		static inline constexpr Address AlarmHighRegister = Address(0x24); // Read/write: the high word; writing it arms the alarm at high:low (0:0 disarms)
		static inline constexpr Address TicksHighRegister = Address(0x28); // Read: the high word of the tick count latched by the last read of TicksRegister

		// Which interrupt the timer requests when it expires. The first user interrupt, so it needs
		// STI to be delivered and cannot surprise a program that never asked for it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt0;

		// Which interrupt the real-time alarm requests when its instant comes: its own, so a handler
		// never has to ask which of the two fired. See AlarmLowRegister.
		static inline constexpr InterruptNumber AlarmInterrupt = InterruptNumber::UserInterrupt8;

		// A running machine looks at the host clock for the alarm once every this many instructions:
		// about 10 microseconds at the default rate, and a clock read per instruction would cost more
		// than the instruction.
		static inline constexpr u32 AlarmPollTicks = 1024;

		// Everything the timer remembers. Exposed so a debugger can put the whole machine back
		// where it was: without the timer, a restored snapshot would keep counting from wherever
		// the live run had got to and fire its interrupt at the wrong moment.
		struct State
		{
			u64 ticks = 0;
			u64 remaining = 0;
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
		u64 _ticks = 0;
		u64 _remaining = 0;   // 0 means disarmed
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
		u32 _alarmPoll = 0;                 // instructions since the running machine last looked at the clock
		u64 _alarmTick = 0;                 // the instant as a tick count, at the halted clock's rate, as of the last look
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

		u64 ticks() const noexcept { return _ticks; }
		bool isArmed() const noexcept { return _remaining > 0; }

		// Arms the timer directly, for a host that wants a heartbeat without the program asking.
		void arm(u64 ticksFromNow, bool periodic = false) noexcept;

		State captureState() const noexcept { return State{ _ticks, _remaining, _periodic, _period, _nanosHigh, _ticksHigh, _alarmNanos, _alarmLow }; }

		// A reset disarms the timer and restarts the count, as the engine restarts its own: a
		// program that is starting over must not be interrupted by what the last one armed.
		void reset() override;

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
		// by it to arm the timer for a halt: at the default 100 MHz, 16 ms is 1 600 000 ticks.
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
		// tick it falls on at the halted clock's rate, rounded up. A halted machine sleeps to that tick, and
		// the count it asks for stays put while it sleeps - worked out afresh from the clock at every look,
		// the ticks still to go would shrink as the host slept, and the halted clock would be held back
		// to them rather than moving on by the time that passed.
		void syncAlarm();

		// The smallest step the host clock is seen to take between two reads, in nanoseconds. A clock
		// that advances in 100 ns steps (the usual one on Windows) shows a run of equal readings and
		// then a jump of 100; one that reads every nanosecond shows whatever a read costs. Either way
		// this is what a program can rely on as the least it can tell apart. Never 0.
		static u32 measureNanosResolution() noexcept;

	public:
		// The one device every program that arms it relies on advancing every instruction, whether
		// or not it is armed - TicksRegister reads instructions executed even while disarmed.
		bool needsTick() const noexcept override { return true; }

		void tick() override;

		// The expiry, for a halted machine that sleeps until it instead of ticking there - the tick timer's
		// or the alarm's, whichever comes first. The alarm's is the tick syncAlarm() worked out at its last
		// look at the clock; if the host wakes a little before the instant, that look finds it still ahead and
		// works out a new one. With the halted clock off (0) time only moves by events, and the alarm has none
		// to offer.
		u64 ticksUntilEvent() const noexcept override;

		// `ticks` tick() calls at once. The bus never asks for more than ticksUntilEvent(), so the timer
		// expires at most once here, on exactly the tick it would have.
		void advance(u64 ticks) override;

	public:
		u32 readUnsignedWord(Address offset) override;

		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		// Writing N to the command register fires the timer N instructions later. Writing 0 disarms
		// it. The high bit asks for a periodic timer that re-arms itself after each expiry.
		void writeWord(Address offset, u32 value) override;

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}
