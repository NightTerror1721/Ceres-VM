// The event scheduler (plan/v2 SPEC 3.3): on its own, and serviced by the engine between instructions.
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	// Remembers every event it is called with, as (tag, cycle, clock when called).
	class Recorder : public IODevice
	{
	public:
		struct Call
		{
			u32 tag;
			u64 cycle;
			u64 now;
		};
		std::vector<Call> calls;
		Scheduler* events = nullptr;
		u64 period = 0;                  // re-schedules tag 0 this many cycles after it came due, when not 0

		u32 read(Address) override { return 0; }
		void write(Address, u32) override {}
		const RegisterMap& registers() const override
		{
			static constexpr RegisterMap map{ "recorder", {} };
			return map;
		}

		void onEvent(u32 tag, u64 cycle) override
		{
			calls.push_back(Call{ tag, cycle, events != nullptr ? events->now() : 0 });
			if (tag == 0 && period != 0 && events != nullptr)
				events->schedule(*this, cycle + period, 0);
		}

		Scheduler* schedulerOfBus() noexcept { return scheduler(); }
	};

	void loadNops(CeresVM& vm, usize count)
	{
		const Address entry = Memory::UnrestrictedSegmentStart;
		for (usize i = 0; i < count; ++i)
			vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(i * Instruction::Size)), Instruction::NOP().raw());
		vm.memory().writeUnchecked<u32>(0_addr, entry.value());
		vm.engine().reset();
	}
}

TEST(scheduler, an_empty_scheduler_has_nothing_next)
{
	Scheduler scheduler;
	CHECK(scheduler.empty());
	CHECK_EQ(scheduler.nextCycle(), NoScheduledEvent);
	scheduler.service(1'000'000);               // nothing to call
}

TEST(scheduler, events_run_in_cycle_order_once_due)
{
	Scheduler scheduler;
	Recorder a;
	Recorder b;
	scheduler.schedule(a, 30, 1);
	scheduler.schedule(b, 10, 2);
	scheduler.schedule(a, 20, 3);
	CHECK_EQ(scheduler.nextCycle(), u64{ 10 });

	scheduler.service(9);
	CHECK(a.calls.empty() && b.calls.empty());

	scheduler.service(25);
	CHECK_EQ(b.calls.size(), usize{ 1 });
	CHECK_EQ(b.calls[0].tag, 2u);
	CHECK_EQ(a.calls.size(), usize{ 1 });
	CHECK_EQ(a.calls[0].tag, 3u);
	CHECK_EQ(a.calls[0].cycle, u64{ 20 });
	CHECK_EQ(scheduler.nextCycle(), u64{ 30 });
}

TEST(scheduler, scheduling_a_tag_again_moves_it)
{
	Scheduler scheduler;
	Recorder device;
	scheduler.schedule(device, 50, 0);
	scheduler.schedule(device, 70, 0);
	CHECK_EQ(scheduler.cycleOf(device, 0), u64{ 70 });
	CHECK_EQ(scheduler.nextCycle(), u64{ 70 });
	scheduler.service(100);
	CHECK_EQ(device.calls.size(), usize{ 1 });
}

TEST(scheduler, cancel_drops_one_event_and_cancel_all_every_one)
{
	Scheduler scheduler;
	Recorder device;
	Recorder other;
	scheduler.schedule(device, 5, 0);
	scheduler.schedule(device, 6, 1);
	scheduler.schedule(other, 7, 0);

	scheduler.cancel(device, 0);
	CHECK_EQ(scheduler.cycleOf(device, 0), NoScheduledEvent);
	CHECK_EQ(scheduler.nextCycle(), u64{ 6 });

	scheduler.cancelAll(device);
	CHECK_EQ(scheduler.nextCycle(), u64{ 7 });
	scheduler.service(10);
	CHECK(device.calls.empty());
	CHECK_EQ(other.calls.size(), usize{ 1 });
}

TEST(scheduler, an_event_scheduled_from_its_own_handler_runs_when_due)
{
	// A periodic source re-arms from the cycle it was due on: serviced late, it catches up in the same call
	// and its period never drifts.
	Scheduler scheduler;
	Recorder device;
	device.events = &scheduler;
	device.period = 10;
	scheduler.schedule(device, 10, 0);
	scheduler.service(35);
	CHECK_EQ(device.calls.size(), usize{ 3 });
	CHECK_EQ(device.calls[2].cycle, u64{ 30 });
	CHECK_EQ(scheduler.nextCycle(), u64{ 40 });
}

TEST(scheduler, the_engine_services_an_event_before_the_instruction_after_the_one_that_reaches_it)
{
	CeresVM vm;
	loadNops(vm, 8);
	Recorder device;
	vm.io().attach(MmioBus::slot(0x80), device);
	device.events = device.schedulerOfBus();
	CHECK(device.events == &vm.io().scheduler());

	device.events->schedule(device, 3, 0);
	for (int i = 0; i < 3; ++i)                 // the third nop reaches cycle 3
		vm.engine().step();
	CHECK(device.calls.empty());
	vm.engine().step();                         // the event runs first, so what it raises is taken on this step
	CHECK_EQ(device.calls.size(), usize{ 1 });
	CHECK_EQ(device.calls[0].now, u64{ 3 });
	vm.io().detach(MmioBus::slot(0x80));
}

TEST(scheduler, detaching_a_device_drops_its_events)
{
	CeresVM vm;
	loadNops(vm, 4);
	Recorder device;
	vm.io().attach(MmioBus::slot(0x80), device);
	vm.io().scheduler().schedule(device, 2, 0);
	vm.io().detach(MmioBus::slot(0x80));
	CHECK(vm.io().scheduler().empty());
	for (int i = 0; i < 4; ++i)
		vm.engine().step();
	CHECK(device.calls.empty());
}

TEST(scheduler, a_halted_machine_with_the_clock_off_jumps_to_the_event)
{
	CeresVM vm;
	const Address entry = Memory::UnrestrictedSegmentStart;
	vm.memory().writeUnchecked<u32>(entry, Instruction::HALT().raw());
	vm.memory().writeUnchecked<u32>(0_addr, entry.value());
	vm.engine().reset();
	vm.engine().setHaltClock(0);
	Recorder device;
	vm.io().attach(MmioBus::slot(0x80), device);
	vm.io().scheduler().schedule(device, 5'000'000, 0);

	vm.engine().step();                         // HALT
	CHECK(vm.engine().isHalted());
	vm.engine().step();                         // one halted step: straight to the event
	CHECK_EQ(device.calls.size(), usize{ 1 });
	CHECK_EQ(vm.engine().cycles(), u64{ 5'000'000 });
	vm.io().detach(MmioBus::slot(0x80));
}
