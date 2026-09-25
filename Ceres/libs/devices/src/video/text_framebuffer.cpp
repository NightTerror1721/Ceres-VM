#include <ceres/devices/video/text_framebuffer.h>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Command",      RegisterAccess::Write,     0x0, false, "1 clears the grid, 2 presents it." },
			{ 0x04, "Width",        RegisterAccess::ReadWrite, 0x0, false, "Columns of the grid." },
			{ 0x08, "Height",       RegisterAccess::ReadWrite, 0x0, false, "Rows of the grid." },
			{ 0x0C, "Data",         RegisterAccess::Write,     0x0, false, "One cell at the cursor: character and attribute." },
			{ 0x10, "Mode",         RegisterAccess::ReadWrite, 0x0, false, "Where a presented frame goes - ModeAuto, ModeTerminal or ModeWindow." },
			{ 0x14, "Output",       RegisterAccess::Read,      0x0, false, "Where it goes right now - OutputTerminal or OutputWindow." },
			{ 0xF0, "BlockAddress", RegisterAccess::Write,     0x0, false, "RAM address of the characters or attributes to copy in." },
			{ 0xF4, "BlockLength",  RegisterAccess::Write,     0x0, false, "Bytes to copy: one a cell." },
			{ 0xF8, "BlockCommand", RegisterAccess::Write,     0x0, false, "2 copies characters into the grid, 3 copies attributes (one byte a cell)." },
		};
	}

	bool FramebufferDevice::takeWindowFrame(Frame& out)
	{
		if (!_hasWindowFrame)
			return false;
		out = std::move(_windowFrame);
		_hasWindowFrame = false;
		return true;
	}

	void FramebufferDevice::fallBackToTerminal()
	{
		_windowHost = false;
		presentToTerminal();
	}

	std::string FramebufferDevice::toText() const
	{
		std::string out;
		out.reserve(static_cast<usize>(_height) * (_width + 1));
		for (u32 row = 0; row < _height; ++row)
		{
			const usize start = static_cast<usize>(row) * _width;
			for (u32 column = 0; column < _width; ++column)
				appendCell(out, _cells[start + column]);
			out.push_back('\n');
		}
		return out;
	}

	std::string FramebufferDevice::toAnsiText() const
	{
		std::string out;
		out.reserve(static_cast<usize>(_height) * (_width + 1));
		for (u32 row = 0; row < _height; ++row)
		{
			const usize start = static_cast<usize>(row) * _width;
			u8 current = 0;
			for (u32 column = 0; column < _width; ++column)
			{
				const u8 attribute = _attributes[start + column];
				if (attribute != current)
				{
					appendSgr(out, attribute);
					current = attribute;
				}
				appendCell(out, _cells[start + column]);
			}
			if (current != 0)
				out += "\x1b[0m";
			out.push_back('\n');
		}
		return out;
	}

	void FramebufferDevice::appendCell(std::string& out, u8 cell)
	{
		if (cell >= 0x20 && cell < 0x7F)
			out.push_back(static_cast<char>(cell));
		else if (cell >= 0xA0)
		{
			out.push_back(static_cast<char>(0xC0 | (cell >> 6)));
			out.push_back(static_cast<char>(0x80 | (cell & 0x3F)));
		}
		else
			out.push_back(' ');
	}

	void FramebufferDevice::appendSgr(std::string& out, u8 attribute)
	{
		if (attribute == 0)
		{
			out += "\x1b[0m";
			return;
		}
		const u32 fg = attribute & 0x0F;
		const u32 bg = attribute >> 4;
		out += "\x1b[" + std::to_string(fg < 8 ? 30 + fg : 90 + (fg - 8)) + ';' +
			std::to_string(bg < 8 ? 40 + bg : 100 + (bg - 8)) + 'm';
	}

	u32 FramebufferDevice::read(Address offset)
	{
		if (offset == WidthRegister) return _width;
		if (offset == HeightRegister) return _height;
		if (offset == ModeRegister) return _mode;
		if (offset == OutputRegister) return output();
		return 0;
	}

	void FramebufferDevice::write(Address offset, u32 value)
	{
		if (offset == WidthRegister) { resize(value, _height); return; }
		if (offset == HeightRegister) { resize(_width, value); return; }
		if (offset == ModeRegister) { if (value <= ModeWindow) _mode = value; return; }

		if (offset == CommandRegister)
		{
			if (value == CommandClear)
			{
				std::ranges::fill(_cells, static_cast<u8>(' '));
				std::ranges::fill(_attributes, static_cast<u8>(0));
				_cursor = 0;
				_attributeCursor = 0;
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
			{
				_cells[_cursor] = static_cast<u8>(value & 0xFFu);
				_attributes[_cursor] = static_cast<u8>((value >> AttributeShift) & 0xFFu);
				++_cursor;
			}
			return;
		}

		if (offset == BlockAddressRegister) { _blockAddress = value; return; }
		if (offset == BlockLengthRegister) { _blockLength = value; return; }
		if (offset == BlockCommandRegister)
		{
			if (value == BlockCommandWrite)
				blockWrite(Address(_blockAddress), _blockLength, _cells, _cursor);
			else if (value == BlockCommandWriteAttributes)
				blockWrite(Address(_blockAddress), _blockLength, _attributes, _attributeCursor);
		}
	}

	void FramebufferDevice::resize(u32 width, u32 height)
	{
		// A grid of nothing, or one larger than any terminal, is a typo rather than a request.
		if (width == 0 || height == 0 || width > MaxWidth || height > MaxHeight)
			return;

		_width = width;
		_height = height;
		_cells.assign(static_cast<usize>(_width) * _height, ' ');
		_attributes.assign(_cells.size(), 0);
		_cursor = 0;
		_attributeCursor = 0;
	}

	void FramebufferDevice::blockWrite(Address ramAddress, u32 size, std::vector<u8>& plane, u32& cursor)
	{
		if (size == 0 || cursor >= plane.size())
			return;

		const u32 available = static_cast<u32>(std::min<usize>(size, plane.size() - cursor));
		const u32 clampSize = memory().clampBlockSize(ramAddress, available);
		if (clampSize == 0)
			return;

		const auto bytes = memory().peekBytes(ramAddress, clampSize);
		std::copy_n(bytes.begin(), clampSize, plane.begin() + static_cast<std::ptrdiff_t>(cursor));
		cursor += clampSize;
	}

	void FramebufferDevice::present()
	{
		if (output() == OutputWindow)
		{
			_windowFrame = snapshot();
			_hasWindowFrame = true;
			_cursor = 0;
			_attributeCursor = 0;
			return;
		}
		presentToTerminal();
	}

	void FramebufferDevice::presentToTerminal()
	{
		const std::string text = hasAttributes() ? toAnsiText() : toText();
		_cursor = 0;
		_attributeCursor = 0;

		if (_sink)
		{
			_sink(text);
			return;
		}

		std::fputs(text.c_str(), stdout);
		std::fflush(stdout);
	}

	const RegisterMap& FramebufferDevice::registers() const
	{
		static constexpr RegisterMap map{ "text-framebuffer", Registers };
		return map;
	}
}
