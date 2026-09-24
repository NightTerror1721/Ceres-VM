#pragma once

#include <ceres/vm/mmio_bus.h>
#include <print>
#include <atomic>
#include <span>
#include <functional>
#include <chrono>
#include <mutex>

namespace ceres::devices
{
	using namespace vm;

	// Every device below follows the same register layout convention: scalar registers sit at low,
	// word-aligned offsets (0x00, 0x04, 0x08, ...) within the device's 64 KiB MMIO slot - the direct
	// replacement for what used to be a handful of single-byte port numbers, just with room to
	// spare. A device that also moves blocks of memory (the disk, the framebuffer) additionally
	// claims three registers near the top of its slot - BLOCK_ADDR/BLOCK_LEN/BLOCK_CMD at
	// 0xF0/0xF4/0xF8 - so a bulk transfer is still one MMIO write to trigger, the same shape `outm`/
	// `inm` used to give it, just addressed like everything else now.

	class SystemControlDevice : public IODevice
	{
	public:
		using ShutdownCallback = std::function<void()>;
		using ResetCallback = std::function<void()>;
		// Told the new value of the features register whenever a program writes it, so the host can
		// pass on to the engine the settings the engine itself has to act on.
		using FeaturesCallback = std::function<void(u32)>;
		// How the device reaches the engine's stack limit, which it does not own: the host passes the
		// engine's stackLimit() and setProgramStackLimit().
		using StackLimitGetter = std::function<u32()>;
		using StackLimitSetter = std::function<void(u32)>;
		// The last memory fault's data address and access, read from the engine (faultAddress/faultAccess).
		using FaultInfoGetter = std::function<u32()>;

		// Write-only: writing specific commands to this register triggers system control actions.
		// The low byte is the command; the next byte is the exit status a shutdown reports.
		static inline constexpr Address CommandRegister = Address(0x00);
		// Read-only: how many bytes of RAM the machine has.
		static inline constexpr Address MemorySizeRegister = Address(0x04);
		// Read/write: switches for behaviour that is off by default, so a program that never asks
		// keeps the machine it always had.
		static inline constexpr Address FeaturesRegister = Address(0x08);
		// Read/write: the lowest address the program's stack may reach. A push below it raises
		// StackOverflow. It starts at the end of the loaded image; a program raises it over its heap as
		// the heap grows, and cannot lower it below the image. All-ones when the host connected no engine.
		static inline constexpr Address StackLimitRegister = Address(0x0C);
		// Read-only: the data address of the last memory fault (AlignmentFault, MemoryFault, PageFault),
		// and its access - 1 read, 2 write, 3 instruction fetch in bits 0-7, the size in bytes in 8-15 -
		// so a fault handler can say "store word to 0x00000801" and not only where the instruction was.
		static inline constexpr Address FaultAddressRegister = Address(0x10);
		static inline constexpr Address FaultAccessRegister = Address(0x14);

		static inline constexpr u32 CommandShutdown = 0x01;
		static inline constexpr u32 CommandReset = 0x02;

		// Division by zero raises the DivisionByZero interrupt (number 4) instead of only setting
		// the Trap flag. The handler returns to the instruction after the division, whose
		// destination was left as it was.
		static inline constexpr u32 FeatureDivisionFault = 1u << 0;

	private:
		ShutdownCallback _shutdownCallback;
		ResetCallback _resetCallback;
		FeaturesCallback _featuresCallback;
		StackLimitGetter _stackLimitGetter;
		StackLimitSetter _stackLimitSetter;
		FaultInfoGetter _faultAddressGetter;
		FaultInfoGetter _faultAccessGetter;
		u32 _features = 0;
		std::atomic<u8> _exitCode{ 0 };

	public:
		explicit SystemControlDevice(ShutdownCallback shutdownCallback = {}, ResetCallback resetCallback = {}) :
			_shutdownCallback(std::move(shutdownCallback)),
			_resetCallback(std::move(resetCallback))
		{}

