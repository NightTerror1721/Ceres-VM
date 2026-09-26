// The pacer (plan/v2 SPEC 3.3): how the machine's time is kept in step with the host's, on a fake clock.
#include "framework.h"
#include <ceres/driver/pacer.h>
#include <vector>

using namespace ceres;
using namespace ceres::driver;
using namespace std::chrono_literals;

namespace
{
	// A host clock that only moves when the pacer sleeps on it, or when the test says so.
	struct FakeHost
	{
		Pacer::Clock::time_point now{};
		std::vector<Pacer::Clock::duration> sleeps;

		Pacer make(Speed speed, u64 clockHz)
		{
			return Pacer{ speed, clockHz, [this] { return now; }, [this](Pacer::Clock::time_point until)
			{
				sleeps.push_back(until - now);
				now = until;
			} };
		}
	};

	constexpr u64 Hz = 1'000'000;   // a microsecond a cycle: easy arithmetic
}

TEST(speed, parses_realtime_max_and_factors)
{
	CHECK(Speed::parse("realtime") == Speed::realtime());
	CHECK(Speed::parse("max") == Speed::unlimited());
	const auto half = Speed::parse("0.5x");
	CHECK(half.has_value() && !half->max && half->factor == 0.5);
	const auto twice = Speed::parse("2x");
	CHECK(twice.has_value() && twice->factor == 2.0);
	for (const char* wrong : { "", "x", "fast", "0x", "-1x", "2", "1.5", "nanx", "2xx" })
		CHECK(!Speed::parse(wrong).has_value());
}

TEST(pacer, a_realtime_machine_ahead_of_the_host_waits_for_it)
{
	FakeHost host;
	Pacer pacer = host.make(Speed::realtime(), Hz);
	CHECK(!pacer.pace(0));                      // the first call anchors: nothing to wait for
	CHECK(pacer.pace(25'000));                  // 25 ms of machine time at once: it sleeps 10 and is still ahead
	CHECK(host.sleeps.back() == Pacer::MaxSleep);
	CHECK(pacer.pace(25'000));
	CHECK(!pacer.pace(25'000));                 // the last 5 ms, and the host has caught up
	CHECK(host.now - Pacer::Clock::time_point{} == 25ms);
}

TEST(pacer, a_machine_behind_the_host_does_not_wait)
{
	FakeHost host;
	Pacer pacer = host.make(Speed::realtime(), Hz);
	pacer.pace(0);
	host.now += 20ms;
	CHECK(!pacer.pace(10'000));                 // 10 ms of machine time in 20 of host time
	CHECK(host.sleeps.empty());
}

TEST(pacer, max_never_waits)
{
	FakeHost host;
	Pacer pacer = host.make(Speed::unlimited(), Hz);
	pacer.pace(0);
	CHECK(!pacer.pace(10'000'000));             // ten seconds ahead
	CHECK(host.sleeps.empty());
}

TEST(pacer, a_factor_scales_the_hosts_time)
{
	FakeHost host;
	Pacer pacer = host.make(Speed{ false, 2.0 }, Hz);
	pacer.pace(0);
	while (pacer.pace(20'000))                  // 20 ms of machine time at 2x: 10 ms of the host's
	{
	}
	CHECK(host.now - Pacer::Clock::time_point{} == 10ms);
}

TEST(pacer, time_the_host_did_not_give_is_let_go)
{
	// The machine waited for a key for a second: it is not raced through afterwards to make up for it.
	FakeHost host;
	Pacer pacer = host.make(Speed::realtime(), Hz);
	pacer.pace(0);
	host.now += 1s;
	CHECK(!pacer.pace(1'000));                  // a millisecond of machine time a second later: far behind
	CHECK(host.sleeps.empty());
	CHECK(pacer.pace(21'000));                  // and from here it is paced again, from where it was let go
	CHECK(host.now - Pacer::Clock::time_point{} == 1s + 10ms);
}

TEST(pacer, a_reset_starts_the_pacing_again)
{
	FakeHost host;
	Pacer pacer = host.make(Speed::realtime(), Hz);
	pacer.pace(0);
	host.now += 5ms;
	pacer.pace(5'000);
	CHECK(!pacer.pace(1'000));                  // the count went back: a fresh anchor, nothing to wait for
	CHECK(host.sleeps.empty());
	CHECK(pacer.pace(21'000));                  // 20 ms from the new anchor
}

TEST(pacer, measures_the_effective_speed)
{
	FakeHost host;
	Pacer pacer = host.make(Speed::unlimited(), Hz);
	pacer.pace(0);
	CHECK(pacer.effectiveSpeed() == 0.0);
	host.now += 500ms;
	pacer.pace(2'000'000);                      // two machine seconds in half a host second
	CHECK(pacer.effectiveSpeed() == 4.0);
}
