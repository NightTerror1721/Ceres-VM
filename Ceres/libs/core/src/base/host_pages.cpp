#include <ceres/core/base/host_pages.h>

#include <new>
#include <utility>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <sys/mman.h>
#endif

namespace ceres
{
	HostPages::HostPages(usize size)
	{
		if (size == 0)
			return;

#if defined(_WIN32)
		// Reserved and committed at once: committing only counts against the commit limit, it does not make
		// a page resident. Windows zeroes each page on its first touch.
		void* block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (block == nullptr)
			throw std::bad_alloc();
#else
		// MAP_NORESERVE: a 2 GiB machine that touches a megabyte must not need 2 GiB of swap to start.
		void* block = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (block == MAP_FAILED)
			throw std::bad_alloc();
#endif
		_data = static_cast<u8*>(block);
		_size = size;
	}

	HostPages::~HostPages()
	{
		release();
	}

	HostPages::HostPages(HostPages&& other) noexcept :
		_data(std::exchange(other._data, nullptr)),
		_size(std::exchange(other._size, 0))
	{
	}

	HostPages& HostPages::operator=(HostPages&& other) noexcept
	{
		if (this != &other)
		{
			release();
			_data = std::exchange(other._data, nullptr);
			_size = std::exchange(other._size, 0);
		}
		return *this;
	}

	void HostPages::release() noexcept
	{
		if (_data == nullptr)
			return;
#if defined(_WIN32)
		VirtualFree(_data, 0, MEM_RELEASE);
#else
		munmap(_data, _size);
#endif
		_data = nullptr;
		_size = 0;
	}
}
