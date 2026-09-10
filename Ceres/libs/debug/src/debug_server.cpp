#include <ceres/debug/debug_server.h>

#include <format>
#include <iostream>

namespace ceres::debug
{
	namespace
	{
		std::string hexEncode(std::span<const u8> bytes)
		{
			std::string out;
			out.reserve(bytes.size() * 2);
			for (u8 byte : bytes)
				out += std::format("{:02x}", byte);
			return out;
		}

		std::optional<std::vector<u8>> hexDecode(std::string_view text)
		{
			if (text.size() % 2 != 0)
				return std::nullopt;

			const auto digit = [](char c) -> std::optional<u8>
			{
				if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
				if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
				if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
				return std::nullopt;
			};

			std::vector<u8> out;
			out.reserve(text.size() / 2);
			for (usize i = 0; i < text.size(); i += 2)
			{
				const auto high = digit(text[i]);
				const auto low = digit(text[i + 1]);
				if (!high.has_value() || !low.has_value())
					return std::nullopt;
				out.push_back(static_cast<u8>((high.value() << 4) | low.value()));
			}
			return out;
		}
	}

	DebugServer::~DebugServer()
	{
		// getline cannot be cancelled portably. The shared control lets a detached reader wake later
		// without dereferencing this, while the mutex waits out any command it is currently handling.
		const std::lock_guard lock{ _readerControl->mutex };
		_readerControl->stopping = true;
		_stopping.store(true, std::memory_order_release);
		if (_reader.joinable())
		{
			// The reader may be blocked in stdin. It owns ReaderControl, not this, after returning
			// from getline, so detaching does not leave an object access behind.
			_reader.detach();
		}
	}

	// --- Writing --------------------------------------------------------------------------------

	void DebugServer::emit(const json::Value& message)
	{
		const std::lock_guard<std::mutex> lock{ _writeMutex };
		std::cout << message.serialize() << '\n' << std::flush;
	}

	void DebugServer::emitEvent(std::string_view event, json::Object body)
	{
		emit(json::Value(json::Object{
			{ "type", json::Value("event") },
			{ "event", json::Value(event) },
			{ "body", json::Value(std::move(body)) }
		}));
	}

	void DebugServer::respond(const json::Value& request, json::Object body)
	{
		emit(json::Value(json::Object{
			{ "type", json::Value("response") },
			{ "request_seq", json::Value(request["seq"].asU32()) },
			{ "command", json::Value(request["command"].asString()) },
			{ "success", json::Value(true) },
			{ "body", json::Value(std::move(body)) }
		}));
	}

	void DebugServer::fail(const json::Value& request, std::string_view message)
	{
		emit(json::Value(json::Object{
			{ "type", json::Value("response") },
			{ "request_seq", json::Value(request["seq"].asU32()) },
			{ "command", json::Value(request["command"].asString()) },
			{ "success", json::Value(false) },
			{ "message", json::Value(message) }
		}));
	}

	// --- Describing the machine -----------------------------------------------------------------

	json::Object DebugServer::describeLocation(u32 address) const
	{
		json::Object out{ { "address", json::Value(address) } };

		if (const auto location = _session.debugInfo().locationOf(address); location.has_value())
		{
			out.insert_or_assign("file", json::Value(location->expansionFile));
			out.insert_or_assign("line", json::Value(location->expansionLine));
			// Where it was *written*, which differs from the above only inside a macro expansion.
			out.insert_or_assign("sourceFile", json::Value(location->file));
			out.insert_or_assign("sourceLine", json::Value(location->line));
			out.insert_or_assign("macroDepth", json::Value(static_cast<u32>(location->macroDepth)));
			out.insert_or_assign("isPadding", json::Value(location->isPadding()));
		}

		return out;
	}

	json::Object DebugServer::describeStop(const StopEvent& event) const
	{
		json::Object body = describeLocation(event.address);
		body.insert_or_assign("reason", json::Value(describe(event.reason)));
		body.insert_or_assign("description", json::Value(event.message));

		if (event.reason == StopReason::Breakpoint)
			body.insert_or_assign("breakpointId", json::Value(event.breakpoint));

		if (event.reason == StopReason::DataBreakpoint)
			body.insert_or_assign("dataBreakpointId", json::Value(event.dataBreakpoint));

		if (event.reason == StopReason::Exception)
		{
			body.insert_or_assign("exception", json::Value(describe(event.exception)));
			body.insert_or_assign("exceptionAddress", json::Value(event.exceptionAddress));
			// The faulting instruction, which is not where the program counter now is.
			json::Object at = describeLocation(event.exceptionAddress);
			body.insert_or_assign("exceptionLocation", json::Value(std::move(at)));
		}

		return body;
	}

