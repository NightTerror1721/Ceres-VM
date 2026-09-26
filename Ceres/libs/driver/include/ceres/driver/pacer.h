#pragma once

#include <ceres/core/base/types.h>
#include <chrono>
#include <functional>
#include <optional>
#include <string_view>

// Keeping the machine's time in step with the host's (plan/v2 SPEC 3.3). The machine counts its own cycles and
// never waits for anything; the runner asks the pacer, between slices, whether the host has caught up with
// them, and sleeps while it has not.
namespace ceres::driver
{
	// How the machine's time relates to the host's: --speed realtime (1x), max (no waiting at all), or <f>x.
	struct Speed
	{
		bool max = false;
		double factor = 1.0;     // machine seconds per host second, when not max

		static Speed realtime() noexcept { return Speed{}; }
		static Speed unlimited() noexcept { return Speed{ true, 1.0 }; }
		// "realtime", "max", or a positive factor followed by x ("0.5x", "2x"); nullopt for anything else.
		static std::optional<Speed> parse(std::string_view text) noexcept;

		friend bool operator==(const Speed&, const Speed&) = default;
	};

	class Pacer
	{
	public:
		using Clock = std::chrono::steady_clock;
		using NowFunction = std::function<Clock::time_point()>;
		using SleepFunction = std::function<void(Clock::time_point)>;

		// The longest one call sleeps, so the loop around it can still pump the host's input and see a close.
		static constexpr auto MaxSleep = std::chrono::milliseconds(10);
		// How far the machine may fall behind before the pacer stops trying to catch it up: time the host did
		// not give it (a slow host, a machine waiting for a key) is let go rather than raced through later.
		static constexpr auto MaxLag = std::chrono::milliseconds(250);
		// How long the effective speed is measured over.
		static constexpr auto SpeedWindow = std::chrono::milliseconds(500);

	private:
		Speed _speed;
		u64 _clockHz;
		NowFunction _now;
		SleepFunction _sleepUntil;
		Clock::time_point _realAnchor;
		u64 _cycleAnchor = 0;
		u64 _lastCycles = 0;     // the count at the last call, to see it go backwards
		bool _anchored = false;
		Clock::time_point _windowStart;
		u64 _windowCycles = 0;
		double _effective = 0.0;

		void anchor(Clock::time_point now, u64 cycles) noexcept;
		// When the host should reach `cycles`, by the anchor.
		Clock::time_point dueAt(u64 cycles) const noexcept;

	public:
		// `now` and `sleepUntil` default to the steady clock and a real sleep; a test hands in its own.
		Pacer(Speed speed, u64 clockHz, NowFunction now = {}, SleepFunction sleepUntil = {});

		const Speed& speed() const noexcept { return _speed; }
		// A new speed takes effect from where the machine is now, not from where the pacing started.
		void setSpeed(Speed speed) noexcept
		{
			if (speed == _speed)
				return;
			_speed = speed;
			_anchored = false;
		}

		// Between two slices: the machine has reached `cycles`. Sleeps while it is ahead of the host, at most
		// MaxSleep, and returns true if it still is - the caller pumps the host and asks again before running on.
		// Under max it never sleeps. A count that went backwards (a reset) starts the pacing again.
		bool pace(u64 cycles);

		// Machine seconds per host second over the last measurement window: 1 when a realtime machine keeps up,
		// less when the host is too slow, more under max. 0 until the first window closes.
		double effectiveSpeed() const noexcept { return _effective; }
	};
}
