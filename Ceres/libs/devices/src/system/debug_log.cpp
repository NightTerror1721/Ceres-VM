#include <ceres/devices/system/debug_log.h>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Output",  RegisterAccess::Write,     0x0, false, "Low byte: one character of the line; a newline sends it." },
			{ 0x04, "Level",   RegisterAccess::ReadWrite, 0x2, false, "0 error, 1 warning, 2 info, 3 debug: the level of the next line." },
			{ 0x08, "Flush",   RegisterAccess::Write,     0x0, false, "Sends the unfinished line." },
			{ 0x0C, "Break",   RegisterAccess::Write,     0x0, false, "Stops the program under the debugger; nothing otherwise." },
			{ 0x10, "Enabled", RegisterAccess::Read,      0x0, false, "1 when the host collects the log." },
		};
	}

	std::string_view DebugLogDevice::levelName(u32 level) noexcept
	{
		switch (level)
		{
			case LevelError:   return "error";
			case LevelWarning: return "warn";
			case LevelInfo:    return "info";
			default:           return "debug";
		}
	}

	void DebugLogDevice::reset()
	{
		_line.clear();
		_level = LevelInfo;
	}

	void DebugLogDevice::emit()
	{
		if (_sink)
			_sink(_level, _line);
		_line.clear();
	}

	u32 DebugLogDevice::read(Address offset)
	{
		if (offset == LevelRegister)
			return _level;
		if (offset == EnabledRegister)
			return _sink ? 1u : 0u;
		return 0;
	}

	void DebugLogDevice::write(Address offset, u32 value)
	{
		if (offset == OutputRegister)
		{
			const char c = static_cast<char>(value & 0xFF);
			if (c == '\n')
				emit();
			else
			{
				_line.push_back(c);
				if (_line.size() >= MaxLine)
					emit();
			}
			return;
		}
		if (offset == LevelRegister)
		{
			_level = value < LevelDebug ? value : LevelDebug;
			return;
		}
		if (offset == FlushRegister)
		{
			if (!_line.empty())
				emit();
			return;
		}
		if (offset == BreakRegister)
		{
			if (_break)
				_break();
		}
	}

	const RegisterMap& DebugLogDevice::registers() const
	{
		static constexpr RegisterMap map{ "debuglog", Registers };
		return map;
	}
}
