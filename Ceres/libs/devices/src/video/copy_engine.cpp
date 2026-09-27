#include <ceres/devices/video/copy_engine.h>
#include <ceres/devices/video/cost_model.h>

#include <cstring>

namespace ceres::devices::video
{
	namespace
	{
		// Whether [address, address + length) is wholly in the RAM or wholly in the VRAM.
		bool backed(u32 address, u32 length, const vm::Memory& memory, const vm::Vram& vram) noexcept
		{
			if (length == 0)
				return true;
			if (address < vm::Vram::BaseValue)
				return u64{ address } + length <= memory.size();
			return vram.backs(address, length);
		}

		// The bytes of such a run, marked as written when they are VRAM.
		u8* bytes(u32 address, u32 length, vm::Memory& memory, vm::Vram& vram) noexcept
		{
			if (address < vm::Vram::BaseValue)
				return memory.peekMutBytesUnchecked(isa::Address(address), length).data();
			return vram.span(address - vm::Vram::BaseValue, length);
		}
	}

	u32 CopyEngine::read(u32 offset) const noexcept
	{
		switch (offset)
		{
			case SrcRegister: return _src;
			case DstRegister: return _dst;
			case LengthRegister: return _length;
			case FillValueRegister: return _fill;
			case StatusRegister: return _status;
			default: return 0;
		}
	}

	CopyEngine::Start CopyEngine::write(u32 offset, u32 value, vm::Memory& memory, const vm::Vram& vram) noexcept
	{
		switch (offset)
		{
			case SrcRegister: _src = value; break;
			case DstRegister: _dst = value; break;
			case LengthRegister: _length = value; break;
			case FillValueRegister: _fill = value; break;
			case CommandRegister:
			{
				if (busy() || (value != CommandCopy && value != CommandFill))
					break;   // one command at a time; anything else is ignored
				Start start;
				if (value == CommandCopy && !backed(_src, _length, memory, vram))
				{
					start.fault = true;
					start.faultAddress = _src;
				}
				else if (!backed(_dst, _length, memory, vram))
				{
					start.fault = true;
					start.faultAddress = _dst;
				}
				if (start.fault)
				{
					_status = StatusError;
					return start;
				}
				_command = value;
				_status = StatusBusy;
				start.started = true;
				start.gpuCycles = value == CommandCopy ? CostModel::copyCycles(_length) : CostModel::fillCycles(_length);
				return start;
			}
			default: break;
		}
		return {};
	}

	void CopyEngine::finish(vm::Memory& memory, vm::Vram& vram) noexcept
	{
		if (!busy())
			return;
		if (_length != 0)
		{
			u8* to = bytes(_dst, _length, memory, vram);
			if (_command == CommandCopy)
			{
				const u8* from = _src < vm::Vram::BaseValue ? memory.peekMutBytesUnchecked(isa::Address(_src), _length).data()
					: vram.data() + (_src - vm::Vram::BaseValue);
				std::memmove(to, from, _length);
			}
			else
				for (u32 i = 0; i < _length; ++i)
					to[i] = static_cast<u8>(_fill >> (8 * ((_dst + i) & 3)));
		}
		_status = 0;
		_command = 0;
	}
}
