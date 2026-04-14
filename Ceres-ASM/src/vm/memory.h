#pragma once

#include "common/types.h"
#include "address.h"
#include "instructions.h"
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <type_traits>

namespace ceres::vm
{
	class Memory
	{
	public:
		using ByteType = u8;

		static inline constexpr usize DefaultSize = 1024 * 1024 * 16; // 16 MiB
		static inline constexpr usize MaxSize = 1024 * 1024 * 1024; // 1 GiB
		static inline constexpr usize MinSize = 1024; // 1 KiB

		static inline constexpr usize NullPageSegmentSize = 0x100; // 256 bytes, used for null page (address 0x00000000 - 0x000000FF)
		static inline constexpr usize BiosSegmentSize = 0x300; // 768 bytes, used for BIOS (address 0x00000100 - 0x000003FF)
		// The rest of the memory (address 0x00000400 - 0xFFFFFFFF) is available for unrestricted use by programs.
		static inline constexpr usize UnrestrictedSegmentStartValue = NullPageSegmentSize + BiosSegmentSize;

		static inline constexpr Address NullPageSegmentStart = 0_addr;
		static inline constexpr Address BiosSegmentStart = NullPageSegmentStart + Address(NullPageSegmentSize);
		static inline constexpr Address UnrestrictedSegmentStart = NullPageSegmentStart + Address(UnrestrictedSegmentStartValue);

	private:
		std::vector<ByteType> _data;

	public:
		explicit Memory(usize size = DefaultSize) :
			_data(size, 0) // Initialize memory with zeros
		{
			if (size < MinSize || size > MaxSize)
				throw std::invalid_argument("Memory size must be between " + std::to_string(MinSize) + " and " + std::to_string(MaxSize) + " bytes.");
		}

		Memory(const Memory&) = delete;
		Memory(Memory&&) = delete;
		~Memory() = default;

		Memory& operator=(const Memory&) = delete;
		Memory& operator=(Memory&&) = delete;

	public:
		constexpr usize size() const noexcept { return _data.size(); }

		forceinline Instruction readInstruction(Address address) const
		{
			return Instruction(readUnchecked<Instruction::RawType>(address));
		}

		void loadBytes(Address address, std::span<const ByteType> bytes)
		{
			const usize base = address.value();
			if (base + bytes.size() > _data.size())
				throw std::out_of_range("Attempt to load bytes beyond memory bounds.");
			std::memcpy(_data.data() + base, bytes.data(), bytes.size());
		}

	public:
		template <Integral T> requires (sizeof(T) <= Register::Size)
		forceinline T read(Address address) const noexcept { return readRaw<T, UnrestrictedSegmentStartValue>(address); }

		forceinline f32 readFloat(Address address) const noexcept
		{
			const u32 rawValue = read<u32>(address);
			return std::bit_cast<f32>(rawValue);
		}

		template <Integral T> requires (sizeof(T) <= Register::Size)
		forceinline T readUnchecked(Address address) const noexcept
		{
			return readRaw<T, 0>(address); // Unchecked read that allows access to the null page (for instructions, etc.)
		}

		template <Integral T> requires (sizeof(T) <= Register::Size)
		forceinline void write(Address address, T value) noexcept { writeRaw<T, UnrestrictedSegmentStartValue>(address, value); }

		forceinline void writeFloat(Address address, f32 value) noexcept
		{
			const u32 rawValue = std::bit_cast<u32>(value);
			write<u32>(address, rawValue);
		}

		template <Integral T> requires (sizeof(T) <= Register::Size)
		forceinline void writeUnchecked(Address address, T value) noexcept
		{
			writeRaw<T, 0>(address, value); // Unchecked write that allows access to the null page (for instructions, etc.)
		}

	private:
        template <Integral T, usize FirstValidIndex> requires (sizeof(T) <= Register::Size)
		inline T readRaw(Address address) const noexcept
		{
			using UT = std::make_unsigned_t<T>;
			UT value = 0;
			const usize base = address.value();
			constexpr usize n = sizeof(T);

			// Fast path: fully in-bounds contiguous access. Keep checks conservative.
			if (_data.size() >= n && base >= FirstValidIndex && base <= _data.size() - n)
			{
				// Specialize common sizes so compiler can optimize (unrolled, no memcpy to avoid host endianness differences).
				if constexpr (n == 1)
				{
					value = static_cast<UT>(_data[base]);
					return static_cast<T>(value);
				}
				else if constexpr (n == 2)
				{
					value = static_cast<UT>(_data[base]) | (static_cast<UT>(_data[base + 1]) << 8);
					return static_cast<T>(value);
				}
				else if constexpr (n == 4)
				{
					value = static_cast<UT>(_data[base])
						  | (static_cast<UT>(_data[base + 1]) << 8)
						  | (static_cast<UT>(_data[base + 2]) << 16)
						  | (static_cast<UT>(_data[base + 3]) << 24);
					return static_cast<T>(value);
				}
				else
				{
					// Generic in-bounds path for other sizes (unrolled loop-like behavior).
					for (usize i = 0; i < n; ++i)
						value |= static_cast<UT>(_data[base + i]) << (i * 8);
					return static_cast<T>(value);
				}
			}

			// Slow path: possibly out-of-bounds bytes read as zero (null-page behavior).
			for (usize i = 0; i < n; ++i)
			{
				const usize idx = base + i;
				ByteType b = 0;
				if (idx >= FirstValidIndex && idx < _data.size())
					b = _data[idx];
				value |= static_cast<UT>(b) << (i * 8);
			}
			return static_cast<T>(value);
		}

        template <Integral T, usize FirstValidIndex> requires (sizeof(T) <= Register::Size)
		inline void writeRaw(Address address, T value) noexcept
		{
			using UT = std::make_unsigned_t<T>;
			const UT uv = static_cast<UT>(value);
			const usize base = address.value();
			constexpr usize n = sizeof(T);

			// Fast path: fully in-bounds contiguous access.
			if (_data.size() >= n && base >= FirstValidIndex && base <= _data.size() - n)
			{
				if constexpr (n == 1)
				{
					_data[base] = static_cast<ByteType>(uv & 0xFFu);
					return;
				}
				else if constexpr (n == 2)
				{
					_data[base] = static_cast<ByteType>(uv & 0xFFu);
					_data[base + 1] = static_cast<ByteType>((uv >> 8) & 0xFFu);
					return;
				}
				else if constexpr (n == 4)
				{
					_data[base] = static_cast<ByteType>(uv & 0xFFu);
					_data[base + 1] = static_cast<ByteType>((uv >> 8) & 0xFFu);
					_data[base + 2] = static_cast<ByteType>((uv >> 16) & 0xFFu);
					_data[base + 3] = static_cast<ByteType>((uv >> 24) & 0xFFu);
					return;
				}
				else
				{
					// Generic in-bounds path for other sizes.
					for (usize i = 0; i < n; ++i)
						_data[base + i] = static_cast<ByteType>((uv >> (i * 8)) & static_cast<UT>(0xFF));
					return;
				}
			}

			// Slow path: write bytes that are in range; ignore out-of-bounds (null-page behavior).
			for (usize i = 0; i < n; ++i)
			{
				const usize idx = base + i;
				if (idx >= FirstValidIndex && idx < _data.size())
					_data[idx] = static_cast<ByteType>((uv >> (i * 8)) & static_cast<UT>(0xFF));
			}
		}
	};
}
