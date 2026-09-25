#include <ceres/devices/system/dma.h>

namespace ceres::devices
{
	void DmaController::advance(u64 ticks)
	{
		if (ticks != 0)
			tick();
	}

	void DmaController::reset()
	{
		_pending = false;
		_status = 0;
		_transferred = 0;
	}

	void DmaController::tick()
	{
		if (!_pending)
			return;

		// A SRC/DST/LEN that runs past the end of memory is clamped rather than fatal: the
		// copy moves what fits, and TransferredRegister reports exactly how much.
		const u32 effective = std::min(
			memory().clampBlockSizeUnchecked(Address(_source), _length),
			memory().clampBlockSizeUnchecked(Address(_destination), _length));

		memory().copyBytesUnchecked(Address(_source), Address(_destination), effective);
		// RAM to RAM always moves the whole length, but a source device that yields fewer
		// bytes (a short terminal read) would land a smaller number here - which is exactly
		// what this register exists to report.
		_transferred = effective;
		_pending = false;
		_status = StatusDone;
		raiseInterrupt(Interrupt);
	}

	u32 DmaController::readUnsignedWord(Address offset)
	{
		if (offset == StatusRegister)
			return _status;
		if (offset == TransferredRegister)
			return _transferred;
		return 0;
	}

	void DmaController::writeWord(Address offset, u32 value)
	{
		if (offset == SourceRegister) { _source = value; return; }
		if (offset == DestinationRegister) { _destination = value; return; }
		if (offset == LengthRegister) { _length = value; return; }
		if (offset == CommandRegister && value == CommandStart)
		{
			_pending = true;
			_status = StatusBusy;
		}
	}
}