		SystemControlDevice(const SystemControlDevice&) = delete;
		SystemControlDevice(SystemControlDevice&&) = delete;
		~SystemControlDevice() override = default;

		SystemControlDevice& operator=(const SystemControlDevice&) = delete;
		SystemControlDevice& operator=(SystemControlDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::SystemControl, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::SystemControl);
		}

		void setShutdownCallback(ShutdownCallback callback)
		{
			_shutdownCallback = std::move(callback);
		}

		void setResetCallback(ResetCallback callback)
		{
			_resetCallback = std::move(callback);
		}

		// A reset switches every feature off again, the way the machine powers on.
		void reset() override
		{
			_features = 0;
			if (_featuresCallback)
				_featuresCallback(0);
		}

		void setFeaturesCallback(FeaturesCallback callback)
		{
			_featuresCallback = std::move(callback);
		}

		void setStackLimitHandlers(StackLimitGetter getter, StackLimitSetter setter)
		{
			_stackLimitGetter = std::move(getter);
			_stackLimitSetter = std::move(setter);
		}

		void setFaultInfoHandlers(FaultInfoGetter address, FaultInfoGetter access)
		{
			_faultAddressGetter = std::move(address);
			_faultAccessGetter = std::move(access);
		}

		// The status the program shut the machine down with: the second byte of the word it wrote
		// (0 for a plain byte write, which is what every program written before this existed does).
		u8 exitCode() const noexcept { return _exitCode.load(std::memory_order_relaxed); }

		u32 features() const noexcept { return _features; }

	private:
		// A byte write carries only the command; a halfword or a word carries the exit status
		// above it.
		void command(u32 value)
		{
			const u32 code = value & 0xFF;
			if (code == CommandShutdown)
			{
				_exitCode.store(static_cast<u8>((value >> 8) & 0xFF), std::memory_order_relaxed);
				if (_shutdownCallback)
					_shutdownCallback();
			}
			else if (code == CommandReset)
			{
				if (_resetCallback)
					_resetCallback();
			}
		}

		bool readable(Address offset, u32& value) const
		{
			if (offset == MemorySizeRegister)
			{
				value = static_cast<u32>(memory().size());
				return true;
			}
			if (offset == FeaturesRegister)
			{
				value = _features;
				return true;
			}
			if (offset == StackLimitRegister && _stackLimitGetter)
			{
				value = _stackLimitGetter();
				return true;
			}
			if (offset == FaultAddressRegister && _faultAddressGetter)
			{
				value = _faultAddressGetter();
				return true;
			}
			if (offset == FaultAccessRegister && _faultAccessGetter)
			{
				value = _faultAccessGetter();
				return true;
			}
			return false;
		}

