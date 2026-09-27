#pragma once

#include <ceres/core/base/types.h>

namespace ceres
{
	// A block of host memory that reads as zero and costs nothing until it is written (plan/v2 SPEC 2).
	//
	// The machine's RAM can be 2 GiB and its VRAM 1 GiB, and a program that touches one megabyte of either
	// must not make the host pay for the rest. A std::vector<u8>(size, 0) writes every byte up front, so
	// every page of it becomes resident on construction. This asks the operating system for pages it zeroes
	// the first time they are touched instead: VirtualAlloc on Windows, an anonymous mmap elsewhere. The
	// block is still one contiguous range, so a load or a store stays a single memcpy.
	class HostPages
	{
	public:
		HostPages() noexcept = default;
		// Throws std::bad_alloc when the host refuses the reservation.
		explicit HostPages(usize size);
		~HostPages();

		HostPages(const HostPages&) = delete;
		HostPages& operator=(const HostPages&) = delete;
		HostPages(HostPages&& other) noexcept;
		HostPages& operator=(HostPages&& other) noexcept;

		forceinline u8* data() noexcept { return _data; }
		forceinline const u8* data() const noexcept { return _data; }
		forceinline usize size() const noexcept { return _size; }
		forceinline u8& operator[](usize index) noexcept { return _data[index]; }
		forceinline const u8& operator[](usize index) const noexcept { return _data[index]; }

	private:
		void release() noexcept;

		u8* _data = nullptr;
		usize _size = 0;
	};
}
