#pragma once

#include <ceres/core/base/types.h>
#include <span>
#include <string_view>

namespace ceres::vm
{
	// What a program may do with a device register.
	enum class RegisterAccess : u8
	{
		Read,            // reading has a meaning; a write is ignored
		Write,           // writing has a meaning; a read gives 0
		ReadWrite,
		WriteOneToClear, // reading gives the bits; writing a 1 clears that bit
	};

	// One register of a device: where it is in the device's slot, what it is called, how it is reached,
	// what it holds after a reset and one sentence on what it does.
	struct RegisterInfo
	{
		u32 offset;
		std::string_view name;
		RegisterAccess access;
		u32 resetValue;
		std::string_view description;
	};

	// A device's registers, declared once (plan/v2 SPEC 5.3). The debugger's `dev` command, --strict-mmio,
	// the tests and the device documentation read it; the device's own read and write never do - they stay a
	// switch, so the table costs nothing on the hot path.
	class RegisterMap
	{
		std::string_view _device;
		std::span<const RegisterInfo> _registers;

	public:
		constexpr RegisterMap() noexcept = default;
		constexpr RegisterMap(std::string_view device, std::span<const RegisterInfo> registers) noexcept :
			_device(device), _registers(registers)
		{}

		constexpr std::string_view device() const noexcept { return _device; }
		constexpr std::span<const RegisterInfo> registers() const noexcept { return _registers; }
		constexpr bool empty() const noexcept { return _registers.empty(); }

		// The register at `offset`, or nullptr when the device declares none there.
		constexpr const RegisterInfo* find(u32 offset) const noexcept
		{
			for (const RegisterInfo& info : _registers)
				if (info.offset == offset)
					return &info;
			return nullptr;
		}
	};
}
