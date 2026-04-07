#pragma once

#include "execution_engine.h"
#include "bios.h"
#include "program.h"
#include <expected>
#include <string>

namespace ceres::vm
{
	class CeresVM
	{
	private:
		Memory _memory;
		IOPorts _ioPorts;
		BIOS _bios;
		ExecutionEngine _engine;

		bool _isPoweredOn = false;

	public:
		explicit CeresVM(usize memorySize = Memory::DefaultSize) :
			_memory(memorySize),
			_engine(_memory, _ioPorts)
		{}

		CeresVM(const CeresVM&) = delete;
		CeresVM(CeresVM&&) = delete;
		~CeresVM() = default;

		CeresVM& operator=(const CeresVM&) = delete;
		CeresVM& operator=(CeresVM&&) = delete;

	public:
		std::expected<void, std::string> loadProgram(const Program& program) noexcept;

		std::expected<void, std::string> run() noexcept;

	public:
		constexpr bool isPoweredOn() const noexcept { return _isPoweredOn; }
		constexpr void shutdown() noexcept { _isPoweredOn = false; }

		IOPorts& io() noexcept { return _ioPorts; }
	};
}
