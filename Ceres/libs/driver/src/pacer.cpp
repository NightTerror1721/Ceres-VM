#include <ceres/driver/pacer.h>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

namespace ceres::driver
{
	std::optional<Speed> Speed::parse(std::string_view text) noexcept
	{
		if (text == "realtime")
			return realtime();
		if (text == "max")
			return unlimited();
		if (text.size() < 2 || text.back() != 'x')
			return std::nullopt;
		// std::from_chars for double is not in every standard library this builds with: strtod on a copy.
		const std::string number(text.substr(0, text.size() - 1));
		char* end = nullptr;
		const double factor = std::strtod(number.c_str(), &end);
		if (end != number.c_str() + number.size() || !std::isfinite(factor) || factor <= 0.0 || factor > 1'000'000.0)
			return std::nullopt;
		return Speed{ false, factor };
	}

	Pacer::Pacer(Speed speed, u64 clockHz, NowFunction now, SleepFunction sleepUntil) :
		_speed(speed),
		_clockHz(clockHz == 0 ? 1 : clockHz),
		_now(now ? std::move(now) : NowFunction{ [] { return Clock::now(); } }),
		_sleepUntil(sleepUntil ? std::move(sleepUntil) : SleepFunction{ [](Clock::time_point until) { std::this_thread::sleep_until(until); } })
	{}

	void Pacer::anchor(Clock::time_point now, u64 cycles) noexcept
	{
		_realAnchor = now;
		_cycleAnchor = cycles;
		_anchored = true;
	}

	Pacer::Clock::time_point Pacer::dueAt(u64 cycles) const noexcept
	{
		const double seconds = static_cast<double>(cycles - _cycleAnchor) / static_cast<double>(_clockHz) / _speed.factor;
		return _realAnchor + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
	}

	bool Pacer::pace(u64 cycles)
	{
		const Clock::time_point now = _now();
		if (!_anchored || cycles < _lastCycles)
		{
			anchor(now, cycles);
			_windowStart = now;
			_windowCycles = cycles;
		}
		_lastCycles = cycles;

		if (now - _windowStart >= SpeedWindow)
		{
			const double real = std::chrono::duration<double>(now - _windowStart).count();
			_effective = static_cast<double>(cycles - _windowCycles) / static_cast<double>(_clockHz) / real;
			_windowStart = now;
			_windowCycles = cycles;
		}

		if (_speed.max)
			return false;

		const Clock::time_point due = dueAt(cycles);
		if (due <= now)
		{
			if (now - due > MaxLag)
				anchor(now, cycles);   // the host fell behind: let the lost time go
			return false;
		}
		const Clock::time_point wake = due - now > MaxSleep ? now + MaxSleep : due;
		_sleepUntil(wake);
		return _now() < due;
	}
}
