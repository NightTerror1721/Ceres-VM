#include <ceres/vm/scheduler.h>
#include <ceres/vm/io_device.h>

namespace ceres::vm
{
	void Scheduler::recomputeNext() noexcept
	{
		u64 next = NoScheduledEvent;
		for (usize i = 0; i < _count; ++i)
			if (_events[i].cycle < next)
				next = _events[i].cycle;
		*_next = next;
	}

	void Scheduler::schedule(IODevice& device, u64 cycle, u32 tag) noexcept
	{
		for (usize i = 0; i < _count; ++i)
		{
			if (_events[i].device == &device && _events[i].tag == tag)
			{
				_events[i].cycle = cycle;
				recomputeNext();
				return;
			}
		}
		// The table holds a few events per device and there are at most a handful of devices that keep time;
		// running out of room is a bug in the machine, not something a program can cause.
		if (_count == MaxEvents)
			return;
		_events[_count++] = Event{ &device, cycle, tag };
		if (cycle < *_next)
			*_next = cycle;
	}

	void Scheduler::cancel(IODevice& device, u32 tag) noexcept
	{
		for (usize i = 0; i < _count; ++i)
		{
			if (_events[i].device == &device && _events[i].tag == tag)
			{
				_events[i] = _events[--_count];
				recomputeNext();
				return;
			}
		}
	}

	void Scheduler::cancelAll(IODevice& device) noexcept
	{
		for (usize i = 0; i < _count;)
		{
			if (_events[i].device == &device)
				_events[i] = _events[--_count];
			else
				++i;
		}
		recomputeNext();
	}

	void Scheduler::restoreEvents(std::span<const Event> events) noexcept
	{
		_count = events.size() < MaxEvents ? events.size() : MaxEvents;
		for (usize i = 0; i < _count; ++i)
			_events[i] = events[i];
		recomputeNext();
	}

	u64 Scheduler::cycleOf(const IODevice& device, u32 tag) const noexcept
	{
		for (usize i = 0; i < _count; ++i)
			if (_events[i].device == &device && _events[i].tag == tag)
				return _events[i].cycle;
		return NoScheduledEvent;
	}

	void Scheduler::service(u64 now)
	{
		while (*_next <= now)
		{
			// The earliest due event; of two due on the same cycle, the one the table holds first - the same one
			// on every run, since the table only changes by what the machine does.
			usize earliest = 0;
			for (usize i = 1; i < _count; ++i)
				if (_events[i].cycle < _events[earliest].cycle)
					earliest = i;

			const Event event = _events[earliest];
			_events[earliest] = _events[--_count];
			recomputeNext();
			event.device->onEvent(event.tag, event.cycle);
		}
	}
}
