#include <ceres/devices/system/dma.h>
#include <algorithm>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Source",      RegisterAccess::Write,     0x0, false, "Source physical address." },
			{ 0x04, "Destination", RegisterAccess::Write,     0x0, false, "Destination physical address." },
			{ 0x08, "Length",      RegisterAccess::Write,     0x0, false, "Bytes to move." },
			{ 0x0C, "Command",     RegisterAccess::Write,     0x0, false, "1 starts the transfer latched above." },
			{ 0x10, "Status",      RegisterAccess::Read,      0x0, false, "Busy / Done bits." },
			{ 0x14, "Transferred", RegisterAccess::Read,      0x0, false, "Bytes the last completed transfer actually moved." },
		};
	}

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

	u32 DmaController::read(Address offset)
	{
		if (offset == StatusRegister)
			return _status;
		if (offset == TransferredRegister)
			return _transferred;
		return 0;
	}

	void DmaController::write(Address offset, u32 value)
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

	const RegisterMap& DmaController::registers() const
	{
		static constexpr RegisterMap map{ "dma", Registers };
		return map;
	}
}
