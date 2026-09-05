#pragma once

#include "types.h"
#include <new>
#include <utility>
#include <memory>
#include <limits>

namespace ceres::mem
{
	template <typename T>
	inline T* allocate(usize count = 1)
	{
		if (count == 0)
			return nullptr;

		if (count > std::numeric_limits<usize>::max() / sizeof(T))
			throw std::bad_array_new_length();

		const usize bytes = sizeof(T) * count;

		void* ptr;
		if (alignof(T) > alignof(std::max_align_t))
			ptr = ::operator new(bytes, std::align_val_t(alignof(T)));
		else
			ptr = ::operator new(bytes);

		return static_cast<T*>(ptr);
	}

	template <typename T>
	inline void deallocate(T* ptr) noexcept
	{
		if (ptr == nullptr)
			return;

		if (alignof(T) > alignof(std::max_align_t))
			::operator delete(static_cast<void*>(ptr), std::align_val_t(alignof(T)));
		else
			::operator delete(static_cast<void*>(ptr));
	}

	template <typename T, typename... Args>
	constexpr T* construct(T* ptr, Args&&... args)
	{
		if (ptr == nullptr)
			return nullptr;
		return std::construct_at(ptr, std::forward<Args>(args)...);
	}

	template <typename T>
	constexpr void destroy(T* ptr) noexcept
	{
		if (ptr == nullptr)
			return;
		std::destroy_at(ptr);
	}
}
