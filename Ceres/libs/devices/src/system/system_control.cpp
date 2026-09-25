#include <ceres/devices/system/system_control.h>

namespace ceres::devices
{
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

	void SystemControlDevice::setFaultInfoHandlers(FaultInfoGetter address, FaultInfoGetter access)
	{
		_faultAddressGetter = std::move(address);
		_faultAccessGetter = std::move(access);
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
		if (offset == ArgumentCountRegister || offset == ArgumentVectorRegister || offset == EnvironmentRegister)
		{
			const u32 which = (offset.value() - ArgumentCountRegister.value()) / 4u;
			value = _argumentGetter ? _argumentGetter(which) : 0u;
			return true;
		}
		return false;
	}

	void SystemControlDevice::writeWord(Address offset, u32 value)
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
}