	json::Array DebugServer::describeFrames() const
	{
		json::Array frames;

		// Walked through the frame pointers where the assembler recorded that the function opens
		// one, and inferred from the calls gone past where it did not. The walk is exact, so it
		// wins whenever there is one; each frame says which it was, and an editor can show it.
		const std::vector<Frame> exact = _session.unwindCallStack();
		const std::span<const Frame> stack = exact.empty()
			? _session.callStack()
			: std::span<const Frame>(exact);

		// Innermost first, which is the order every editor expects to render. The reconstruction
		// grows outward, so it is walked backwards; the unwind already starts at the innermost.
		for (usize n = 0; n < stack.size(); ++n)
		{
			const usize i = exact.empty() ? stack.size() - 1 - n : n;
			const Frame& frame = stack[i];
			json::Object out = describeLocation(frame.address);
			out.insert_or_assign("id", json::Value(static_cast<u32>(n)));
			out.insert_or_assign("name", json::Value(frame.name));
			out.insert_or_assign("returnAddress", json::Value(frame.returnAddress));
			out.insert_or_assign("stackPointer", json::Value(frame.stackPointer));
			out.insert_or_assign("isInterruptHandler", json::Value(frame.isInterruptHandler));
			out.insert_or_assign("reconstructed", json::Value(frame.reconstructed));
			frames.push_back(json::Value(std::move(out)));
		}

		return frames;
	}

	json::Object DebugServer::describeRegisters() const
	{
		const RegisterView view = _session.registers();

		json::Array general;
		for (u32 value : view.general)
			general.push_back(json::Value(value));

		json::Array floating;
		for (f32 value : view.floating)
			floating.push_back(json::Value(static_cast<double>(value)));

		return json::Object{
			{ "general", json::Value(std::move(general)) },
			{ "floating", json::Value(std::move(floating)) },
			{ "pc", json::Value(view.programCounter) },
			{ "flags", json::Value(view.flags) },
			{ "ticks", json::Value(view.executedInstructions) },
			{ "zero", json::Value(view.zero) },
			{ "sign", json::Value(view.sign) },
			{ "carry", json::Value(view.carry) },
			{ "overflow", json::Value(view.overflow) },
			{ "interrupt", json::Value(view.interruptEnabled) },
			{ "halting", json::Value(view.halting) },
			{ "trap", json::Value(view.trap) }
		};
	}

	json::Array DebugServer::describeGlobals() const
	{
		json::Array out;
		for (const VariableView& variable : _session.globals())
		{
			out.push_back(json::Value(json::Object{
				{ "name", json::Value(variable.name) },
				{ "type", json::Value(variable.type) },
				{ "value", json::Value(variable.value) },
				{ "address", json::Value(variable.address) },
				{ "size", json::Value(variable.size) },
				{ "isConstant", json::Value(variable.isConstant) }
			}));
		}
		return out;
	}

	json::Array DebugServer::describeBreakpoints() const
	{
		json::Array out;
		for (const Breakpoint& breakpoint : _session.breakpoints())
		{
			out.push_back(json::Value(json::Object{
				{ "id", json::Value(breakpoint.id) },
				{ "verified", json::Value(breakpoint.verified) },
				{ "address", json::Value(breakpoint.address) },
				{ "file", json::Value(breakpoint.file) },
				{ "line", json::Value(breakpoint.line) },
				{ "symbol", json::Value(breakpoint.symbol) },
				{ "hitCount", json::Value(breakpoint.hitCount) },
				{ "condition", json::Value(breakpoint.options.condition) },
				{ "hitCondition", json::Value(breakpoint.options.hitCondition) },
				{ "logMessage", json::Value(breakpoint.options.logMessage) }
			}));
		}
		return out;
	}

	void DebugServer::reportStop(const StopEvent& event)
	{
		emitEvent("stopped", describeStop(event));

		if (event.reason == StopReason::Exited)
		{
			emitEvent("exited", json::Object{ { "exitCode", json::Value(u32{ 0 }) } });
			_stopping.store(true, std::memory_order_release);
		}
	}

	// --- Reading --------------------------------------------------------------------------------

