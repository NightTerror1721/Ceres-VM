#include <ceres/devices/system/system_control.h>

#include <format>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Command",        RegisterAccess::Write,     0x0, false, "Low byte: 1 shut down, 2 reset, 3 load and run LoadPath; the next byte is the exit status." },
			{ 0x04, "MemorySize",     RegisterAccess::Read,      0x0, false, "How many bytes of RAM the machine has." },
			{ 0x08, "Features",       RegisterAccess::ReadWrite, 0x0, false, "Switches for behaviour that is off by default." },
			{ 0x0C, "StackLimit",     RegisterAccess::ReadWrite, 0x0, false, "The lowest address the stack may reach; below it a push raises StackOverflow." },
			{ 0x10, "FaultAddress",   RegisterAccess::Read,      0x0, false, "The data address of the last memory fault." },
			{ 0x14, "FaultAccess",    RegisterAccess::Read,      0x0, false, "That fault: 1 read, 2 write, 3 fetch in bits 7:0, the size in bytes above." },
			{ 0x18, "ArgumentCount",  RegisterAccess::Read,      0x0, false, "argc, as main received it." },
			{ 0x1C, "ArgumentVector", RegisterAccess::Read,      0x0, false, "The address of argv." },
			{ 0x20, "Environment",    RegisterAccess::Read,      0x0, false, "The address of envp." },
			{ 0x24, "CpuClockHz",     RegisterAccess::Read,      0x0, false, "The CPU clock, in cycles per second." },
			{ 0x28, "ProfileId",      RegisterAccess::Read,      0x5, false, "The machine's profile: 0 micro ... 5 standard, 6 workstation, 7 custom." },
			{ 0x2C, "FaultReason",    RegisterAccess::Read,      0x0, false, "Why the last memory fault happened: a FaultReason (plan/v2 SPEC 5.4)." },
			{ 0x30, "LoadPath",       RegisterAccess::Write,     0x0, false, "The address of the HostFs path of the .cres command 3 loads." },
			{ 0x34, "LoadArgs",       RegisterAccess::Write,     0x0, false, "The address of argc, argv and envp for command 3 (0: argv is the path)." },
			{ 0x38, "VramSize",       RegisterAccess::Read,      0x0, false, "How many bytes of VRAM the machine has." },
		};

		// A little-endian word of RAM at an aligned address, or nothing when it is not all in RAM.
		std::optional<u32> ramWord(const Memory& memory, u32 address)
		{
			if (address % 4 != 0 || memory.clampBlockSize(Address(address), 4) != 4)
				return std::nullopt;
			const auto bytes = memory.peekBytes(Address(address), 4);
			return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[2]) << 16 | static_cast<u32>(bytes[3]) << 24;
		}

		// A NUL-terminated string of RAM, of up to `limit` bytes.
		std::expected<std::string, std::string> ramString(const Memory& memory, u32 address, u32 limit, std::string_view what)
		{
			const u32 available = memory.clampBlockSize(Address(address), limit + 1);
			if (available == 0)
				return std::unexpected(std::format("{} at 0x{:08X} is not in RAM", what, address));
			const auto bytes = memory.peekBytes(Address(address), available);
			for (u32 i = 0; i < bytes.size(); ++i)
				if (bytes[i] == 0)
					return std::string(reinterpret_cast<const char*>(bytes.data()), i);
			return std::unexpected(available == limit + 1 ? std::format("{} at 0x{:08X} is longer than {} bytes", what, address, limit)
				: std::format("{} at 0x{:08X} runs off the end of RAM", what, address));
		}

		// A NULL-terminated array of string addresses (envp), or `count` of them (argv).
		std::expected<std::vector<std::string>, std::string> ramStrings(const Memory& memory, u32 address, std::optional<u32> count, std::string_view what)
		{
			std::vector<std::string> strings;
			for (u32 i = 0;; ++i)
			{
				if (count && i == *count)
					return strings;
				if (i == SystemControlDevice::MaxLoadStrings)
					return std::unexpected(std::format("{} has more than {} entries", what, SystemControlDevice::MaxLoadStrings));
				const auto pointer = ramWord(memory, address + i * 4);
				if (!pointer)
					return std::unexpected(std::format("{}[{}] at 0x{:08X} is not in RAM", what, i, address + i * 4));
				if (*pointer == 0 && !count)
					return strings;
				auto text = ramString(memory, *pointer, SystemControlDevice::MaxLoadString, std::format("{}[{}]", what, i));
				if (!text)
					return std::unexpected(text.error());
				strings.push_back(std::move(*text));
			}
		}
	}

	void SystemControlDevice::reset()
	{
		_loadPath = 0;
		_loadArgs = 0;
		_features = 0;
		if (_featuresCallback)
			_featuresCallback(0);
	}

	void SystemControlDevice::setStackLimitHandlers(StackLimitGetter getter, StackLimitSetter setter)
	{
		_stackLimitGetter = std::move(getter);
		_stackLimitSetter = std::move(setter);
	}

	void SystemControlDevice::setFaultInfoHandlers(FaultInfoGetter address, FaultInfoGetter access, FaultInfoGetter reason)
	{
		_faultAddressGetter = std::move(address);
		_faultAccessGetter = std::move(access);
		_faultReasonGetter = std::move(reason);
	}

	void SystemControlDevice::command(u32 value)
	{
		const u32 code = value & 0xFF;
		if (code == CommandShutdown)
		{
			_exitCode.store(static_cast<u8>((value >> 8) & 0xFF), std::memory_order_relaxed);
			if (_shutdownCallback)
				_shutdownCallback();
		}
		else if (code == CommandReset)
		{
			if (_resetCallback)
				_resetCallback();
		}
		else if (code == CommandLoad)
		{
			if (_loadCallback)
				_loadCallback(readLoadRequest());
		}
	}

	std::expected<SystemControlDevice::LoadRequest, std::string> SystemControlDevice::readLoadRequest() const
	{
		LoadRequest request;
		auto path = ramString(memory(), _loadPath, MaxLoadString, "LoadPath");
		if (!path)
			return std::unexpected(path.error());
		request.path = std::move(*path);
		if (_loadArgs == 0)
		{
			request.arguments.push_back(request.path);
			return request;
		}

		const auto argc = ramWord(memory(), _loadArgs);
		const auto argv = ramWord(memory(), _loadArgs + 4);
		const auto envp = ramWord(memory(), _loadArgs + 8);
		if (!argc || !argv || !envp)
			return std::unexpected(std::format("LoadArgs at 0x{:08X} is not three aligned words of RAM", _loadArgs));
		if (*argc > MaxLoadStrings)
			return std::unexpected(std::format("LoadArgs gives {} arguments, more than {}", *argc, MaxLoadStrings));
		auto arguments = ramStrings(memory(), *argv, *argc, "argv");
		if (!arguments)
			return std::unexpected(arguments.error());
		request.arguments = std::move(*arguments);
		if (*envp != 0)
		{
			auto environment = ramStrings(memory(), *envp, std::nullopt, "envp");
			if (!environment)
				return std::unexpected(environment.error());
			request.environment = std::move(*environment);
		}
		return request;
	}

	bool SystemControlDevice::readable(Address offset, u32& value) const
	{
		if (offset == MemorySizeRegister)
		{
			value = static_cast<u32>(memory().size());
			return true;
		}
		if (offset == FeaturesRegister)
		{
			value = _features;
			return true;
		}
		if (offset == StackLimitRegister && _stackLimitGetter)
		{
			value = _stackLimitGetter();
			return true;
		}
		if (offset == FaultAddressRegister && _faultAddressGetter)
		{
			value = _faultAddressGetter();
			return true;
		}
		if (offset == FaultAccessRegister && _faultAccessGetter)
		{
			value = _faultAccessGetter();
			return true;
		}
		if (offset == CpuClockHzRegister)
		{
			const Scheduler* clock = scheduler();
			value = static_cast<u32>(clock != nullptr ? clock->clockHz() : DefaultCpuClockHz);
			return true;
		}
		if (offset == FaultReasonRegister && _faultReasonGetter)
		{
			value = _faultReasonGetter();
			return true;
		}
		if (offset == ProfileIdRegister)
		{
			value = _profileId;
			return true;
		}
		if (offset == VramSizeRegister)
		{
			value = static_cast<u32>(vram().size());
			return true;
		}
		if (offset == ArgumentCountRegister || offset == ArgumentVectorRegister || offset == EnvironmentRegister)
		{
			const u32 which = (offset.value() - ArgumentCountRegister.value()) / 4u;
			value = _argumentGetter ? _argumentGetter(which) : 0u;
			return true;
		}
		return false;
	}

	void SystemControlDevice::write(Address offset, u32 value)
	{
		if (offset == CommandRegister)
		{
			command(value);
		}
		else if (offset == FeaturesRegister)
		{
			_features = value;
			if (_featuresCallback)
				_featuresCallback(value);
		}
		else if (offset == StackLimitRegister && _stackLimitSetter)
		{
			_stackLimitSetter(value);
		}
		else if (offset == LoadPathRegister)
		{
			_loadPath = value;
		}
		else if (offset == LoadArgsRegister)
		{
			_loadArgs = value;
		}
	}

	const RegisterMap& SystemControlDevice::registers() const
	{
		static constexpr RegisterMap map{ "system-control", Registers };
		return map;
	}
}
