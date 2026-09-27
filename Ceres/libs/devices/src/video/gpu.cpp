#include <ceres/devices/video/gpu.h>
#include <ceres/devices/video/formats.h>

#include <algorithm>

namespace ceres::devices
{
	namespace
	{
		// Every register of the core and the display (plan/v2 SPEC 7.3), in offset order.
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
			{ 0x02C, "FaultCode",       RegisterAccess::Read,            0x0, false, "Why the GPU faulted: 1 an engine's address outside RAM and VRAM." },
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
		};

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
		reset();
	}

	void GpuDevice::detachFrom(MmioBus& bus)
	{
		bus.detach(default_mmio::Gpu);
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
		scheduleVblank();
		scheduleLine();
	}

	void GpuDevice::setResolution(u32 width, u32 height)
	{
		_width = std::clamp<u32>(width, 8, _config.maxWidth);
		_height = std::clamp<u32>(height, 16, _config.maxHeight);
		_display.setHeight(_height);
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

	void GpuDevice::vblank()
	{
		_frameCounter = _nextFrame + 1;
		++_nextFrame;
		const bool presented = _presentPending;
		_presentPending = false;
		_irqStatus |= IrqVblank;
		if ((_irqEnable & IrqVblank) != 0)
			raiseInterrupt(VblankInterrupt);
		scheduleVblank();
		if (_vblankObserver)
			_vblankObserver(presented);
	}

	void GpuDevice::onEvent(u32 tag, u64 cycle)
	{
		switch (tag)
		{
			case VblankEvent:
				vblank();
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
		video::ScanoutState state;
		state.width = _width;
		state.height = _height;
		state.mode = _mode;
		state.displayOn = (_control & ControlDisplayOn) != 0;
		state.background = _background;
		state.frameCounter = _frameCounter;
		_executor->compose(state, vram(), frame);
	}

	u32 GpuDevice::read(Address offset)
	{
		switch (offset.value())
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
				return (_faultCode != 0 ? StatusFault : 0u) | (_display.inVblank(now()) ? StatusVblank : 0u) | (_presentPending ? StatusFlipPending : 0u);
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

	void GpuDevice::write(Address offset, u32 value)
	{
		switch (offset.value())
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
					_presentPending = true;
				break;
			default:
				break;
		}
	}
}
