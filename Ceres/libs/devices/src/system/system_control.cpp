#include <ceres/devices/system/system_control.h>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Command",        RegisterAccess::Write,     0x0, false, "Low byte: 1 shut down, 2 reset; the next byte is the exit status." },
			{ 0x04, "MemorySize",     RegisterAccess::Read,      0x0, false, "How many bytes of RAM the machine has." },
			{ 0x08, "Features",       RegisterAccess::ReadWrite, 0x0, false, "Switches for behaviour that is off by default." },
			{ 0x0C, "StackLimit",     RegisterAccess::ReadWrite, 0x0, false, "The lowest address the stack may reach; below it a push raises StackOverflow." },
			{ 0x10, "FaultAddress",   RegisterAccess::Read,      0x0, false, "The data address of the last memory fault." },
			{ 0x14, "FaultAccess",    RegisterAccess::Read,      0x0, false, "That fault: 1 read, 2 write, 3 fetch in bits 7:0, the size in bytes above." },
			{ 0x18, "ArgumentCount",  RegisterAccess::Read,      0x0, false, "argc, as main received it." },
			{ 0x1C, "ArgumentVector", RegisterAccess::Read,      0x0, false, "The address of argv." },
			{ 0x20, "Environment",    RegisterAccess::Read,      0x0, false, "The address of envp." },
			{ 0x2C, "FaultReason",    RegisterAccess::Read,      0x0, false, "Why the last memory fault happened: a FaultReason (plan/v2 SPEC 5.4)." },
		};
	}

	void SystemControlDevice::reset()
	{
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
		if (offset == FaultReasonRegister && _faultReasonGetter)
		{
			value = _faultReasonGetter();
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
	}

	const RegisterMap& SystemControlDevice::registers() const
	{
		static constexpr RegisterMap map{ "system-control", Registers };
		return map;
	}
}
