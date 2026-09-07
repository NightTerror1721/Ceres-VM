#pragma once

// Running the machine backwards.
//
// Nothing in the machine can undo an instruction, so "back" means "start again from a snapshot and
// stop earlier". That works here, where it would not on real hardware, because the machine is
// deterministic by construction: the timer counts executed instructions rather than wall clock, so
// a program behaves identically on every run. Two things are not deterministic - the real-time
// clock port and whatever the user types - and both are recorded as they happen and replayed from
// the recording.
//
// Snapshots are stored as the memory pages that differ from a base image taken at the start. A
// program's working set is a few pages out of four thousand, so a snapshot costs kilobytes rather
// than the sixteen megabytes a full copy would.

#include "common/types.h"
#include "vm/devices.h"
#include "vm/registers.h"
#include "vm/fregisters.h"
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ceres::vm { class CeresVM; }

namespace ceres::debug
{
	class History
	{
	public:
		// Four kilobytes, the size at which comparing the whole of a 16 MiB machine costs a few
		// thousand memcmps and a typical program dirties a handful of entries.
		static inline constexpr u32 PageSize = 4096;

		struct Settings
		{
			// How many instructions between snapshots. Smaller means less replaying to reach a
			// given moment, and more snapshots to keep.
			u64 interval = 20'000;
			// How far back you can go: interval * snapshots instructions of history.
			usize maxSnapshots = 64;
		};

	private:
		struct Snapshot
		{
			u64 tick = 0;
			vm::GeneralPurposeRegisterPool registers;
			vm::FloatingPointRegisterPool fregisters;
			u32 flags = 0;
			u32 programCounter = 0;
			u64 pendingInterrupts = 0;
			vm::TimerDevice::State timer;
			vm::TerminalDevice::State terminal;
			// Page index to contents, for every page that differs from the base image.
			std::unordered_map<u32, std::vector<u8>> dirtyPages;
		};

		Settings _settings;
		bool _enabled = false;

		// The memory as it stood when recording began. Every snapshot is a delta against this one
		// rather than against its predecessor, so restoring never has to walk a chain.
		std::vector<u8> _base;
		std::deque<Snapshot> _snapshots;

		// The two things a replay cannot reproduce on its own, recorded against the tick they
		// happened at and served back from here while replaying.
		std::vector<std::pair<u64, u32>> _clockReads;
		std::vector<std::pair<u64, std::string>> _inputs;

		// Set while re-executing, so recording does not record its own replay and breakpoints do
		// not fire on ground already covered.
		bool _replaying = false;
		usize _nextClockRead = 0;

	public:
		History() = default;
		History(const History&) = delete;
		History(History&&) = default;
		~History() = default;

		History& operator=(const History&) = delete;
		History& operator=(History&&) = default;

	public:
		void configure(const Settings& settings) noexcept { _settings = settings; }
		const Settings& settings() const noexcept { return _settings; }

		bool isEnabled() const noexcept { return _enabled; }
		bool isReplaying() const noexcept { return _replaying; }
		void beginReplay() noexcept { _replaying = true; }
		void endReplay() noexcept { _replaying = false; }

		// Starts recording, taking the base image and the first snapshot. Called once the machine
		// is loaded and before anything runs.
		void start(const vm::CeresVM& machine, const vm::TimerDevice& timer, const vm::TerminalDevice& terminal);
		void clear();

		// Takes a snapshot if enough instructions have gone by since the last one. Cheap when it
		// decides not to: one comparison.
		// True when it actually took one, so a caller keeping anything alongside the snapshots -
		// the debugger keeps the reconstructed call stack - knows to record its own.
		bool maybeSnapshot(const vm::CeresVM& machine, const vm::TimerDevice& timer,
			const vm::TerminalDevice& terminal, u64 tick);

		// The newest snapshot at or before `tick`, or nothing when the moment asked for is older
		// than the oldest snapshot still kept.
		std::optional<u64> restoreNearest(u64 tick, vm::CeresVM& machine, vm::TimerDevice& timer,
			vm::TerminalDevice& terminal);

		u64 oldestTick() const noexcept { return _snapshots.empty() ? 0 : _snapshots.front().tick; }
		usize snapshotCount() const noexcept { return _snapshots.size(); }
		bool canReach(u64 tick) const noexcept { return _enabled && !_snapshots.empty() && tick >= oldestTick(); }

	public:
		// The real-time clock, recorded on the way forward and served back on the way through
		// again, so a program that reads it sees the same second both times.
		u32 clockValue(u64 tick, u32 liveValue);
		void recordInput(u64 tick, std::string_view text);
		// Input that was typed between `fromTick` and `toTick`, to be pushed again during replay.
		std::vector<std::string> inputsBetween(u64 fromTick, u64 toTick) const;
		void rewindClockCursor(u64 tick);

		usize memoryCost() const noexcept;
	};
}
