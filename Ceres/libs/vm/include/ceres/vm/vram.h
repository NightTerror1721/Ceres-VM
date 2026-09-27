#pragma once

#include <ceres/core/base/types.h>
#include <ceres/core/base/host_pages.h>
#include <ceres/core/format/memory_map.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ceres::vm
{
	// The video memory: up to 1 GiB at 0xA0000000 (plan/v2 SPEC 2). The CPU reaches it with the same loads and
	// stores as RAM, at 3 cycles instead of 2, and so do the block instructions and the DMA.
	//
	// It lives in the VM rather than with the GPU because the execution engine routes to it on every access past
	// the RAM: the GPU (F5) is one more reader of it, not its owner.
	//
	// Every store the CPU makes marks its 4 KiB page as written. The hardware executor (F12) uploads only those
	// pages to the host GPU, and clears the marks when it has.
	class Vram
	{
	public:
		static inline constexpr u32 BaseValue = fmt::MemoryMap::VramStartValue;
		// Where the VRAM window ends (exclusive): past the largest VRAM, 0xE0000000-0xFEFFFFFF is empty.
		static inline constexpr u32 WindowEndValue = fmt::MemoryMap::VramLimitValue;

		static inline constexpr usize MinSize = 16 * 1024;              // 16 KiB
		static inline constexpr usize MaxSize = 1024 * 1024 * 1024;     // 1 GiB
		static inline constexpr usize DefaultSize = 32 * 1024 * 1024;   // 32 MiB, the `standard` profile's
		static inline constexpr u32 PageSize = 4096;
		static inline constexpr u32 PageShift = 12;

	private:
		HostPages _data;
		std::vector<u64> _written;   // bit p: page p was stored to since the marks were last cleared

		static usize checkedSize(usize size)
		{
			if (size < MinSize || size > MaxSize || size % PageSize != 0)
				throw std::invalid_argument("VRAM size must be a multiple of 4096 bytes between " + std::to_string(MinSize) +
					" and " + std::to_string(MaxSize) + " bytes.");
			return size;
		}

	public:
		explicit Vram(usize size = DefaultSize) :
			_data(checkedSize(size)),
			_written((size / PageSize + 63) / 64, 0)
		{
		}

		Vram(const Vram&) = delete;
		Vram(Vram&&) = delete;
		Vram& operator=(const Vram&) = delete;
		Vram& operator=(Vram&&) = delete;

		forceinline usize size() const noexcept { return _data.size(); }

		// Whether a physical access of `bytes` at `physical` lies wholly in the backed VRAM.
		forceinline bool backs(u32 physical, u32 bytes) const noexcept
		{
			const u64 offset = static_cast<u64>(physical) - BaseValue;
			return physical >= BaseValue && offset + bytes <= _data.size();
		}

		static forceinline constexpr bool inWindow(u32 physical) noexcept
		{
			return physical >= BaseValue && physical < WindowEndValue;
		}

		// `offset` is from the start of the VRAM, and the access has to fit (backs()). Little-endian, as the RAM is.
		template <typename T> requires std::is_trivially_copyable_v<T> && (sizeof(T) <= 8)
		forceinline T read(u32 offset) const noexcept
		{
			if constexpr (std::endian::native == std::endian::little)
			{
				T value;
				std::memcpy(&value, _data.data() + offset, sizeof(T));
				return value;
			}
			else
			{
				using Bits = std::conditional_t<sizeof(T) == 1, u8, std::conditional_t<sizeof(T) == 2, u16, std::conditional_t<sizeof(T) == 4, u32, u64>>>;
				Bits bits = 0;
				for (usize i = 0; i < sizeof(T); ++i)
					bits |= static_cast<Bits>(static_cast<Bits>(_data[offset + i]) << (8 * i));
				return std::bit_cast<T>(bits);
			}
		}

		template <typename T> requires std::is_trivially_copyable_v<T> && (sizeof(T) <= 8)
		forceinline void write(u32 offset, T value) noexcept
		{
			if constexpr (std::endian::native == std::endian::little)
				std::memcpy(_data.data() + offset, &value, sizeof(T));
			else
			{
				using Bits = std::conditional_t<sizeof(T) == 1, u8, std::conditional_t<sizeof(T) == 2, u16, std::conditional_t<sizeof(T) == 4, u32, u64>>>;
				const Bits bits = std::bit_cast<Bits>(value);
				for (usize i = 0; i < sizeof(T); ++i)
					_data[offset + i] = static_cast<u8>(bits >> (8 * i));
			}
			markWritten(offset, sizeof(T));
		}

		// The bytes themselves, for a reader that does not store (the GPU, the debugger). A writer goes through
		// span(), which marks what it hands out.
		const u8* data() const noexcept { return _data.data(); }

		// [offset, offset + bytes) for writing, marked as written. The range has to fit.
		u8* span(u32 offset, u32 bytes) noexcept
		{
			markWritten(offset, bytes);
			return _data.data() + offset;
		}

		// How many of `bytes` from `offset` fit in the VRAM.
		u32 clamp(u32 offset, u32 bytes) const noexcept
		{
			if (offset >= _data.size())
				return 0;
			return static_cast<u32>(std::min<usize>(bytes, _data.size() - offset));
		}

		void markWritten(u32 offset, u32 bytes) noexcept
		{
			if (bytes == 0)
				return;
			const u32 first = offset >> PageShift;
			const u32 last = (offset + bytes - 1) >> PageShift;
			for (u32 page = first; page <= last; ++page)
				_written[page >> 6] |= u64{ 1 } << (page & 63);
		}

		bool written(u32 page) const noexcept
		{
			return page / 64 < _written.size() && ((_written[page >> 6] >> (page & 63)) & 1u) != 0;
		}

		usize writtenPages() const noexcept
		{
			usize count = 0;
			for (u64 word : _written)
				count += static_cast<usize>(std::popcount(word));
			return count;
		}

		void clearWritten() noexcept { std::fill(_written.begin(), _written.end(), u64{ 0 }); }
	};
}
