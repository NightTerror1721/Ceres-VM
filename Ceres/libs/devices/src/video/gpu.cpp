#include <ceres/devices/video/gpu.h>
#include <ceres/devices/video/cost_model.h>
#include <ceres/devices/video/formats.h>

#include <algorithm>

namespace ceres::devices
{
	namespace
	{
		// A tile layer's registers (plan/v2 SPEC 7.5), from its block's first offset.
#define LAYER(n, at) 			{ at + 0x00, "Layer" #n "Control",   RegisterAccess::ReadWrite, 0x0, false, "Bit 0 on, 1 16x16 tiles, 2 8 bpp, 5:4 priority, 7:6 and 9:8 the map's size, 10 line scroll, 15:12 palette bank." }, 			{ at + 0x04, "Layer" #n "MapBase",   RegisterAccess::ReadWrite, 0x0, false, "The map: 16-bit entries, row by row." }, 			{ at + 0x08, "Layer" #n "TileBase",  RegisterAccess::ReadWrite, 0x0, false, "The tiles the map's entries number." }, 			{ at + 0x0C, "Layer" #n "ScrollX",   RegisterAccess::ReadWrite, 0x0, false, "The map's column at the screen's left edge (it wraps round)." }, 			{ at + 0x10, "Layer" #n "ScrollY",   RegisterAccess::ReadWrite, 0x0, false, "Its row at the top." }, 			{ at + 0x14, "Layer" #n "LineScrollBase", RegisterAccess::ReadWrite, 0x0, false, "A word a line: dx in bits 15:0, dy in 31:16 (with Control bit 10)." }

