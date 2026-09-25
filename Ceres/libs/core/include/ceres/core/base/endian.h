#pragma once

#include "types.h"
#include <bit>
#include <concepts>

// Ceres is little-endian: an instruction, a word in memory, a field of a .cres are stored low byte first,
// whatever the host is. These are the only way code turns a value into bytes or back, so the host's own byte
// order never leaks in (plan/v2 SPEC 6.6). Written byte by byte: correct on any host, and every compiler folds
// them into one load or store on a little-endian one.
namespace ceres
{
	constexpr u32 loadLittleEndian32(const u8* bytes) noexcept
	{
		return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
			(static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
	}

	constexpr void storeLittleEndian32(u8* bytes, u32 value) noexcept
	{
		bytes[0] = static_cast<u8>(value);
		bytes[1] = static_cast<u8>(value >> 8);
		bytes[2] = static_cast<u8>(value >> 16);
		bytes[3] = static_cast<u8>(value >> 24);
	}

	// Any unsigned value, low byte first, into `bytes` (sizeof(T) of them).
	template <std::unsigned_integral T>
	constexpr void storeLittleEndian(u8* bytes, T value) noexcept
	{
		for (usize i = 0; i < sizeof(T); ++i)
			bytes[i] = static_cast<u8>(value >> (8 * i));
	}

	// The value as it sits in little-endian memory, read as a native integer: a no-op on a little-endian host.
	template <std::unsigned_integral T>
	constexpr T nativeToLittleEndian(T value) noexcept
	{
		if constexpr (std::endian::native == std::endian::little)
			return value;
		else
			return std::byteswap(value);
	}
}
