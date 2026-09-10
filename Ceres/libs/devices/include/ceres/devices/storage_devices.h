#pragma once

// The two device ranges that were reserved from the start and never filled in: a block disk and
// a text framebuffer. Both are deliberately the simplest thing that is actually usable rather
// than a sketch of a richer device: a disk is sectors of a fixed size, and a framebuffer is a
// grid of characters that is written to and then shown.

#include <ceres/vm/mmio_bus.h>
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
	// otherwise. A transfer goes through the block registers: the sector number says which one,
	// and BLOCK_ADDR/BLOCK_LEN/BLOCK_CMD say where in memory it goes.
	//
	//   li   r1, 3
	//   str  [r_sector + 0], r1     // which sector
	//   str  [r_disk + BLOCK_ADDR], r_buffer
	//   str  [r_disk + BLOCK_LEN], r2      // 512
	//   li   r3, 1
	//   str  [r_disk + BLOCK_CMD], r3       // 1 = read sector -> buffer
	//   ldrb r4, [r_status]
	class DiskDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);
		static inline constexpr Address CommandRegister = Address(0x04);
		static inline constexpr Address SectorRegister = Address(0x08);
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 SectorSize = 512;

		// Status bits. Ready is set unless the last operation failed, so a program that never
		// checks still behaves, and one that does gets told.
		static inline constexpr u32 StatusReady = 1u << 0;
		static inline constexpr u32 StatusError = 1u << 1;

		// Commands, written to the command register. A flush is only meaningful for a file-backed
		// disk; on a memory one it succeeds and does nothing.
		static inline constexpr u32 CommandFlush = 1;

		// Block commands: which direction BLOCK_CMD moves the selected sector.
		static inline constexpr u32 BlockCommandRead = 1;  // sector -> RAM
		static inline constexpr u32 BlockCommandWrite = 2; // RAM -> sector

	private:
		std::vector<u8> _image;
		std::filesystem::path _path; // Empty for a disk that lives only in memory
		u32 _sector = 0;
		u32 _status = StatusReady;
		bool _dirty = false;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;

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
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Disk, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Disk);
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

	private:
		// `size` bytes of the selected sector, into memory at `ramAddress`.
		void blockRead(Address ramAddress, u32 size)
		{
			if (size == 0)
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
			auto buffer = memory().peekMutBytes(ramAddress, available);
			std::copy_n(_image.begin() + static_cast<std::ptrdiff_t>(offset), available, buffer.begin());
			_status = StatusReady;
		}

		// `size` bytes of memory at `ramAddress`, into the selected sector.
		void blockWrite(Address ramAddress, u32 size)
		{
			if (size == 0)
				return;

			const usize offset = static_cast<usize>(_sector) * SectorSize;
			if (offset >= _image.size() || size > SectorSize)
			{
				_status = StatusError;
				return;
			}

			const u32 available = static_cast<u32>(std::min<usize>(size, _image.size() - offset));
			const auto bytes = memory().peekBytes(ramAddress, available);
			std::copy_n(bytes.begin(), available, _image.begin() + static_cast<std::ptrdiff_t>(offset));
			_status = StatusReady;
			_dirty = true;
		}

	public:
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedWord(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedWord(offset)); }

		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister) return _status;
			if (offset == SectorRegister) return _sector;
			return 0;
		}

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == SectorRegister)
			{
				_sector = value;
				_status = value < sectorCount() ? StatusReady : StatusError;
				return;
			}
			if (offset == CommandRegister)
			{
				if (value == CommandFlush)
					_status = flush() ? StatusReady : StatusError;
				return;
			}
			if (offset == BlockAddressRegister) { _blockAddress = value; return; }
			if (offset == BlockLengthRegister) { _blockLength = value; return; }
			if (offset == BlockCommandRegister)
			{
				if (value == BlockCommandRead)
					blockRead(Address(_blockAddress), _blockLength);
				else if (value == BlockCommandWrite)
					blockWrite(Address(_blockAddress), _blockLength);
			}
		}
	};

	// A grid of characters that a program draws into and then shows. Not pixels: this machine has
	// no window to put them in, and a text framebuffer is the thing the tutorial's games actually
	// want - a board that is redrawn whole rather than a terminal that is scrolled.
	class FramebufferDevice final : public IODevice
	{
	public:
		static inline constexpr Address CommandRegister = Address(0x00);
		static inline constexpr Address WidthRegister = Address(0x04);
		static inline constexpr Address HeightRegister = Address(0x08);
		static inline constexpr Address DataRegister = Address(0x0C);
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 CommandClear = 1;
		static inline constexpr u32 CommandPresent = 2;

		static inline constexpr u32 MaxWidth = 200;
		static inline constexpr u32 MaxHeight = 100;

		// Block commands: DataRegister writes one cell at a time; the block form (BlockCommandWrite
		// only - a framebuffer is never read back in bulk) writes a run in one trigger.
		static inline constexpr u32 BlockCommandWrite = 2;

		using PresentSink = std::function<void(std::string_view)>;

	private:
		u32 _width = 40;
		u32 _height = 20;
		std::vector<u8> _cells;
		u32 _cursor = 0; // Where the next block write lands, in cells
		PresentSink _sink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;

	public:
		FramebufferDevice() : _cells(static_cast<usize>(_width) * _height, ' ') {}

		FramebufferDevice(const FramebufferDevice&) = delete;
		FramebufferDevice(FramebufferDevice&&) = delete;
		~FramebufferDevice() override = default;

		FramebufferDevice& operator=(const FramebufferDevice&) = delete;
		FramebufferDevice& operator=(FramebufferDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Framebuffer, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Framebuffer);
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
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedWord(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedWord(offset)); }

		u32 readUnsignedWord(Address offset) override
		{
			if (offset == WidthRegister) return _width;
			if (offset == HeightRegister) return _height;
			return 0;
		}

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == WidthRegister) { resize(value, _height); return; }
			if (offset == HeightRegister) { resize(_width, value); return; }

			if (offset == CommandRegister)
			{
				if (value == CommandClear)
				{
					std::ranges::fill(_cells, static_cast<u8>(' '));
					_cursor = 0;
				}
				else if (value == CommandPresent)
				{
					present();
				}
				return;
			}

			if (offset == DataRegister)
			{
				// One cell at a time, for a program that would rather poke than blit.
				if (_cursor < _cells.size())
					_cells[_cursor++] = static_cast<u8>(value & 0xFFu);
				return;
			}

			if (offset == BlockAddressRegister) { _blockAddress = value; return; }
			if (offset == BlockLengthRegister) { _blockLength = value; return; }
			if (offset == BlockCommandRegister && value == BlockCommandWrite)
				blockWrite(Address(_blockAddress), _blockLength);
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

		// A run of cells starting where the last write left off, so a whole grid is one trigger
		// and a row is one per row.
		void blockWrite(Address ramAddress, u32 size)
		{
			if (size == 0 || _cursor >= _cells.size())
				return;

			const u32 available = static_cast<u32>(std::min<usize>(size, _cells.size() - _cursor));
			const auto bytes = memory().peekBytes(ramAddress, available);
			std::copy_n(bytes.begin(), available, _cells.begin() + static_cast<std::ptrdiff_t>(_cursor));
			_cursor += available;
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