	void DebugServer::readLoop(const std::shared_ptr<ReaderControl>& control)
	{
		std::string line;
		while (std::getline(std::cin, line))
		{
			const std::lock_guard lock{ control->mutex };
			if (control->stopping)
				return;

			if (line.empty())
				continue;

			auto parsed = json::Value::parse(line);
			if (!parsed.has_value())
			{
				emitEvent("protocolError", json::Object{
					{ "message", json::Value(parsed.error()) }
				});
				continue;
			}

			// Acted on here rather than queued, because the main thread is very likely inside
			// resume() and will not look at the queue until something stops it.
			const std::string_view command = (*parsed)["command"].asString();
			if (command == "pause" || command == "terminate")
				_session.requestPause();

			{
				const std::lock_guard<std::mutex> queueLock{ _queueMutex };
				_pending.push_back(std::move(parsed.value()));
			}
			_queueSignal.notify_one();
		}

		const std::lock_guard lock{ control->mutex };
		if (!control->stopping)
		{
			_inputClosed.store(true, std::memory_order_release);
			_queueSignal.notify_all();
		}
	}

	int DebugServer::run()
	{
		_session.setOutputHandler([this](std::span<const u8> bytes)
		{
			// Bytes rather than text: a multi-byte UTF-8 character reaches the terminal device as
			// several separate port writes, so the adapter is the first place that can safely
			// decode one. Hex keeps it lossless on the way.
			emitEvent("output", json::Object{
				{ "category", json::Value("stdout") },
				{ "hex", json::Value(hexEncode(bytes)) }
			});
		});

		// A logpoint is the debugger talking, not the program, so it goes to the console category
		// where an editor renders it differently.
		_session.setLogHandler([this](std::string_view text)
		{
			emitEvent("output", json::Object{
				{ "category", json::Value("console") },
				{ "text", json::Value(text) }
			});
		});

		emitEvent("initialized", json::Object{
			{ "protocolVersion", json::Value(ProtocolVersion) },
			{ "hasDebugInfo", json::Value(!_session.debugInfo().isEmpty()) },
			{ "entryPoint", json::Value(_session.program().header().entryPoint) },
			{ "textSize", json::Value(_session.program().header().textSize) },
			{ "capabilities", json::Value(json::Object{
				{ "lineBreakpoints", json::Value(!_session.debugInfo().isEmpty()) },
				{ "functionBreakpoints", json::Value(!_session.debugInfo().isEmpty()) },
				{ "instructionBreakpoints", json::Value(true) },
				{ "disassembly", json::Value(true) },
				{ "readMemory", json::Value(true) },
				{ "writeMemory", json::Value(true) },
				{ "setRegister", json::Value(true) },
				{ "restart", json::Value(true) },
				{ "goto", json::Value(true) },
				{ "conditionalBreakpoints", json::Value(true) },
				{ "hitConditionalBreakpoints", json::Value(true) },
				{ "logPoints", json::Value(true) },
				{ "dataBreakpoints", json::Value(true) },
				{ "exceptionFilters", json::Value(true) },
				{ "evaluate", json::Value(true) },
				{ "coverage", json::Value(true) },
				// The machine is deterministic, so going backwards is a recording away rather than
				// out of reach; the session says whether it is actually recording.
				{ "stepBack", json::Value(_session.history().isEnabled()) }
			}) }
		});

		_reader = std::thread([this, control = _readerControl]() { readLoop(control); });

		while (!_stopping.load(std::memory_order_acquire))
		{
			json::Value request;
			{
				std::unique_lock<std::mutex> lock{ _queueMutex };
				_queueSignal.wait(lock, [this]()
				{
					return !_pending.empty() ||
						_inputClosed.load(std::memory_order_acquire) ||
						_stopping.load(std::memory_order_acquire);
				});

				if (_pending.empty())
					break; // Input closed or shutting down.

				request = std::move(_pending.front());
				_pending.pop_front();
			}

			if (!dispatch(request))
				break;
		}

		_session.terminate();
		emitEvent("terminated", json::Object{});
		return 0;
	}

	// --- Commands -------------------------------------------------------------------------------

