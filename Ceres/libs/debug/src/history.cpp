#include <ceres/debug/history.h>

#include <ceres/vm/ceresvm.h>
#include <algorithm>
#include <cstring>

namespace ceres::debug
{
	void History::clear()
	{
		_enabled = false;
		_base.clear();
		_base.shrink_to_fit();
		_snapshots.clear();
		_clockReads.clear();
		_inputs.clear();
		_replaying = false;
		_nextClockRead = 0;
	}

	void History::start(const vm::CeresVM& machine, const TimerDevice& timer, const TerminalDevice& terminal)
	{
		clear();
		_enabled = true;

		const vm::Memory& memory = machine.memory();
		const auto bytes = memory.peekBytesUnchecked(vm::Address(0), static_cast<u32>(memory.size()));
		_base.assign(bytes.begin(), bytes.end());

		// The first snapshot is the program as loaded, so the oldest reachable moment is before
		// anything has run.
		maybeSnapshot(machine, timer, terminal, 0);
	}

	bool History::maybeSnapshot(const CeresVM& machine, const TimerDevice& timer,
		const TerminalDevice& terminal, u64 tick)
	{
		if (!_enabled || _replaying)
			return false;

		if (!_snapshots.empty())
		{
			const u64 last = _snapshots.back().tick;
			if (tick < last + _settings.interval)
				return false;
		}

		Snapshot snapshot;
		snapshot.tick = tick;
		snapshot.registers = machine.engine().registers();
		snapshot.fregisters = machine.engine().fregisters();
		snapshot.flags = machine.engine().flags().value();
		snapshot.programCounter = machine.engine().programCounter().value();
		snapshot.pendingInterrupts = const_cast<vm::CeresVM&>(machine).interrupts().pendingMask();
		snapshot.timer = timer.captureState();
		snapshot.terminal = terminal.captureState();

		const vm::Memory& memory = machine.memory();
		const auto bytes = memory.peekBytesUnchecked(vm::Address(0), static_cast<u32>(memory.size()));
		const usize pages = (bytes.size() + PageSize - 1) / PageSize;

		for (usize page = 0; page < pages; ++page)
		{
			const usize offset = page * PageSize;
			const usize length = std::min<usize>(PageSize, bytes.size() - offset);

			// Only what differs from the base image. A program's working set is a few pages out
			// of four thousand, which is what makes keeping sixty-four of these affordable.
			if (std::memcmp(bytes.data() + offset, _base.data() + offset, length) == 0)
				continue;

			snapshot.dirtyPages.emplace(
				static_cast<u32>(page),
				std::vector<u8>(bytes.begin() + offset, bytes.begin() + offset + length));
		}

		_snapshots.push_back(std::move(snapshot));

		while (_snapshots.size() > _settings.maxSnapshots)
			_snapshots.pop_front();

		return true;
	}

	std::optional<u64> History::restoreNearest(u64 tick, CeresVM& machine, TimerDevice& timer, TerminalDevice& terminal)
	{
		if (!_enabled || _snapshots.empty())
			return std::nullopt;

		// The newest snapshot at or before the moment asked for, so the replay that follows is as
		// short as the history allows.
		const Snapshot* chosen = nullptr;
		for (const Snapshot& snapshot : _snapshots)
		{
			if (snapshot.tick <= tick)
				chosen = &snapshot;
			else
				break;
		}

		if (chosen == nullptr)
			return std::nullopt;

		vm::Memory& memory = machine.memory();
		// Base first, then the pages that differed: anything the program has written since is
		// undone by the base copy, and anything it had written by the snapshot is put back.
		memory.writeBytesUnchecked(vm::Address(0), _base);
		for (const auto& [page, contents] : chosen->dirtyPages)
			memory.writeBytesUnchecked(vm::Address(page * PageSize), contents);

		vm::ExecutionEngine& engine = machine.engine();
		for (usize i = 0; i < vm::GeneralPurposeRegisterPool::Count; ++i)
			engine.setRegister(static_cast<u8>(i), chosen->registers.getValue(i));
		for (usize i = 0; i < vm::FloatingPointRegisterPool::Count; ++i)
			engine.setFloatRegister(static_cast<u8>(i), chosen->fregisters.getValue(i));

		engine.setFlags(vm::FlagRegister{ chosen->flags });
		engine.setProgramCounter(vm::Address(chosen->programCounter));
		engine.setExecutedInstructions(chosen->tick);

		machine.interrupts().restorePendingMask(chosen->pendingInterrupts);
		timer.restoreState(chosen->timer);
		terminal.restoreState(chosen->terminal);

		rewindClockCursor(chosen->tick);
		return chosen->tick;
	}

	u32 History::clockValue(u64 tick, u32 liveValue)
	{
		if (!_enabled)
			return liveValue;

		if (_replaying)
		{
			// Served from the recording, so a program that asks the time twice on two passes
			// through the same instruction gets the same answer both times.
			while (_nextClockRead < _clockReads.size() && _clockReads[_nextClockRead].first < tick)
				++_nextClockRead;

			if (_nextClockRead < _clockReads.size() && _clockReads[_nextClockRead].first == tick)
				return _clockReads[_nextClockRead++].second;

			// Nothing recorded for this moment, which means the replay has diverged. The last
			// known value is a better answer than a fresh one: it at least stays put.
			return _clockReads.empty() ? liveValue : _clockReads.back().second;
		}

		_clockReads.emplace_back(tick, liveValue);
		return liveValue;
	}

	void History::rewindClockCursor(u64 tick)
	{
		_nextClockRead = 0;
		while (_nextClockRead < _clockReads.size() && _clockReads[_nextClockRead].first < tick)
			++_nextClockRead;
	}

	void History::recordInput(u64 tick, std::string_view text)
	{
		if (_enabled && !_replaying)
			_inputs.emplace_back(tick, std::string(text));
	}

	std::vector<std::string> History::inputsBetween(u64 fromTick, u64 toTick) const
	{
		std::vector<std::string> out;
		for (const auto& [tick, text] : _inputs)
		{
			if (tick >= fromTick && tick < toTick)
				out.push_back(text);
		}
		return out;
	}

	usize History::memoryCost() const noexcept
	{
		usize total = _base.size();
		for (const Snapshot& snapshot : _snapshots)
		{
			for (const auto& [page, contents] : snapshot.dirtyPages)
				total += contents.size();
		}
		return total;
	}
}
