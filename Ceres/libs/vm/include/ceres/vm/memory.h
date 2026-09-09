#pragma once

#include <ceres/core/base/types.h>
#include <ceres/core/isa/address.h>
#include <ceres/core/isa/instructions.h>
#include <ceres/core/format/memory_map.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <type_traits>

namespace ceres::vm
{
	using namespace isa;

	class Memory
	{
	public:
		using ByteType = u8;

		static inline constexpr usize DefaultSize = 1024 * 1024 * 16; // 16 MiB
		static inline constexpr usize MaxSize = 1024 * 1024 * 1024; // 1 GiB
		static inline constexpr usize MinSize = 1024; // 1 KiB

		// The top of memory belongs to interrupt handlers, not to the program. A handler used to
		// run on whatever stack it interrupted, which meant a program that had nearly exhausted
		// its own stack could not take an interrupt at all: the push of the saved PC was the thing
		// that overflowed, and the overflow was itself an interrupt.
		static inline constexpr usize SystemStackSize = fmt::MemoryMap::SystemStackSize;

		static inline constexpr usize NullPageSegmentSize = fmt::MemoryMap::NullPageSegmentSize; // 256 bytes, used for null page (address 0x00000000 - 0x000000FF)
		static inline constexpr usize BiosSegmentSize = fmt::MemoryMap::BiosSegmentSize; // 768 bytes, used for BIOS (address 0x00000100 - 0x000003FF)
		// The rest of the memory (address 0x00000400 - 0xFFFFFFFF) is available for unrestricted use by programs.
		static inline constexpr usize UnrestrictedSegmentStartValue = fmt::MemoryMap::UnrestrictedSegmentStartValue;

		static inline constexpr Address NullPageSegmentStart = fmt::MemoryMap::NullPageSegmentStart;
		static inline constexpr Address BiosSegmentStart = fmt::MemoryMap::BiosSegmentStart;
		static inline constexpr Address UnrestrictedSegmentStart = fmt::MemoryMap::UnrestrictedSegmentStart;

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

		void writeBytesUnchecked(Address address, std::span<const ByteType> bytes)
		{
			const usize base = address.value();
			if (base + bytes.size() > _data.size())
				throw std::out_of_range("Attempt to load bytes beyond memory bounds.");
			if (bytes.size() > 0)
				std::memcpy(_data.data() + base, bytes.data(), bytes.size());
		}
		void writeBytes(Address address, std::span<const ByteType> bytes)
		{
			const usize base = address.value();
			if (base < UnrestrictedSegmentStartValue || base + bytes.size() > _data.size())
				throw std::out_of_range("Attempt to load bytes beyond memory bounds or into restricted segment.");
			if (bytes.size() > 0)
				std::memcpy(_data.data() + base, bytes.data(), bytes.size());
		}

		void readBytesUnchecked(Address address, std::span<ByteType> buffer) const
		{
			const usize base = address.value();
			if (base + buffer.size() > _data.size())
				throw std::out_of_range("Attempt to read bytes beyond memory bounds.");
			if (buffer.size() > 0)
				std::memcpy(buffer.data(), _data.data() + base, buffer.size());
		}
		void readBytes(Address address, std::span<ByteType> buffer) const
		{
			const usize base = address.value();
			if (base < UnrestrictedSegmentStartValue || base + buffer.size() > _data.size())
				throw std::out_of_range("Attempt to read bytes beyond memory bounds or from restricted segment.");
			if (buffer.size() > 0)
				std::memcpy(buffer.data(), _data.data() + base, buffer.size());
		}

		std::span<const ByteType> peekBytesUnchecked(Address address, u32 size) const
		{
			const usize base = address.value();
			if (base + size > _data.size())
				throw std::out_of_range("Attempt to get bytes beyond memory bounds.");
			return std::span<const ByteType>(_data.data() + base, size);
		}
		std::span<const ByteType> peekBytes(Address address, u32 size) const
		{
			const usize base = address.value();
			if (base < UnrestrictedSegmentStartValue || base + size > _data.size())
				throw std::out_of_range("Attempt to get bytes beyond memory bounds or from restricted segment.");
			return std::span<const ByteType>(_data.data() + base, size);
		}

		std::span<ByteType> peekMutBytesUnchecked(Address address, u32 size)
		{
			const usize base = address.value();
			if (base + size > _data.size())
				throw std::out_of_range("Attempt to get mutable bytes beyond memory bounds.");
			return std::span<ByteType>(_data.data() + base, size);
		}
		std::span<ByteType> peekMutBytes(Address address, u32 size)
		{
			const usize base = address.value();
			if (base < UnrestrictedSegmentStartValue || base + size > _data.size())
				throw std::out_of_range("Attempt to get mutable bytes beyond memory bounds or from restricted segment.");
			return std::span<ByteType>(_data.data() + base, size);
		}

		void copyBytesUnchecked(Address srcAddress, Address destAddress, u32 size)
		{
			const usize srcBase = srcAddress.value();
			const usize destBase = destAddress.value();
			if (srcBase + size > _data.size() || destBase + size > _data.size())
				throw std::out_of_range("Attempt to copy bytes beyond memory bounds.");
			if (size > 0)
				std::memmove(_data.data() + destBase, _data.data() + srcBase, size);
		}
		void copyBytes(Address srcAddress, Address destAddress, u32 size)
		{
			const usize srcBase = srcAddress.value();
			const usize destBase = destAddress.value();
			if (srcBase < UnrestrictedSegmentStartValue || destBase < UnrestrictedSegmentStartValue ||
				srcBase + size > _data.size() || destBase + size > _data.size())
				throw std::out_of_range("Attempt to copy bytes beyond memory bounds or into restricted segment.");
			if (size > 0)
				std::memmove(_data.data() + destBase, _data.data() + srcBase, size);
		}

		void setBytesUnchecked(Address address, ByteType value, u32 size)
		{
			const usize base = address.value();
			if (base + size > _data.size())
				throw std::out_of_range("Attempt to set bytes beyond memory bounds.");
			if (size > 0)
				std::memset(_data.data() + base, value, size);
		}
		void setBytes(Address address, ByteType value, u32 size)
		{
			const usize base = address.value();
			if (base < UnrestrictedSegmentStartValue || base + size > _data.size())
				throw std::out_of_range("Attempt to set bytes beyond memory bounds or into restricted segment.");
			if (size > 0)
				std::memset(_data.data() + base, value, size);
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