		// Every register of the core, the display and the planes (plan/v2 SPEC 7.3 and 7.5), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x000, "Id",              RegisterAccess::Read,            GpuDevice::IdValue, false, "0x55504743, \"CGPU\"." },
			{ 0x004, "Version",         RegisterAccess::Read,            GpuDevice::VersionValue, false, "Major << 16 | minor." },
			{ 0x008, "Caps",            RegisterAccess::Read,            0x0, false, "Bits 0-6 the levels available, 8-15 the bitmap formats, 31 a hardware executor." },
			{ 0x00C, "VramSize",        RegisterAccess::Read,            0x0, false, "The VRAM, in bytes." },
			{ 0x010, "GpuClockHz",      RegisterAccess::Read,            0x0, false, "The GPU clock, in cycles per second." },
			{ 0x014, "Mode",            RegisterAccess::ReadWrite,       0x0, false, "The video level, 0-6; a write above the highest available is clamped." },
			{ 0x018, "MaxLevel",        RegisterAccess::Read,            0x0, false, "The highest level the machine's profile allows." },
			{ 0x01C, "Control",         RegisterAccess::ReadWrite,       GpuDevice::ControlDisplayOn, false, "Bit 0 display on, 1 command processor, 2 reset the GPU." },
			{ 0x020, "Status",          RegisterAccess::Read,            0x0, false, "Bit 0 busy, 1 fault, 2 in the vertical blank, 3 flip pending." },
			{ 0x024, "IrqEnable",       RegisterAccess::ReadWrite,       0x0, false, "Bit 0 VBlank (32), 1 line (33), 2 fence or copy (34), 3 fault (35)." },
			{ 0x028, "IrqStatus",       RegisterAccess::WriteOneToClear, 0x0, false, "What happened, by the same bits; write 1s to clear." },
			{ 0x02C, "FaultCode",       RegisterAccess::Read,            0x0, false, "Why the GPU faulted: 1 an engine's address outside RAM and VRAM, 2 a CPU store to VRAM outside the vertical blank." },
			{ 0x030, "FaultAddress",    RegisterAccess::Read,            0x0, false, "The address that faulted." },
			{ 0x100, "Width",           RegisterAccess::ReadWrite,       0x0, false, "The screen's width in pixels, up to the profile's largest." },
			{ 0x104, "Height",          RegisterAccess::ReadWrite,       0x0, false, "The screen's height in pixels." },
			{ 0x108, "Refresh",         RegisterAccess::Read,            60,  false, "Frames a second: 50 or 60." },
			{ 0x10C, "LinesTotal",      RegisterAccess::Read,            0x0, false, "Lines a frame: the visible ones plus 45 of blanking, at least 262." },
			{ 0x110, "VCount",          RegisterAccess::Read,            0x0, false, "The line being scanned now." },
			{ 0x114, "LineCompare",     RegisterAccess::ReadWrite,       0x0, false, "Interrupt 33 when the scan reaches this line (with IrqEnable bit 1)." },
			{ 0x118, "FrameCounter",    RegisterAccess::Read,            0x0, false, "Vertical blanks since the start (32 bits, wraps)." },
			{ 0x11C, "BackgroundColor", RegisterAccess::ReadWrite,       0x0, false, "0x00RRGGBB, behind every plane." },
			{ 0x120, "Present",         RegisterAccess::Write,           0x0, false, "1 applies the pending bases at the next vertical blank." },
			{ 0x200, "TextEnable",      RegisterAccess::ReadWrite,       0x1, false, "1 shows the text plane." },
			{ 0x204, "TextCols",        RegisterAccess::Read,            0x0, false, "Columns of cells: Width / 8." },
			{ 0x208, "TextRows",        RegisterAccess::Read,            0x0, false, "Rows of cells: Height / 16." },
			{ 0x20C, "CellsBase",       RegisterAccess::ReadWrite,       0x0, false, "The first cell, in VRAM (a Present applies it)." },
			{ 0x210, "CellFormat",      RegisterAccess::ReadWrite,       0x0, false, "0 16-bit cells (glyph, ink, background), 1 32-bit." },
			{ 0x214, "FontBase",        RegisterAccess::ReadWrite,       0x0, false, "The font, 16 bytes a glyph (a Present applies it)." },
			{ 0x218, "GlyphCount",      RegisterAccess::ReadWrite,       256, false, "Glyphs in the font, 1-256; the others draw blank." },
			{ 0x21C, "TextPaletteBase", RegisterAccess::ReadWrite,       0x0, false, "256 entries of 0x00RRGGBB (a Present applies it)." },
			{ 0x220, "CursorX",         RegisterAccess::ReadWrite,       0x0, false, "The cursor's column." },
			{ 0x224, "CursorY",         RegisterAccess::ReadWrite,       0x0, false, "The cursor's row." },
			{ 0x228, "CursorShape",     RegisterAccess::ReadWrite,       0x0, false, "Bits 1:0 none, underline, block or bar; bit 8 blinks." },
			{ 0x22C, "ScrollbackBase",  RegisterAccess::ReadWrite,       0x0, false, "The ring of rows that scrolled off the top." },
			{ 0x230, "ScrollbackLines", RegisterAccess::ReadWrite,       0x0, false, "How many rows the ring holds." },
			{ 0x234, "ScrollY",         RegisterAccess::ReadWrite,       0x0, false, "Rows of the ring shown at the top; 0 shows the live screen." },
			{ 0x238, "ScrollbackHead",  RegisterAccess::ReadWrite,       0x0, false, "The row of the ring the next scrolled-off line goes in." },
			{ 0x23C, "ScrollbackCount", RegisterAccess::ReadWrite,       0x0, false, "The rows the ring holds now." },
			{ 0x240, "BitmapEnable",    RegisterAccess::ReadWrite,       0x0, false, "1 shows the bitmap plane (in Mode 1 and above)." },
			{ 0x244, "Base",            RegisterAccess::ReadWrite,       0x0, false, "The picture shown (a Present applies a write)." },
			{ 0x248, "BackBase",        RegisterAccess::ReadWrite,       0x0, false, "The picture to draw into." },
			{ 0x24C, "Pitch",           RegisterAccess::ReadWrite,       0x0, false, "Bytes from a row to the next." },
			{ 0x250, "Format",          RegisterAccess::ReadWrite,       0x6, false, "0 I1, 1 I2, 2 I4, 3 I8, 4 RGB565, 5 ARGB1555, 6 XRGB8888, 7 ARGB8888." },
			{ 0x254, "BitmapWidth",     RegisterAccess::ReadWrite,       0x0, false, "The picture's width in pixels." },
			{ 0x258, "BitmapHeight",    RegisterAccess::ReadWrite,       0x0, false, "The picture's height in pixels." },
			{ 0x25C, "ScrollX",         RegisterAccess::ReadWrite,       0x0, false, "The picture's column at the left edge (it wraps round)." },
			{ 0x260, "BitmapScrollY",   RegisterAccess::ReadWrite,       0x0, false, "The picture's row at the top (it wraps round)." },
			{ 0x264, "Buffers",         RegisterAccess::ReadWrite,       0x1, false, "1-3 pictures that take turns; a Present flips them." },
			{ 0x268, "PaletteBase",     RegisterAccess::ReadWrite,       0x0, false, "256 entries of 0x00RRGGBB for I1-I8 (a Present applies a write)." },
			{ 0x26C, "SpareBase",       RegisterAccess::ReadWrite,       0x0, false, "The third picture, with Buffers 3." },
			{ 0x280, "CopySrc",         RegisterAccess::ReadWrite,       0x0, false, "Where a copy reads, in RAM or VRAM." },
			{ 0x284, "CopyDst",         RegisterAccess::ReadWrite,       0x0, false, "Where a copy or a fill writes." },
			{ 0x288, "CopyLength",      RegisterAccess::ReadWrite,       0x0, false, "Bytes." },
			{ 0x28C, "FillValue",       RegisterAccess::ReadWrite,       0x0, false, "The 32-bit pattern a fill repeats, aligned to the address." },
			{ 0x290, "CopyCommand",     RegisterAccess::Write,           0x0, false, "1 copy, 2 fill; done after its GPU cycles (interrupt 34)." },
			{ 0x294, "CopyStatus",      RegisterAccess::Read,            0x0, false, "Bit 0 busy, bit 1 the last command faulted." },
			{ 0x300, "TilePaletteBase", RegisterAccess::ReadWrite,       0x0, false, "The tile layers' palette: 256 entries of 0x00RRGGBB, 16 banks of 16 in 4 bpp." },
			{ 0x304, "SpritePaletteBase", RegisterAccess::ReadWrite,     0x0, false, "The sprites' palette, the same way." },
			{ 0x308, "OamBase",         RegisterAccess::ReadWrite,       0x0, false, "The 128 sprites, 16 bytes each." },
			{ 0x30C, "SpriteTileBase",  RegisterAccess::ReadWrite,       0x0, false, "The sprites' pictures: an entry's graphic is at this plus 32 times its number." },
			{ 0x310, "SpriteControl",   RegisterAccess::ReadWrite,       0x0, false, "Bit 0 the sprites on." },
			{ 0x314, "SpriteStatus",    RegisterAccess::Read,            0x0, false, "Of the last frame: bit 0 a line had more sprites than the limit, 31:16 the first." },
			{ 0x318, "SpriteLimit",     RegisterAccess::Read,            0x0, false, "Sprites a line, the profile's." },
			{ 0x31C, "LineTableBase",   RegisterAccess::ReadWrite,       0x0, false, "The line table: 8-byte entries of (line | register << 16, value)." },
			{ 0x320, "LineTableCount",  RegisterAccess::ReadWrite,       0x0, false, "Its entries, up to 8192; 0 turns it off." },
			LAYER(0, 0x340), LAYER(1, 0x360), LAYER(2, 0x380), LAYER(3, 0x3A0),
			{ 0x3C0, "AffineControl",   RegisterAccess::ReadWrite,       0x0, false, "As a layer's, and bit 11 repeats the map (without it, outside is transparent)." },
			{ 0x3C4, "AffineMapBase",   RegisterAccess::ReadWrite,       0x0, false, "The affine layer's map." },
			{ 0x3C8, "AffineTileBase",  RegisterAccess::ReadWrite,       0x0, false, "Its tiles." },
			{ 0x3CC, "AffineOriginX",   RegisterAccess::ReadWrite,       0x0, false, "The map's x under the screen's top-left pixel, signed 24.8." },
			{ 0x3D0, "AffineOriginY",   RegisterAccess::ReadWrite,       0x0, false, "Its y." },
			{ 0x3D4, "AffineMatrixAB",  RegisterAccess::ReadWrite,       0x100, false, "pa in bits 15:0 (x per screen x), pb in 31:16 (x per screen y), signed 8.8." },
			{ 0x3D8, "AffineMatrixCD",  RegisterAccess::ReadWrite,       0x1000000, false, "pc in bits 15:0 (y per screen x), pd in 31:16 (y per screen y)." },
		};

#undef LAYER

