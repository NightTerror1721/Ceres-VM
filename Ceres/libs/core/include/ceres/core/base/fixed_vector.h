#pragma once

#include "memory.h"
#include <cstring>
#include <initializer_list>
#include <span>
#include <ranges>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <functional>

namespace ceres
{
	template <typename T> requires DefaultConstructible<T> && (std::copyable<T> || std::movable<T>)
	class FixedVector
	{
	public:
		using ValueType = T;
		using SizeType = usize;
		using Reference = ValueType&;
		using ConstReference = const ValueType&;
		using Pointer = ValueType*;
		using ConstPointer = const ValueType*;
		using iterator = ValueType*;
		using const_iterator = const ValueType*;

	private:
		static inline constexpr bool IsCopyable = std::copyable<T>;
		static inline constexpr bool IsMoveable = std::movable<T>;
		static inline constexpr bool IsTriviallyCopyable = std::is_trivially_copyable_v<T>;
		static inline constexpr bool IsTriviallyDestructible = std::is_trivially_destructible_v<T>;

	private:
		Pointer _data = nullptr;
		SizeType _size = 0;

	public:
		constexpr FixedVector() noexcept = default;
		constexpr FixedVector(const FixedVector& other) requires IsCopyable :
			_data(other._size > 0 ? mem::allocate<ValueType>(other._size) : nullptr), _size(other._size)
		{
			if constexpr (IsTriviallyCopyable)
			{
				if (_data)
					std::memcpy(_data, other._data, sizeof(ValueType) * _size);
			}
			else
			{
				if (_data)
				{
					SizeType i = 0;
					try
					{
						for (; i < _size; ++i)
							mem::construct(&_data[i], other._data[i]);
					}
					catch (...)
					{
						for (SizeType j = 0; j < i; ++j)
							mem::destroy(&_data[j]);
						mem::deallocate(_data);
						throw;
					}
				}
			}
		}
		constexpr FixedVector(FixedVector&& other) noexcept requires IsMoveable :
			_data(other._data), _size(other._size)
		{
			other._data = nullptr;
			other._size = 0;
		}
		constexpr ~FixedVector() noexcept
		{
			if (_data)
			{
				if constexpr (!std::is_trivially_destructible_v<ValueType>)
				{
					for (SizeType i = 0; i < _size; ++i)
						mem::destroy(&_data[i]);
				}
				mem::deallocate(_data);
			}
		}

		constexpr FixedVector& operator=(const FixedVector& other) requires IsCopyable
		{
			if (this != &other)
			{
				FixedVector temp{ other };
				std::swap(_data, temp._data);
				std::swap(_size, temp._size);
			}
			return *this;
		}
		constexpr FixedVector& operator=(FixedVector&& other) noexcept requires IsMoveable
		{
			if (this != &other)
			{
				FixedVector temp{ std::move(other) };
				std::swap(_data, temp._data);
				std::swap(_size, temp._size);
			}
			return *this;
		}

	public:
		constexpr FixedVector(std::initializer_list<ValueType> init) requires IsCopyable :
			_data(init.size() ? mem::allocate<ValueType>(init.size()) : nullptr), _size(init.size())
		{
			if (_data)
				for (SizeType i = 0; const auto& value : init)
					_data[i++] = value;
		}

		constexpr explicit FixedVector(std::span<const ValueType> span) requires IsCopyable :
			_data(span.size() ? mem::allocate<ValueType>(span.size()) : nullptr), _size(span.size())
		{
			if (_data)
				for (SizeType i = 0; i < _size; ++i)
					_data[i] = span[i];
		}

		constexpr explicit FixedVector(std::span<const ValueType> span, const std::function<ValueType(const ValueType&)>& transform) requires IsCopyable :
			_data(span.size() ? mem::allocate<ValueType>(span.size()) : nullptr), _size(span.size())
		{
			if (_data)
				for (SizeType i = 0; i < _size; ++i)
					_data[i] = transform(span[i]);
		}

