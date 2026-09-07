#pragma once

// `ceres debug --server`: the same DebugSession, spoken as newline-delimited JSON over stdin and
// stdout, for an editor to drive.
//
// Deliberately *not* the Debug Adapter Protocol. This is a small vocabulary in the machine's own
// terms - addresses, registers, ports, ticks, sections - and the editor-side adapter translates.
// Keeping DAP's shape out of the C++ means the protocol stays something a script can speak too,
// and that the awkward parts of DAP (sequencing, reverse requests, capability negotiation) live
// where a library already handles them.
//
// The program's own output travels as events rather than being written to stdout directly, which
// it would otherwise corrupt: `ceres run` prints straight to the same stream.

#include "debug_session.h"
#include "json.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace ceres::debug
{
	class DebugServer
	{
	public:
		static inline constexpr u32 ProtocolVersion = 1;

	private:
		DebugSession& _session;

		// Requests arrive on a reader thread so that `pause` can be acted on while the main thread
		// is inside resume(); everything else is queued and handled in order.
		std::thread _reader;
		std::deque<json::Value> _pending;
		std::mutex _queueMutex;
		std::condition_variable _queueSignal;
		std::atomic<bool> _inputClosed{ false };
		std::atomic<bool> _stopping{ false };

		// stdout has two writers - the main thread's responses and events, and the reader thread's
		// acknowledgements - and a half-written line is not JSON.
		std::mutex _writeMutex;

		bool _configured = false;

	public:
		DebugServer() = delete;
		DebugServer(const DebugServer&) = delete;
		DebugServer(DebugServer&&) = delete;
		~DebugServer();

		DebugServer& operator=(const DebugServer&) = delete;
		DebugServer& operator=(DebugServer&&) = delete;

		explicit DebugServer(DebugSession& session) : _session(session) {}

	public:
		// Runs until the client disconnects or asks to terminate. Returns the process exit code.
		int run();

	private:
		void readLoop();
		bool dispatch(const json::Value& request);

		void emit(const json::Value& message);
		void emitEvent(std::string_view event, json::Object body);
		void respond(const json::Value& request, json::Object body);
		void fail(const json::Value& request, std::string_view message);

		void reportStop(const StopEvent& event);

		json::Object describeLocation(u32 address) const;
		json::Object describeStop(const StopEvent& event) const;
		json::Array describeFrames() const;
		json::Object describeRegisters() const;
		json::Array describeGlobals() const;
		json::Array describeBreakpoints() const;
	};
}