	bool DebugServer::dispatch(const json::Value& request)
	{
		const std::string_view command = request["command"].asString();
		const json::Value& arguments = request["arguments"];

		if (command.empty())
		{
			fail(request, "Every request needs a 'command'");
			return true;
		}

		// --- Lifecycle ---
		if (command == "configurationDone")
		{
			if (_configured)
			{
				fail(request, "Already configured");
				return true;
			}
			_configured = true;
			respond(request, json::Object{});

			// Breakpoints are set before this point, so the program starts only now.
			reportStop(_session.start());
			return true;
		}

		if (command == "terminate" || command == "disconnect")
		{
			respond(request, json::Object{});
			return false;
		}

		if (command == "restart")
		{
			const StopEvent event = _session.restart();
			respond(request, describeStop(event));
			reportStop(event);
			return true;
		}

		// Every command below needs the machine to be up.
		const bool needsRunningSession =
			command == "continue" || command == "next" || command == "stepIn" ||
			command == "stepOut" || command == "stepInstruction" || command == "runTo" ||
			command == "stepBack" || command == "stepBackInstruction" ||
			command == "reverseContinue" || command == "runToTick";

		if (needsRunningSession && !_configured)
		{
			fail(request, "Send configurationDone before running anything");
			return true;
		}

		// --- Running ---
		if (command == "continue" || command == "next" || command == "stepIn" ||
			command == "stepOut" || command == "stepInstruction" || command == "runTo")
		{
			StopEvent event{};
			if (command == "continue")            event = _session.resume();
			else if (command == "next")           event = _session.stepOver();
			else if (command == "stepIn")         event = _session.stepLine();
			else if (command == "stepOut")        event = _session.stepOut();
			else if (command == "stepInstruction") event = _session.stepInstruction();
			else                                  event = _session.runToAddress(arguments["address"].asU32());

			respond(request, json::Object{});
			reportStop(event);
			return true;
		}

		if (command == "stepBack" || command == "stepBackInstruction" ||
			command == "reverseContinue" || command == "runToTick")
		{
			StopEvent event{};
			if (command == "stepBack")                  event = _session.stepBackLine();
			else if (command == "stepBackInstruction")  event = _session.stepBackInstruction();
			else if (command == "reverseContinue")      event = _session.reverseContinue();
			else                                        event = _session.runToTick(arguments["tick"].asU64());

			respond(request, json::Object{});
			reportStop(event);
			return true;
		}

		if (command == "coverage")
		{
			json::Array lines;
			for (const CoverageEntry& entry : _session.coverage())
			{
				json::Object out = describeLocation(entry.address);
				out.insert_or_assign("count", json::Value(entry.count));
				lines.push_back(json::Value(std::move(out)));
			}

			respond(request, json::Object{ { "instructions", json::Value(std::move(lines)) } });
			return true;
		}

		if (command == "history")
		{
			const History& history = _session.history();
			respond(request, json::Object{
				{ "enabled", json::Value(history.isEnabled()) },
				{ "snapshots", json::Value(static_cast<u64>(history.snapshotCount())) },
				{ "interval", json::Value(history.settings().interval) },
				{ "oldestTick", json::Value(history.oldestTick()) },
				{ "currentTick", json::Value(_session.currentTick()) },
				{ "memoryCost", json::Value(static_cast<u64>(history.memoryCost())) }
			});
			return true;
		}

		if (command == "pause")
		{
			// The reader thread has already asked the session to stop; by the time this is
			// dequeued the machine is standing still, so there is nothing left to do but say so.
			respond(request, describeLocation(_session.programCounter()));
			return true;
		}

		// --- Breakpoints ---
		if (command == "setBreakpoints")
		{
			// Replace-all for one file, which is how an editor thinks about it: the client sends
			// the complete set every time the user clicks in the gutter.
			const std::string file{ arguments["file"].asString() };

			std::vector<BreakpointId> toRemove;
			for (const Breakpoint& breakpoint : _session.breakpoints())
			{
				if (breakpoint.kind == BreakpointKind::Line && breakpoint.file == file)
					toRemove.push_back(breakpoint.id);
			}
			for (BreakpointId id : toRemove)
				_session.removeBreakpoint(id);

			json::Array results;
			for (const json::Value& entry : arguments["lines"].asArray())
			{
				// A line may arrive as a bare number or as an object carrying a condition; both
				// shapes are accepted so a script does not have to spell out the long form.
				const u32 line = entry.isObject() ? entry["line"].asU32() : entry.asU32();
				BreakpointOptions options{
					std::string(entry["condition"].asString()),
					std::string(entry["hitCondition"].asString()),
					std::string(entry["logMessage"].asString())
				};

				auto added = _session.addLineBreakpoint(file, line, std::move(options));

				json::Object result{
					{ "line", json::Value(line) },
					{ "verified", json::Value(added.has_value()) }
				};
				if (added.has_value())
				{
					result.insert_or_assign("id", json::Value(added.value()));
					result.insert_or_assign("address", json::Value(_session.breakpoints().back().address));
				}
				else
				{
					result.insert_or_assign("message", json::Value(added.error()));
				}
				results.push_back(json::Value(std::move(result)));
			}

			respond(request, json::Object{ { "breakpoints", json::Value(std::move(results)) } });
			return true;
		}

		if (command == "setFunctionBreakpoints")
		{
			std::vector<BreakpointId> toRemove;
			for (const Breakpoint& breakpoint : _session.breakpoints())
			{
				if (breakpoint.kind == BreakpointKind::Symbol)
					toRemove.push_back(breakpoint.id);
			}
			for (BreakpointId id : toRemove)
				_session.removeBreakpoint(id);

			json::Array results;
			for (const json::Value& entry : arguments["names"].asArray())
			{
				const std::string_view name = entry.asString();
				auto added = _session.addSymbolBreakpoint(name);

				json::Object result{
					{ "name", json::Value(name) },
					{ "verified", json::Value(added.has_value()) }
				};
				if (added.has_value())
				{
					result.insert_or_assign("id", json::Value(added.value()));
					result.insert_or_assign("address", json::Value(_session.breakpoints().back().address));
				}
				else
				{
					result.insert_or_assign("message", json::Value(added.error()));
				}
				results.push_back(json::Value(std::move(result)));
			}

			respond(request, json::Object{ { "breakpoints", json::Value(std::move(results)) } });
			return true;
		}

		if (command == "setInstructionBreakpoints")
		{
			std::vector<BreakpointId> toRemove;
			for (const Breakpoint& breakpoint : _session.breakpoints())
			{
				if (breakpoint.kind == BreakpointKind::Address)
					toRemove.push_back(breakpoint.id);
			}
			for (BreakpointId id : toRemove)
				_session.removeBreakpoint(id);

			json::Array results;
			for (const json::Value& entry : arguments["addresses"].asArray())
			{
				const u32 address = entry.asU32();
				auto added = _session.addAddressBreakpoint(address);
				results.push_back(json::Value(json::Object{
					{ "address", json::Value(address) },
					{ "verified", json::Value(added.has_value()) },
					{ "id", json::Value(added.value_or(0)) }
				}));
			}

			respond(request, json::Object{ { "breakpoints", json::Value(std::move(results)) } });
			return true;
		}

		if (command == "setDataBreakpoints")
		{
			_session.clearDataBreakpoints();

			json::Array results;
			for (const json::Value& entry : arguments["watches"].asArray())
			{
				const u32 address = entry["address"].asU32();
				const u32 size = entry["size"].asU32(4);

				// Writes unless asked otherwise, which is what a watch on a variable usually means.
				// An unknown mode is a write rather than an error: the watch still works, which is
				// better than refusing it over a spelling.
				const std::string_view modeName = entry["mode"].asString("write");
				WatchMode mode = WatchMode::Write;
				if (modeName == "read")
					mode = WatchMode::Read;
				else if (modeName == "readWrite" || modeName == "access")
					mode = WatchMode::ReadWrite;

				auto added = _session.addDataBreakpoint(address, size, std::string(entry["label"].asString()), mode);

				json::Object result{
					{ "address", json::Value(address) },
					{ "size", json::Value(size) },
					{ "mode", json::Value(std::string(modeName)) },
					{ "verified", json::Value(added.has_value()) }
				};
				if (added.has_value())
					result.insert_or_assign("id", json::Value(added.value()));
				else
					result.insert_or_assign("message", json::Value(added.error()));
				results.push_back(json::Value(std::move(result)));
			}

			respond(request, json::Object{ { "watches", json::Value(std::move(results)) } });
			return true;
		}

		if (command == "setExceptionFilters")
		{
			// A missing 'filters' means every system exception stops the machine, which is the
			// default; an explicit list - even an empty one - means exactly those.
			if (!arguments.has("filters") || arguments["filters"].isNull())
			{
				_session.setExceptionFilters(std::nullopt);
			}
			else
			{
				std::vector<vm::InterruptNumber> filters;
				for (const json::Value& entry : arguments["filters"].asArray())
				{
					const std::string_view name = entry.asString();
					for (u8 number = 0; number < vm::ReservedInterruptCount; ++number)
					{
						if (describe(static_cast<vm::InterruptNumber>(number)) == name)
						{
							filters.push_back(static_cast<vm::InterruptNumber>(number));
							break;
						}
					}
				}
				_session.setExceptionFilters(std::move(filters));
			}

			respond(request, json::Object{});
			return true;
		}

		if (command == "resolveLine")
		{
			// Where a line is *entered*, which is not merely its lowest address: a line occupying
			// several words is only entered at its head. "Jump to cursor" needs exactly this, and
			// resolving it by setting a throwaway breakpoint would clobber the real ones.
			const std::string_view file = arguments["file"].asString();
			const u32 line = arguments["line"].asU32();

			const auto address = _session.debugInfo().firstAddressOfLine(file, line);
			if (!address.has_value())
			{
				fail(request, std::format("No code was emitted for {}:{}", file, line));
				return true;
			}

			respond(request, json::Object{
				{ "file", json::Value(file) },
				{ "line", json::Value(line) },
				{ "address", json::Value(address.value()) }
			});
			return true;
		}

		if (command == "evaluate")
		{
			auto value = _session.evaluate(arguments["expression"].asString());
			if (!value.has_value())
			{
				fail(request, value.error());
				return true;
			}

			json::Object body{
				{ "result", json::Value(value->text) },
				{ "type", json::Value(value->type) }
			};
			if (value->address.has_value())
				body.insert_or_assign("address", json::Value(value->address.value()));
			if (value->integer.has_value())
				body.insert_or_assign("integer", json::Value(static_cast<double>(value->integer.value())));

			respond(request, std::move(body));
			return true;
		}

		if (command == "breakpoints")
		{
			respond(request, json::Object{ { "breakpoints", json::Value(describeBreakpoints()) } });
			return true;
		}

		// --- Inspecting ---
		if (command == "stackTrace")
		{
			respond(request, json::Object{ { "frames", json::Value(describeFrames()) } });
			return true;
		}

		if (command == "registers")
		{
			respond(request, describeRegisters());
			return true;
		}

		if (command == "globals")
		{
			respond(request, json::Object{ { "variables", json::Value(describeGlobals()) } });
			return true;
		}

		if (command == "readMemory")
		{
			const u32 address = arguments["address"].asU32();
			const u32 count = arguments["count"].asU32(64);
			const std::vector<u8> bytes = _session.readMemory(address, count);

			respond(request, json::Object{
				{ "address", json::Value(address) },
				{ "hex", json::Value(hexEncode(bytes)) },
				{ "size", json::Value(static_cast<u32>(bytes.size())) }
			});
			return true;
		}

		if (command == "writeMemory")
		{
			const auto bytes = hexDecode(arguments["hex"].asString());
			if (!bytes.has_value())
			{
				fail(request, "'hex' is not an even-length string of hex digits");
				return true;
			}

			const u32 address = arguments["address"].asU32();
			if (!_session.writeMemory(address, bytes.value()))
			{
				fail(request, std::format("{} bytes do not fit in memory at {:#010x}", bytes->size(), address));
				return true;
			}

			respond(request, json::Object{ { "written", json::Value(static_cast<u32>(bytes->size())) } });
			return true;
		}

		if (command == "disassemble")
		{
			const u32 address = arguments["address"].asU32(_session.programCounter());
			const u32 before = arguments["before"].asU32(0);
			const u32 count = arguments["count"].asU32(16);

			json::Array lines;
			for (const DisassembledInstruction& line : _session.disassemble(address, before, count))
			{
				json::Object entry = describeLocation(line.address);
				entry.insert_or_assign("raw", json::Value(line.raw));
				entry.insert_or_assign("text", json::Value(line.text));
				entry.insert_or_assign("symbol", json::Value(line.symbol));
				lines.push_back(json::Value(std::move(entry)));
			}

			respond(request, json::Object{ { "instructions", json::Value(std::move(lines)) } });
			return true;
		}

		// --- Changing things ---
		if (command == "setRegister")
		{
			const std::string_view name = arguments["name"].asString();
			const u32 value = arguments["value"].asU32();
			if (!_session.setRegister(name, value))
			{
				fail(request, std::format("'{}' is not a register this machine has", name));
				return true;
			}
			respond(request, describeRegisters());
			return true;
		}

		if (command == "goto")
		{
			const u32 address = arguments["address"].asU32();
			if (!_session.setProgramCounter(address))
			{
				fail(request, std::format("{:#010x} is not a valid instruction address", address));
				return true;
			}
			respond(request, describeLocation(address));
			return true;
		}

		if (command == "input")
		{
			_session.pushInput(arguments["text"].asString());
			respond(request, json::Object{});
			return true;
		}

		fail(request, std::format("Unknown command '{}'", command));
		return true;
	}
}