		template <std::ranges::range R> requires std::convertible_to<std::ranges::range_value_t<R>, ValueType> && IsCopyable
		constexpr explicit FixedVector(R&& range) noexcept :
			_data(std::ranges::size(range) ? mem::allocate<ValueType>(std::ranges::size(range)) : nullptr), _size(std::ranges::size(range))
		{
			if (_data)
				for (SizeType i = 0; const auto& value : range)
					_data[i++] = value;
		}

		template <std::ranges::range R> requires IsMoveable
		constexpr explicit FixedVector(const R& range, const std::function<ValueType(std::ranges::range_value_t<R>)>& transform) noexcept :
			_data(std::ranges::size(range) ? mem::allocate<ValueType>(std::ranges::size(range)) : nullptr), _size(std::ranges::size(range))
		{
			if (_data)
				for (SizeType i = 0; const auto& value : range)
					_data[i++] = transform(value);
		}

		constexpr explicit FixedVector(SizeType size, const ValueType& defaultValue = ValueType()) requires IsCopyable :
			_data(size > 0 ? mem::allocate<ValueType>(size) : nullptr), _size(size)
		{
			if (_data)
				for (SizeType i = 0; i < _size; ++i)
					_data[i] = defaultValue;
		}

		constexpr explicit FixedVector(SizeType size, const std::function<ValueType(SizeType)>& generator) requires IsMoveable :
			_data(size > 0 ? mem::allocate<ValueType>(size) : nullptr), _size(size)
		{
			if (_data)
				for (SizeType i = 0; i < _size; ++i)
					_data[i] = generator(i);
		}

		constexpr SizeType size() const noexcept { return _size; }
		constexpr bool empty() const noexcept { return _size == 0; }

		constexpr Pointer data() noexcept { return _data; }
		constexpr ConstPointer data() const noexcept { return _data; }

		constexpr Reference front() noexcept { return _data[0]; }
		constexpr ConstReference front() const noexcept { return _data[0]; }

		constexpr Reference back() noexcept { return _data[_size - 1]; }
		constexpr ConstReference back() const noexcept { return _data[_size - 1]; }

		constexpr Reference at(SizeType index)
		{
			if (index >= _size)
				throw std::out_of_range("FixedVector::at: index out of range");
			return _data[index];
		}
		constexpr ConstReference at(SizeType index) const
		{
			if (index >= _size)
				throw std::out_of_range("FixedVector::at: index out of range");
			return _data[index];
		}

		constexpr std::span<ValueType> span() noexcept { return std::span<ValueType>(_data, _size); }
		constexpr std::span<const ValueType> span() const noexcept { return std::span<const ValueType>(_data, _size); }

	public:
		constexpr explicit operator bool() const noexcept { return _data != nullptr; }
		constexpr bool operator!() const noexcept { return _data == nullptr; }

		constexpr operator std::span<ValueType>() noexcept { return std::span<ValueType>(_data, _size); }
		constexpr operator std::span<const ValueType>() const noexcept { return std::span<const ValueType>(_data, _size); }

		constexpr Reference operator[](SizeType index) noexcept { return _data[index]; }
		constexpr ConstReference operator[](SizeType index) const noexcept { return _data[index]; }

	public:
		constexpr iterator begin() noexcept { return _data; }
		constexpr const_iterator begin() const noexcept { return _data; }
		constexpr const_iterator cbegin() const noexcept { return _data; }

		constexpr iterator end() noexcept { return _data + _size; }
		constexpr const_iterator end() const noexcept { return _data + _size; }
		constexpr const_iterator cend() const noexcept { return _data + _size; }

	public:
		static constexpr FixedVector makeEmpty() noexcept { return FixedVector(); }

		static constexpr void swap(FixedVector& a, FixedVector& b) noexcept
		{
			std::swap(a._data, b._data);
			std::swap(a._size, b._size);
		}

		static const FixedVector Empty;
	};

	template <typename T> requires DefaultConstructible<T> && (std::copyable<T> || std::movable<T>)
	inline constexpr const FixedVector<T> FixedVector<T>::Empty = FixedVector<T>::makeEmpty();
}