	public:
		u8 readUnsignedByte(Address offset) override { u32 v; return readable(offset, v) ? static_cast<u8>(v) : 0xFF; }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { u32 v; return readable(offset, v) ? static_cast<u16>(v) : 0xFFFF; }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }
		u32 readUnsignedWord(Address offset) override { u32 v; return readable(offset, v) ? v : 0xFFFFFFFF; }

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
		void writeWord(Address offset, u32 value) override
		{
			if (offset == CommandRegister)
			{
				command(value);
			}
			else if (offset == FeaturesRegister)
			{
				_features = value;
				if (_featuresCallback)
					_featuresCallback(value);
			}
			else if (offset == StackLimitRegister && _stackLimitSetter)
			{
				_stackLimitSetter(value);
			}
		}
	};

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
		void arm(u64 ticksFromNow, bool periodic = false) noexcept
		{
			_remaining = ticksFromNow;
			_periodic = periodic;
			_period = ticksFromNow;
		}

		State captureState() const noexcept { return State{ _ticks, _remaining, _periodic, _period, _nanosHigh, _ticksHigh, _alarmNanos, _alarmLow }; }

		// A reset disarms the timer and restarts the count, as the engine restarts its own: a
		// program that is starting over must not be interrupted by what the last one armed.
		void reset() override
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

		void restoreState(const State& state) noexcept
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
		void syncAlarm()
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

		// The smallest step the host clock is seen to take between two reads, in nanoseconds. A clock
		// that advances in 100 ns steps (the usual one on Windows) shows a run of equal readings and
		// then a jump of 100; one that reads every nanosecond shows whatever a read costs. Either way
		// this is what a program can rely on as the least it can tell apart. Never 0.
		static u32 measureNanosResolution() noexcept
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

	public:
		// The one device every program that arms it relies on advancing every instruction, whether
		// or not it is armed - TicksRegister reads instructions executed even while disarmed.
		bool needsTick() const noexcept override { return true; }

		void tick() override
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

		// The expiry, for a halted machine that sleeps until it instead of ticking there - the tick timer's
		// or the alarm's, whichever comes first. The alarm's is the tick syncAlarm() worked out at its last
		// look at the clock; if the host wakes a little before the instant, that look finds it still ahead and
		// works out a new one. With the halted clock off (0) time only moves by events, and the alarm has none
		// to offer.
		u64 ticksUntilEvent() const noexcept override
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

		// `ticks` tick() calls at once. The bus never asks for more than ticksUntilEvent(), so the timer
		// expires at most once here, on exactly the tick it would have.
		void advance(u64 ticks) override
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

	public:
		u32 readUnsignedWord(Address offset) override
		{
			// The tick count is 64 bits and read as two words, like the nanosecond one: the low read takes
			// the count and keeps its high half, so the pair is one moment however many ticks pass between
			// the two reads. A 32-bit count wraps in 43 s at the usual rate.
			if (offset == TicksRegister)
			{
				_ticksHigh = static_cast<u32>(_ticks >> 32);
				return static_cast<u32>(_ticks);
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

		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		// Writing N to the command register fires the timer N instructions later. Writing 0 disarms
		// it. The high bit asks for a periodic timer that re-arms itself after each expiry.
		void writeWord(Address offset, u32 value) override
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

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};

	class TerminalDevice : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00); // Read-only: bit 0 input available, bit 1 ready for output, bit 2 end of input.
		static inline constexpr Address OutputRegister = Address(0x04); // Write-only: writing a byte to this register outputs it to the terminal.
		static inline constexpr Address InputRegister = Address(0x08);  // Read-only: reading from this register returns the next byte of input, or 0 if none is available.
		static inline constexpr Address BytesAvailableRegister = Address(0x0C); // Read-only: bytes currently sitting unread in the input ring.
		static inline constexpr Address BlockReadCountRegister = Address(0x10); // Read-only: bytes the most recent block-read actually moved into RAM.
		static inline constexpr Address DroppedInputRegister = Address(0x14); // Read-only: input bytes discarded by a full ring (truncated to 32 bits).
		static inline constexpr Address ModeRegister = Address(0x18); // Read/write: write ModeRaw to ask the host for keys as they are pressed (no line editing, no echo); read what the host granted.

		// A bulk transfer: write the RAM address and length, then a command (1 = read from the
		// terminal's input ring into RAM, 2 = write RAM out to the terminal) - the direct
		// replacement for what `inm`/`outm` used to do in one instruction.
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);
		static inline constexpr u32 BlockCommandRead = 1;
		static inline constexpr u32 BlockCommandWrite = 2;

		// Which interrupt pushInput() requests once new bytes are actually sitting in the buffer.
		// The second user interrupt (the first, UserInterrupt0, is the timer's) - so a program that
		// never expects terminal input keeps working exactly as before: STI is still required, and
		// nothing raises this unless pushInput() is called at all.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt1;

	public:
		// Kept small to model a simple UART. Hosts can detect loss through droppedInputBytes().
		static inline constexpr usize InputBufferCapacity = 64;

	private:
		static inline constexpr u8 RxReadyMask = 0x01; // Bit 0 indicates if input is available.
		static inline constexpr u8 TxReadyMask = 0x02; // Bit 1 indicates if the terminal is ready to accept output (always ready in this simple implementation).
		static inline constexpr u8 EofMask = 0x04;     // Bit 2: the host closed the input and every byte it sent has been read - nothing more will ever arrive.

	public:
		// The bits of StatusRegister, for the code that reads them.
		static inline constexpr u32 StatusInputAvailable = RxReadyMask;
		static inline constexpr u32 StatusOutputReady = TxReadyMask;
		static inline constexpr u32 StatusEndOfInput = EofMask;

		// The bits of ModeRegister. A write is a request; a read is the answer. A host that cannot give
		// raw keys (input from a pipe, a file) answers 0, and the program keeps reading the terminal.
		static inline constexpr u32 ModeRaw = 1u << 0;        // Keys arrive as they are pressed: no line buffering, no echo.
		static inline constexpr u32 ModeKeystrokes = 1u << 1; // Read-only: they arrive on the keyboard device's KeyRegister.

		// Decides what a write to ModeRegister gets: it is handed the requested bits and returns the granted
		// ones. The host switches its console in there. Empty means nothing is ever granted.
		using ModeHandler = std::function<u32(u32)>;

	private:

	public:
		// Where a byte written to the output register ends up. `ceres run` leaves it empty and the
		// bytes go to stdout, which is what a plain terminal program wants; a debugger installs
		// one so the program's output can be forwarded to the editor instead of racing with a
		// protocol sharing that same stream.
		using OutputSink = std::function<void(u8)>;

	private:
		std::array<u8, InputBufferCapacity> _buffer{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};
		std::atomic<u64> _droppedInputBytes{0};
		std::atomic<bool> _inputClosed{false};
		// Protects the byte array while a debugger snapshots it. Head/tail remain atomic so the
		// single-producer/single-consumer fast path still has a minimal synchronization surface.
		mutable std::mutex _inputMutex;
		OutputSink _outputSink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;
		u32 _blockReadCount = 0;
		ModeHandler _modeHandler;
		std::atomic<u32> _modeRequested{0};
		std::atomic<u32> _modeGranted{0};

	public:
		TerminalDevice() = default;
		TerminalDevice(const TerminalDevice&) = delete;
		TerminalDevice(TerminalDevice&&) = delete;
		~TerminalDevice() override = default;

		TerminalDevice& operator=(const TerminalDevice&) = delete;
		TerminalDevice& operator=(TerminalDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Terminal, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Terminal);
		}

		void pushInput(std::span<const u8> input)
		{
			const std::lock_guard lock{_inputMutex};
			bool wroteAnyByte = false;
			for (u8 byte : input)
			{
				usize nextTail = (_tail.load(std::memory_order_relaxed) + 1) % InputBufferCapacity;
				if (nextTail == _head.load(std::memory_order_acquire))
				{
					_droppedInputBytes.fetch_add(1, std::memory_order_relaxed);
					continue;
				}

				_buffer[_tail.load(std::memory_order_relaxed)] = byte;
				_tail.store(nextTail, std::memory_order_release);
				wroteAnyByte = true;
			}

			// Idempotent if the machine is already awake or a request is already pending - raise()
			// only sets a bit, and triggerInterrupt() clears it once delivered - so calling this once
			// per pushInput() rather than once per byte costs nothing and wakes a halted CPU exactly
			// as reliably.
			if (wroteAnyByte)
				raiseInterrupt(Interrupt);
		}

		void pushInput(std::string_view input)
		{
			pushInput(std::span<const u8>(reinterpret_cast<const u8*>(input.data()), input.size()));
		}

		void pushInput(const char* input)
		{
			pushInput(std::string_view(input));
		}

		void pushInput(char input)
		{
			pushInput(std::string_view(&input, 1));
		}

		// The host has nothing more to send: stdin was closed, or the pipe ran dry. The status
		// register reports end of input once the program has also read what is still buffered, so a
		// reader can tell "no data yet" from "no data ever". The interrupt is raised so a program
		// halted waiting for input wakes up to notice.
		void closeInput()
		{
			_inputClosed.store(true, std::memory_order_release);
			raiseInterrupt(Interrupt);
		}

		bool isInputClosed() const noexcept { return _inputClosed.load(std::memory_order_acquire); }

		u64 droppedInputBytes() const noexcept { return _droppedInputBytes.load(std::memory_order_relaxed); }

		// How many bytes are currently buffered and unread. The one number a program needs to
		// decide whether to block-read, and how large a block to ask for, without polling the
		// status bit and guessing.
		usize availableBytes() const noexcept
		{
			const usize head = _head.load(std::memory_order_acquire);
			const usize tail = _tail.load(std::memory_order_acquire);
			return (tail - head + InputBufferCapacity) % InputBufferCapacity;
		}

		void setModeHandler(ModeHandler handler) { _modeHandler = std::move(handler); }

		// Whether the program asked for raw keys, so a host can tell whether to also type into the terminal.
		bool rawRequested() const noexcept { return (_modeRequested.load(std::memory_order_acquire) & ModeRaw) != 0; }

		void setOutputSink(OutputSink sink) { _outputSink = std::move(sink); }
		void clearOutputSink() { _outputSink = nullptr; }

		// The input ring, so a debugger restoring a snapshot can put back exactly the bytes the
		// program had not yet read. Copied rather than shared: the live buffer is written from
		// another thread.
		struct State
		{
			std::array<u8, InputBufferCapacity> buffer{};
			usize head = 0;
			usize tail = 0;
			bool closed = false;
		};

		State captureState() const noexcept
		{
			const std::lock_guard lock{_inputMutex};
			State state;
			state.buffer = _buffer;
			state.head = _head.load(std::memory_order_acquire);
			state.tail = _tail.load(std::memory_order_acquire);
			state.closed = _inputClosed.load(std::memory_order_acquire);
			return state;
		}

		void restoreState(const State& state) noexcept
		{
			const std::lock_guard lock{_inputMutex};
			_buffer = state.buffer;
			_head.store(state.head, std::memory_order_release);
			_tail.store(state.tail, std::memory_order_release);
			_inputClosed.store(state.closed, std::memory_order_release);
		}

	private:
		// The single place output leaves the device. Bytes are handed over one at a time and
		// deliberately not decoded here: a multi-byte UTF-8 sequence is written by the program as
		// several separate register writes, so only the consumer knows where a character ends.
		void emitByte(u8 value)
		{
			if (_outputSink)
			{
				_outputSink(value);
				return;
			}

			// Cast to char (not just u8) so std::format picks the character formatter: the
			// integer formatter's 'c' presentation additionally demands the value fit in a
			// *signed* char, which throws format_error and aborts the process for any byte
			// >= 0x80 — i.e. any accented letter or multi-byte UTF-8 sequence.
			std::print("{:c}", static_cast<char>(value));
		}

		void blockRead(Address ramAddress, u32 size)
		{
			if (size == 0)
			{
				_blockReadCount = 0;
				return;
			}

			const std::lock_guard lock{_inputMutex};
			// Clamp to RAM the store is allowed to touch: a BLOCK_ADDR/BLOCK_LEN past the end of
			// memory (or into a protected segment) moves fewer bytes rather than throwing.
			const u32 clampSize = memory().clampBlockSize(ramAddress, size);
			if (clampSize == 0)
			{
				_blockReadCount = 0;
				return;
			}

			auto buffer = memory().peekMutBytes(ramAddress, clampSize);
			usize bytesRead = 0;
			while (bytesRead < buffer.size())
			{
				usize currentHead = _head.load(std::memory_order_relaxed);
				if (currentHead == _tail.load(std::memory_order_acquire))
					break; // No more input available.

				buffer[bytesRead] = _buffer[currentHead];
				_head.store((currentHead + 1) % InputBufferCapacity, std::memory_order_release);
				++bytesRead;
			}

			// A short read - fewer bytes buffered than asked for - is now observable: the program
			// reads this back afterwards and knows exactly where its input ended.
			_blockReadCount = static_cast<u32>(bytesRead);
		}

		void blockWrite(Address ramAddress, u32 size)
		{
			if (size == 0)
				return;

			const u32 clampSize = memory().clampBlockSize(ramAddress, size);
			if (clampSize == 0)
				return;

			const auto buffer = memory().peekBytes(ramAddress, clampSize);
			for (u32 i = 0; i < buffer.size(); ++i)
				emitByte(buffer[i]);
		}

	public:
		u8 readUnsignedByte(Address offset) override
		{
			if (offset == StatusRegister)
			{
				u8 status = 0;
				if (_head.load(std::memory_order_acquire) != _tail.load(std::memory_order_acquire))
					status |= RxReadyMask; // Set RxReady if input is available.
				else if (_inputClosed.load(std::memory_order_acquire))
					status |= EofMask; // Closed and drained: nothing more will ever arrive.
				status |= TxReadyMask; // Terminal is always ready to accept output.
				return status;
			}

			if (offset == InputRegister)
			{
				const std::lock_guard lock{_inputMutex};
				usize currentHead = _head.load(std::memory_order_relaxed);
				if (currentHead == _tail.load(std::memory_order_acquire))
					return 0; // No input available, return 0.

				u8 value = _buffer[currentHead];
				_head.store((currentHead + 1) % InputBufferCapacity, std::memory_order_release);
				return value;
			}

			return 0xFF; // Undefined register.
		}
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedByte(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		u32 readUnsignedWord(Address offset) override
		{
			if (offset == BytesAvailableRegister)
				return static_cast<u32>(availableBytes());
			if (offset == BlockReadCountRegister)
				return _blockReadCount;
			if (offset == DroppedInputRegister)
				return static_cast<u32>(_droppedInputBytes.load(std::memory_order_relaxed));
			if (offset == ModeRegister)
				return _modeGranted.load(std::memory_order_acquire);
			return static_cast<u32>(readUnsignedByte(offset));
		}

		void writeByte(Address offset, u8 value) override
		{
			if (offset == OutputRegister)
				emitByte(value);
		}
		void writeHalfword(Address offset, u16 value) override
		{
			if (offset == OutputRegister)
			{
				emitByte(static_cast<u8>(value & 0xFF)); // Output the lower byte as a character.
				emitByte(static_cast<u8>((value >> 8) & 0xFF)); // Output the upper byte as a character.
			}
		}
		void writeWord(Address offset, u32 value) override
		{
			if (offset == OutputRegister)
			{
				emitByte(static_cast<u8>(value & 0xFF));
				emitByte(static_cast<u8>((value >> 8) & 0xFF));
				emitByte(static_cast<u8>((value >> 16) & 0xFF));
				emitByte(static_cast<u8>((value >> 24) & 0xFF));
				return;
			}

			if (offset == ModeRegister)
			{
				const u32 requested = value & ModeRaw;
				_modeRequested.store(requested, std::memory_order_release);
				_modeGranted.store(_modeHandler ? _modeHandler(requested) : 0u, std::memory_order_release);
				return;
			}

			// The block trio: two plain registers latched here, and a command that fires the transfer.
			if (offset == BlockAddressRegister) { _blockAddress = value; return; }
			if (offset == BlockLengthRegister) { _blockLength = value; return; }
			if (offset == BlockCommandRegister)
			{
				if (value == BlockCommandRead)
					blockRead(Address(_blockAddress), _blockLength);
				else if (value == BlockCommandWrite)
					blockWrite(Address(_blockAddress), _blockLength);
			}
		}
	};

	// A real DMA engine, not the pseudo-DMA the port opcodes used to be: SRC/DST/LEN/CMD are
	// ordinary registers, and completion is a tick later rather than instantaneous - modelled the
	// same way TimerDevice already models a delay, so a program can either poll STATUS or wait for
	// the interrupt. It moves memory the VM already knows how to move
	// (Memory::copyBytesUnchecked): RAM to RAM today, and RAM to or from a device's own MMIO window
	// once a device chooses to expose one, since both are just addresses in the same space.
	class DmaController : public IODevice
	{
	public:
		static inline constexpr Address SourceRegister = Address(0x00);      // Write: source physical address
		static inline constexpr Address DestinationRegister = Address(0x04); // Write: destination physical address
		static inline constexpr Address LengthRegister = Address(0x08);      // Write: bytes to move
		static inline constexpr Address CommandRegister = Address(0x0C);     // Write: 1 starts the transfer latched above
		static inline constexpr Address StatusRegister = Address(0x10);      // Read: Busy / Done bits
		static inline constexpr Address TransferredRegister = Address(0x14); // Read: bytes the last completed transfer actually moved

		static inline constexpr u32 CommandStart = 1;
		static inline constexpr u32 StatusBusy = 1u << 0;
		static inline constexpr u32 StatusDone = 1u << 1;

		// Third user interrupt: UserInterrupt0 is the timer's, UserInterrupt1 the terminal's.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt2;

	private:
		u32 _source = 0;
		u32 _destination = 0;
		u32 _length = 0;
		u32 _status = 0;
		u32 _transferred = 0;
		bool _pending = false;

	public:
		DmaController() = default;
		DmaController(const DmaController&) = delete;
		DmaController(DmaController&&) = delete;
		~DmaController() override = default;

		DmaController& operator=(const DmaController&) = delete;
		DmaController& operator=(DmaController&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Dma, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Dma);
		}

	public:
		// A transfer lands on the tick after it was armed.
		u64 ticksUntilEvent() const noexcept override { return _pending ? 1 : vm::NoDeviceEvent; }

		void advance(u64 ticks) override
		{
			if (ticks != 0)
				tick();
		}

		// A reset drops a transfer that was armed but has not landed yet.
		void reset() override
		{
			_pending = false;
			_status = 0;
			_transferred = 0;
		}

		// Must be unconditionally true, not "return _pending": MmioBus only re-reads needsTick() when
		// the topology changes (attach/detach), not every instruction, so a device that flipped this
		// on the fly could arm a transfer that then never sees the tick() that lands it.
		bool needsTick() const noexcept override { return true; }

		// Arms on the CMD write; the actual copy happens on the next tick(), one instruction later -
		// never on the same step that requested it, so a program relying on the interrupt (rather
		// than busy-polling STATUS) always sees a real handoff instead of an already-finished copy.
		void tick() override
		{
			if (!_pending)
				return;

			// A SRC/DST/LEN that runs past the end of memory is clamped rather than fatal: the
			// copy moves what fits, and TransferredRegister reports exactly how much.
			const u32 effective = std::min(
				memory().clampBlockSizeUnchecked(Address(_source), _length),
				memory().clampBlockSizeUnchecked(Address(_destination), _length));

			memory().copyBytesUnchecked(Address(_source), Address(_destination), effective);
			// RAM to RAM always moves the whole length, but a source device that yields fewer
			// bytes (a short terminal read) would land a smaller number here - which is exactly
			// what this register exists to report.
			_transferred = effective;
			_pending = false;
			_status = StatusDone;
			raiseInterrupt(Interrupt);
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister)
				return _status;
			if (offset == TransferredRegister)
				return _transferred;
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == SourceRegister) { _source = value; return; }
			if (offset == DestinationRegister) { _destination = value; return; }
			if (offset == LengthRegister) { _length = value; return; }
			if (offset == CommandRegister && value == CommandStart)
			{
				_pending = true;
				_status = StatusBusy;
			}
		}
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}
