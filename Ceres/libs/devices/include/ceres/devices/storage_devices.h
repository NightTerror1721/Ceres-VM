#pragma once

// The two device ranges that were reserved from the start and never filled in: a block disk at
// 0x20-0x23 and a text framebuffer at 0x30-0x33. Both are deliberately the simplest thing that
// is actually usable rather than a sketch of a richer device: a disk is sectors of a fixed size,
// and a framebuffer is a grid of characters that is written to and then shown.

#include <ceres/vm/io_ports.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	// A disk of fixed-size sectors, backed by a host file when it is given one and by memory
	// otherwise. Transfers go through `inm`/`outm` on the data port: the sector number and the
	// command say what to move, and the block instruction says where in memory it goes.
	//
	//   li   r1, 3
	//   out  DISK_SECTOR, r1        // which sector
	//   li   r2, 512
	//   inm  buffer, DISK_DATA, r2  // read it into `buffer`
	//   inb  r3, DISK_STATUS        // and check it worked
	class DiskDevice final : public IODevice
	{
	public:
		static inline constexpr PortNumber StatusPort = default_ports::DISK_STATUS;
		static inline constexpr PortNumber CommandPort = default_ports::DISK_CMD;
		static inline constexpr PortNumber SectorPort = default_ports::DISK_SECTOR;
		static inline constexpr PortNumber DataPort = default_ports::DISK_DATA;

		static inline constexpr u32 SectorSize = 512;

		// Status bits. Ready is set unless the last operation failed, so a program that never
		// checks still behaves, and one that does gets told.
		static inline constexpr u32 StatusReady = 1u << 0;
		static inline constexpr u32 StatusError = 1u << 1;

		// Commands, written to the command port. A flush is only meaningful for a file-backed
		// disk; on a memory one it succeeds and does nothing.
		static inline constexpr u32 CommandFlush = 1;

	private:
		std::vector<u8> _image;
		std::filesystem::path _path; // Empty for a disk that lives only in memory
		u32 _sector = 0;
		u32 _status = StatusReady;
		bool _dirty = false;

	public:
		// A disk of `sectors` empty sectors, with nothing behind it.
		explicit DiskDevice(u32 sectors = 64) :
			_image(static_cast<usize>(sectors) * SectorSize, 0)
		{}

		DiskDevice(const DiskDevice&) = delete;
		DiskDevice(DiskDevice&&) = delete;
		~DiskDevice() override
		{
			// A program that forgets to flush still gets its writes; losing them would make the
			// device useless for the one thing it is for.
			flush();
		}

		DiskDevice& operator=(const DiskDevice&) = delete;
		DiskDevice& operator=(DiskDevice&&) = delete;

	public:
		void attachTo(IOPorts& ioPorts)
		{
			ioPorts.attach(StatusPort, *this);
			ioPorts.attach(CommandPort, *this);
			ioPorts.attach(SectorPort, *this);
			ioPorts.attach(DataPort, *this);
		}

		void detachFrom(IOPorts& ioPorts)
		{
			ioPorts.detach(StatusPort);
			ioPorts.detach(CommandPort);
			ioPorts.detach(SectorPort);
			ioPorts.detach(DataPort);
		}

		// Backs the disk with a host file, creating it at `sectors` sectors if it is not there.
		// Returns false when the file cannot be read, and leaves the disk as it was.
		bool open(const std::filesystem::path& path, u32 sectors = 64)
		{
			std::error_code error;
			if (std::filesystem::exists(path, error))
			{
				std::ifstream file(path, std::ios::binary);
				if (!file)
					return false;

				_image.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
			else
			{
				_image.assign(static_cast<usize>(sectors) * SectorSize, 0);
			}

			// A short file is padded rather than refused: a disk with a partial last sector is a
			// disk, and rounding it up is what every image format does anyway.
			if (_image.size() % SectorSize != 0)
				_image.resize(((_image.size() / SectorSize) + 1) * SectorSize, 0);

			_path = path;
			_status = StatusReady;
			_dirty = false;
			return true;
		}

		bool flush()
		{
			if (_path.empty() || !_dirty)
				return true;

			std::ofstream file(_path, std::ios::binary | std::ios::trunc);
			if (!file)
				return false;

			file.write(reinterpret_cast<const char*>(_image.data()), static_cast<std::streamsize>(_image.size()));
			_dirty = false;
			return true;
		}

		u32 sectorCount() const noexcept { return static_cast<u32>(_image.size() / SectorSize); }
		std::span<const u8> image() const noexcept { return _image; }

	public:
		u8 readPortUnsignedByte(PortNumber port) override { return static_cast<u8>(readPortUnsignedWord(port)); }
		i8 readPortSignedByte(PortNumber port) override { return static_cast<i8>(readPortUnsignedWord(port)); }
		u16 readPortUnsignedHalfword(PortNumber port) override { return static_cast<u16>(readPortUnsignedWord(port)); }
		i16 readPortSignedHalfword(PortNumber port) override { return static_cast<i16>(readPortUnsignedWord(port)); }

		u32 readPortUnsignedWord(PortNumber port) override
		{
			switch (port)
			{
				case StatusPort: return _status;
				case SectorPort: return _sector;
				default: return 0;
			}
		}

		// `inm buffer, DISK_DATA, size`: the selected sector, into memory.
		void readPort(PortNumber port, Address address, u32 size) override
		{
			if (port != DataPort || size == 0)
				return;

			const usize offset = static_cast<usize>(_sector) * SectorSize;
			if (offset >= _image.size() || size > SectorSize)
			{
				_status = StatusError;
				return;
			}

			// A transfer that runs off the end of the sector is clamped rather than reading the
			// next one: a sector is the unit, and silently spilling into its neighbour is how a
			// program ends up with data it never asked for.
			const u32 available = static_cast<u32>(std::min<usize>(size, _image.size() - offset));
			auto buffer = memory().peekMutBytes(address, available);
			std::copy_n(_image.begin() + static_cast<std::ptrdiff_t>(offset), available, buffer.begin());
			_status = StatusReady;
		}

		void writePortByte(PortNumber port, u8 value) override { writePortWord(port, value); }
		void writePortHalfword(PortNumber port, u16 value) override { writePortWord(port, value); }

		void writePortWord(PortNumber port, u32 value) override
		{
			switch (port)
			{
				case SectorPort:
					_sector = value;
					_status = value < sectorCount() ? StatusReady : StatusError;
					break;

				case CommandPort:
					if (value == CommandFlush)
						_status = flush() ? StatusReady : StatusError;
					break;

				default:
					break;
			}
		}

		// `outm DISK_DATA, buffer, size`: memory into the selected sector.
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (port != DataPort || size == 0)
				return;

			const usize offset = static_cast<usize>(_sector) * SectorSize;
			if (offset >= _image.size() || size > SectorSize)
			{
				_status = StatusError;
				return;
			}

			const u32 available = static_cast<u32>(std::min<usize>(size, _image.size() - offset));
			const auto bytes = memory().peekBytes(address, available);
			std::copy_n(bytes.begin(), available, _image.begin() + static_cast<std::ptrdiff_t>(offset));
			_status = StatusReady;
			_dirty = true;
		}
	};

	// A grid of characters that a program draws into and then shows. Not pixels: this machine has
	// no window to put them in, and a text framebuffer is the thing the tutorial's games actually
	// want - a board that is redrawn whole rather than a terminal that is scrolled.
	//
	//   li   r1, 20
	//   out  GPU_WIDTH, r1
	//   li   r1, 10
	//   out  GPU_HEIGHT, r1
	//   li   r1, 1
	//   out  GPU_CMD, r1            // clear
	//   outm GPU_DATA, cells, size  // the whole grid at once
	//   li   r1, 2
	//   out  GPU_CMD, r1            // show it
	class FramebufferDevice final : public IODevice
	{
	public:
		static inline constexpr PortNumber CommandPort = default_ports::GPU_CMD;
		static inline constexpr PortNumber WidthPort = default_ports::GPU_WIDTH;
		static inline constexpr PortNumber HeightPort = default_ports::GPU_HEIGHT;
		static inline constexpr PortNumber DataPort = default_ports::SPRITE_DATA;

		static inline constexpr u32 CommandClear = 1;
		static inline constexpr u32 CommandPresent = 2;

		static inline constexpr u32 MaxWidth = 200;
		static inline constexpr u32 MaxHeight = 100;

		using PresentSink = std::function<void(std::string_view)>;

	private:
		u32 _width = 40;
		u32 _height = 20;
		std::vector<u8> _cells;
		u32 _cursor = 0; // Where the next block write lands, in cells
		PresentSink _sink;

	public:
		FramebufferDevice() : _cells(static_cast<usize>(_width) * _height, ' ') {}

		FramebufferDevice(const FramebufferDevice&) = delete;
		FramebufferDevice(FramebufferDevice&&) = delete;
		~FramebufferDevice() override = default;

		FramebufferDevice& operator=(const FramebufferDevice&) = delete;
		FramebufferDevice& operator=(FramebufferDevice&&) = delete;

	public:
		void attachTo(IOPorts& ioPorts)
		{
			ioPorts.attach(CommandPort, *this);
			ioPorts.attach(WidthPort, *this);
			ioPorts.attach(HeightPort, *this);
			ioPorts.attach(DataPort, *this);
		}

		void detachFrom(IOPorts& ioPorts)
		{
			ioPorts.detach(CommandPort);
			ioPorts.detach(WidthPort);
			ioPorts.detach(HeightPort);
			ioPorts.detach(DataPort);
		}

		// Where a presented frame goes. Without one it goes to stdout, which is what the CLI wants
		// and what a test does not.
		void setPresentSink(PresentSink sink) { _sink = std::move(sink); }

		u32 width() const noexcept { return _width; }
		u32 height() const noexcept { return _height; }
		std::span<const u8> cells() const noexcept { return _cells; }

		// The grid as text, rows separated by newlines. What `present` sends, and what a test can
		// compare against without going through a terminal.
		std::string toText() const
		{
			std::string out;
			out.reserve(static_cast<usize>(_height) * (_width + 1));
			for (u32 row = 0; row < _height; ++row)
			{
				const usize start = static_cast<usize>(row) * _width;
				for (u32 column = 0; column < _width; ++column)
				{
					const u8 cell = _cells[start + column];
					// A cell nobody wrote is a space, and a control character would move the
					// terminal's own cursor rather than showing anything.
					out.push_back(cell >= 0x20 && cell < 0x7F ? static_cast<char>(cell) : ' ');
				}
				out.push_back('\n');
			}
			return out;
		}

	public:
		u8 readPortUnsignedByte(PortNumber port) override { return static_cast<u8>(readPortUnsignedWord(port)); }
		i8 readPortSignedByte(PortNumber port) override { return static_cast<i8>(readPortUnsignedWord(port)); }
		u16 readPortUnsignedHalfword(PortNumber port) override { return static_cast<u16>(readPortUnsignedWord(port)); }
		i16 readPortSignedHalfword(PortNumber port) override { return static_cast<i16>(readPortUnsignedWord(port)); }

		u32 readPortUnsignedWord(PortNumber port) override
		{
			switch (port)
			{
				case WidthPort: return _width;
				case HeightPort: return _height;
				default: return 0;
			}
		}

		void readPort(PortNumber, Address, u32) override {}

		void writePortByte(PortNumber port, u8 value) override { writePortWord(port, value); }
		void writePortHalfword(PortNumber port, u16 value) override { writePortWord(port, value); }

		void writePortWord(PortNumber port, u32 value) override
		{
			switch (port)
			{
				case WidthPort: resize(value, _height); break;
				case HeightPort: resize(_width, value); break;

				case CommandPort:
					if (value == CommandClear)
					{
						std::ranges::fill(_cells, static_cast<u8>(' '));
						_cursor = 0;
					}
					else if (value == CommandPresent)
					{
						present();
					}
					break;

				case DataPort:
					// One cell at a time, for a program that would rather poke than blit.
					if (_cursor < _cells.size())
						_cells[_cursor++] = static_cast<u8>(value & 0xFFu);
					break;

				default:
					break;
			}
		}

		// `outm GPU_DATA, cells, size`: a run of cells starting where the last one left off, so a
		// whole grid is one instruction and a row is one per row.
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (port != DataPort || size == 0 || _cursor >= _cells.size())
				return;

			const u32 available = static_cast<u32>(std::min<usize>(size, _cells.size() - _cursor));
			const auto bytes = memory().peekBytes(address, available);
			std::copy_n(bytes.begin(), available, _cells.begin() + static_cast<std::ptrdiff_t>(_cursor));
			_cursor += available;
		}

	private:
		void resize(u32 width, u32 height)
		{
			// A grid of nothing, or one larger than any terminal, is a typo rather than a request.
			if (width == 0 || height == 0 || width > MaxWidth || height > MaxHeight)
				return;

			_width = width;
			_height = height;
			_cells.assign(static_cast<usize>(_width) * _height, ' ');
			_cursor = 0;
		}

		void present()
		{
			const std::string text = toText();
			_cursor = 0;

			if (_sink)
			{
				_sink(text);
				return;
			}

			std::fputs(text.c_str(), stdout);
			std::fflush(stdout);
		}
	};
}