		constexpr RegisterMap Map{ "gpu", Registers };
	}

	const RegisterMap& GpuDevice::registers() const { return Map; }

	void GpuDevice::configure(const Config& config) noexcept
	{
		_config = config;
		_config.maxWidth = std::max<u32>(8, config.maxWidth);
		_config.maxHeight = std::max<u32>(16, config.maxHeight);
		_display.setRefresh(config.refresh);
	}

	void GpuDevice::attachTo(MmioBus& bus)
	{
		bus.attach(default_mmio::Gpu, *this);
		if (_config.vramInVblankOnly)
			vram().setWriteGate(this);
		reset();
	}

	void GpuDevice::detachFrom(MmioBus& bus)
	{
		if (vram().writeGate() == this)
			vram().setWriteGate(nullptr);
		bus.detach(default_mmio::Gpu);
	}

	bool GpuDevice::admitCpuStore(u32 physical) noexcept
	{
		if ((_control & ControlDisplayOn) == 0 || _display.inVblank(now()))
			return true;
		fault(FaultVramBusy, physical);
		return false;
	}

	u64 GpuDevice::now() const noexcept
	{
		const Scheduler* clock = scheduler();
		return clock != nullptr ? clock->now() : 0;
	}

	u32 GpuDevice::availableLevel() const noexcept
	{
		return std::min(_config.maxLevel, ImplementedLevel);
	}

	void GpuDevice::reset()
	{
		_mode = 0;
		_control = ControlDisplayOn;
		_irqEnable = 0;
		_irqStatus = 0;
		_faultCode = 0;
		_faultAddress = 0;
		_lineCompare = 0;
		_background = 0;
		_presentPending = false;
		_frameCounter = 0;
		_nextFrame = 0;
		if (const Scheduler* clock = scheduler())
			_display.setClock(clock->clockHz());
		_display.restart(now());
		setResolution(std::min(BootWidth, _config.maxWidth), std::min(BootHeight, _config.maxHeight));
		// The VRAM as the machine starts (plan/v2 SPEC 7.4); only once attached, when there is a VRAM to write.
		if (scheduler() != nullptr)
		{
			_text.reset(video::TextPlane::bootLayout(_config.maxWidth / video::TextPlane::CellWidth, _config.maxHeight / video::TextPlane::CellHeight, vram().size()),
				_width / video::TextPlane::CellWidth, _height / video::TextPlane::CellHeight);
			_text.writeBootData(vram());
			const video::TextPlane::Layout& layout = _text.layout();
			_bitmap.reset(_width, _height, (layout.scrollbackBase + layout.scrollbackBytes + 255) & ~255u, layout.paletteBase, vram().size());
		}
		_copy.reset();
		_retro.reset(_config.spritesPerLine);
		if (Scheduler* events = scheduler())
			events->cancel(*this, CopyEvent);
		scheduleVblank();
		scheduleLine();
		_scanned.clear();
		beginScan();
	}

	void GpuDevice::setResolution(u32 width, u32 height)
	{
		_width = std::clamp<u32>(width, 8, _config.maxWidth);
		_height = std::clamp<u32>(height, 16, _config.maxHeight);
		_display.setHeight(_height);
		if (_text.cols() != _width / video::TextPlane::CellWidth || _text.rows() != _height / video::TextPlane::CellHeight)
			_text.setGeometry(_width / video::TextPlane::CellWidth, _height / video::TextPlane::CellHeight);
	}

	void GpuDevice::scheduleVblank()
	{
		if (Scheduler* events = scheduler())
			events->schedule(*this, _display.vblankStart(_nextFrame), VblankEvent);
	}

	// The line interrupt is an event only while it is enabled and names a line the frame has.
	void GpuDevice::scheduleLine()
	{
		Scheduler* events = scheduler();
		if (events == nullptr)
			return;
		if ((_irqEnable & IrqLine) != 0 && _lineCompare < _display.linesTotal())
			events->schedule(*this, _display.nextLineStart(now(), _lineCompare), LineEvent);
		else
			events->cancel(*this, LineEvent);
	}

	// What the frame just scanned did with the profile's limit of sprites a line (SpriteStatus): each stretch of lines
	// with its own OAM, limit and level, the first line over the limit being the one reported.
	void GpuDevice::countSprites()
	{
		bool read = false;
		u32 readBase = 0;
		for (usize i = 0; i < _scanning.size(); ++i)
		{
			const video::LineState& lines = _scanning[i];
			const u32 end = std::min(i + 1 < _scanning.size() ? _scanning[i + 1].firstLine : _height, _height);
			if (lines.mode < 2 || !lines.displayOn || !lines.retro.spritesOn() || lines.firstLine >= end)
				continue;
			if (!read || readBase != lines.retro.oamBase())
			{
				video::readOam(vram(), lines.retro.oamBase(), _sprites);
				readBase = lines.retro.oamBase();
				read = true;
			}
			const video::SpriteOverflow overflow = video::findOverflow(_sprites, lines.firstLine, end, lines.retro.spriteLimit());
			if (overflow.overflowed)
			{
				_retro.setSpriteStatus(true, overflow.firstLine);
				return;
			}
		}
		_retro.setSpriteStatus(false, 0);
	}

	// Everything the scanout reads but the text plane, as the registers hold it now.
	video::LineState GpuDevice::liveState(u32 firstLine) const
	{
		return video::LineState{ .firstLine = firstLine, .mode = _mode, .displayOn = (_control & ControlDisplayOn) != 0,
			.background = _background, .bitmap = _bitmap, .retro = _retro };
	}

	// A new frame's scan, from its line 0 with the registers as they are.
	void GpuDevice::beginScan()
	{
		_scanFrame = _nextFrame;
		_scanLine = 0;
		_tableCursor = 0;
		_scanning.clear();
		_scanning.push_back(liveState(0));
	}

	// The scan reaches the machine's cycle: every line started before it is done, the line table applied at each. A line
	// that starts on this very cycle is not: what is written now is in time for it.
	void GpuDevice::catchUp()
	{
		const Scheduler* clock = scheduler();
		if (clock == nullptr)
			return;
		const u64 cycle = clock->now();
		const u64 frame = _display.frameAt(cycle);
		if (frame < _scanFrame)
			return;   // the blank before the frame: nothing of it is scanned yet
		if (frame > _scanFrame)
		{
			scanTo(_height);
			return;
		}
		const u32 line = _display.lineAt(cycle);
		scanTo(std::min(cycle > _display.lineStart(frame, line) ? line + 1 : line, _height));
	}

	void GpuDevice::scanTo(u32 line)
	{
		for (; _scanLine < line; ++_scanLine)
			applyLineTable(_scanLine);
	}

	// The entries of `line` in the line table, written as the CPU would write them (plan/v2 SPEC 7.5). The table is read
	// in order; one whose line has passed is skipped, and it ends where the VRAM does.
	void GpuDevice::applyLineTable(u32 line)
	{
		const u32 count = _retro.lineTableCount();
		bool changed = false;
		while (_tableCursor < count)
		{
			const u64 at = u64{ _retro.lineTableBase() } + u64{ _tableCursor } * video::Retro2D::LineTableEntrySize;
			if (at > 0xFFFFFFFFull || !vram().backs(static_cast<u32>(at), video::Retro2D::LineTableEntrySize))
			{
				_tableCursor = count;
				break;
			}
			const u32 offset = static_cast<u32>(at) - Vram::BaseValue;
			const u32 head = vram().read<u32>(offset);
			if ((head & 0xFFFFu) > line)
				break;
			++_tableCursor;
			if ((head & 0xFFFFu) < line)
				continue;
			const u32 target = (head >> 16) & 0xFFFu;
			if ((target & 3u) != 0)
				continue;
			const u32 value = vram().read<u32>(offset + 4);
			if (target == BackgroundColorRegister.value())
				_background = value & 0x00FFFFFFu;
			else if (target == video::BitmapPlane::ScrollXRegister || target == video::BitmapPlane::ScrollYRegister)
				_bitmap.write(target, value);
			else if (video::Retro2D::lineTableWrites(target))
				_retro.write(target, value);
			else
				continue;
			changed = true;
		}
		if (changed)
			noteChange();
	}

	// A register may have changed: from the next line to scan, the lines have the registers as they are now. Nothing is
	// kept when nothing the scanout reads changed, nor once the visible lines are done (the next frame starts from the
	// registers as the vertical blank leaves them).
	void GpuDevice::noteChange()
	{
		if (_scanLine >= _height || _scanning.empty())
			return;
		video::LineState now = liveState(_scanLine);
		if (now.sameAs(_scanning.back()))
			return;
		if (_scanning.back().firstLine == _scanLine)
			_scanning.back() = now;
		else
			_scanning.push_back(now);
	}

	void GpuDevice::vblank()
	{
		scanTo(_height);
		countSprites();
		_scanned.swap(_scanning);
		_scannedWidth = _width;
		_scannedHeight = _height;
		_frameCounter = _nextFrame + 1;
		_scannedNumber = _frameCounter;
		++_nextFrame;
		const bool presented = _presentPending;
		_presentPending = false;
		if (presented)
		{
			_text.applyPending();
			_bitmap.applyPending();
		}
		beginScan();
		_irqStatus |= IrqVblank;
		if ((_irqEnable & IrqVblank) != 0)
			raiseInterrupt(VblankInterrupt);
		scheduleVblank();
		if (_vblankObserver)
			_vblankObserver(presented);
	}

	void GpuDevice::fault(u32 code, u32 address)
	{
		_faultCode = code;
		_faultAddress = address;
		_irqStatus |= IrqFault;
		if ((_irqEnable & IrqFault) != 0)
			raiseInterrupt(FaultInterrupt);
	}

	void GpuDevice::onEvent(u32 tag, u64 cycle)
	{
		switch (tag)
		{
			case VblankEvent:
				vblank();
				break;
			case CopyEvent:
				_copy.finish(memory(), vram());
				_irqStatus |= IrqCopy;
				if ((_irqEnable & IrqCopy) != 0)
					raiseInterrupt(CopyInterrupt);
				break;
			case LineEvent:
				_irqStatus |= IrqLine;
				if ((_irqEnable & IrqLine) != 0)
					raiseInterrupt(LineInterrupt);
				if (Scheduler* events = scheduler(); events != nullptr && (_irqEnable & IrqLine) != 0)
					events->schedule(*this, _display.nextLineStart(cycle + 1, _lineCompare), LineEvent);
				break;
			default:
				break;
		}
	}

	void GpuDevice::compose(video::VideoFrame& frame)
	{
		const video::LineState now = liveState(0);
		video::ScanoutState state;
		state.width = _width;
		state.height = _height;
		state.frameCounter = _frameCounter;
		state.text = &_text;
		state.lines = std::span<const video::LineState>(&now, 1);
		_executor->compose(state, vram(), frame);
	}

	void GpuDevice::composeScanned(video::VideoFrame& frame)
	{
		if (_scanned.empty())
		{
			compose(frame);
			return;
		}
		video::ScanoutState state;
		state.width = _scannedWidth;
		state.height = _scannedHeight;
		state.frameCounter = _scannedNumber;
		state.text = &_text;
		state.lines = _scanned;
		_executor->compose(state, vram(), frame);
	}

	u32 GpuDevice::read(Address offset)
	{
		catchUp();
		return readRegister(offset.value());
	}

	void GpuDevice::write(Address offset, u32 value)
	{
		catchUp();
		writeRegister(offset.value(), value);
		noteChange();
	}

	u32 GpuDevice::readRegister(u32 offset)
	{
		if (video::TextPlane::handles(offset))
			return _text.read(offset);
		if (video::BitmapPlane::handles(offset))
			return _bitmap.read(offset);
		if (video::CopyEngine::handles(offset))
			return _copy.read(offset);
		if (video::Retro2D::handles(offset))
			return _retro.read(offset);
		switch (offset)
		{
			case IdRegister.value(): return IdValue;
			case VersionRegister.value(): return VersionValue;
			case CapsRegister.value():
			{
				u32 caps = 0;
				for (u32 level = 0; level <= availableLevel(); ++level)
					caps |= 1u << level;
				if (availableLevel() >= 1)
					caps |= ((1u << video::PixelFormatCount) - 1) << 8;
				return caps;
			}
			case VramSizeRegister.value(): return static_cast<u32>(vram().size());
			case GpuClockHzRegister.value(): return static_cast<u32>(std::min<u64>(_config.gpuClockHz, 0xFFFFFFFFull));
			case ModeRegister.value(): return _mode;
			case MaxLevelRegister.value(): return _config.maxLevel;
			case ControlRegister.value(): return _control;
			case StatusRegister.value():
				return (_copy.busy() ? StatusBusy : 0u) | (_faultCode != 0 ? StatusFault : 0u) | (_display.inVblank(now()) ? StatusVblank : 0u) | (_presentPending ? StatusFlipPending : 0u);
			case IrqEnableRegister.value(): return _irqEnable;
			case IrqStatusRegister.value(): return _irqStatus;
			case FaultCodeRegister.value(): return _faultCode;
			case FaultAddressRegister.value(): return _faultAddress;
			case WidthRegister.value(): return _width;
			case HeightRegister.value(): return _height;
			case RefreshRegister.value(): return _display.refresh();
			case LinesTotalRegister.value(): return _display.linesTotal();
			case VCountRegister.value(): return _display.lineAt(now());
			case LineCompareRegister.value(): return _lineCompare;
			case FrameCounterRegister.value(): return static_cast<u32>(_frameCounter);
			case BackgroundColorRegister.value(): return _background;
			default: return 0;
		}
	}

	void GpuDevice::writeRegister(u32 offset, u32 value)
	{
		if (video::TextPlane::handles(offset))
		{
			_text.write(offset, value);
			return;
		}
		if (video::BitmapPlane::handles(offset))
		{
			_bitmap.write(offset, value);
			return;
		}
		if (video::CopyEngine::handles(offset))
		{
			const video::CopyEngine::Start start = _copy.write(offset, value, memory(), vram());
			if (start.fault)
				fault(FaultBadAddress, start.faultAddress);
			else if (start.started)
				if (Scheduler* events = scheduler())
					events->schedule(*this, now() + video::CostModel::toCpuCycles(start.gpuCycles, events->clockHz(), _config.gpuClockHz), CopyEvent);
			return;
		}
		if (video::Retro2D::handles(offset))
		{
			_retro.write(offset, value);
			return;
		}
		switch (offset)
		{
			case ModeRegister.value():
				_mode = std::min(value, availableLevel());
				break;
			case ControlRegister.value():
				if ((value & ControlReset) != 0)
				{
					reset();
					break;
				}
				_control = value & (ControlDisplayOn | ControlCommandProcessor);
				break;
			case IrqEnableRegister.value():
				_irqEnable = value & (IrqVblank | IrqLine | IrqCopy | IrqFault);
				scheduleLine();
				break;
			case IrqStatusRegister.value():
				_irqStatus &= ~value;
				if ((value & IrqFault) != 0)
					_faultCode = 0;   // acknowledging the fault clears it
				break;
			case WidthRegister.value():
				setResolution(value, _height);
				scheduleVblank();
				scheduleLine();
				break;
			case HeightRegister.value():
				setResolution(_width, value);
				scheduleVblank();
				scheduleLine();
				break;
			case LineCompareRegister.value():
				_lineCompare = value;
				scheduleLine();
				break;
			case BackgroundColorRegister.value():
				_background = value & 0x00FFFFFFu;
				break;
			case PresentRegister.value():
				if ((value & 1u) != 0)
				{
					_presentPending = true;
					_bitmap.present();
				}
				break;
			default:
				break;
		}
	}
}
