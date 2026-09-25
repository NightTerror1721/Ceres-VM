// The blitter (BlitterDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

TEST(blitter, the_blitter_fills_copies_keys_scales_and_indexes)
{
	CeresVM vm{ Memory::DefaultSize };
	BlitterDevice blitter{};
	blitter.attachTo(vm.io());
	auto px = [&](u32 address) { return vm.memory().readUnchecked<u32>(Address(address)); };
	using B = BlitterDevice;
	// A 4x3 destination surface at 0x10000 (stride 16), filled.
	blitter.write(B::DstAddressRegister, 0x10000);
	blitter.write(B::DstStrideRegister, 16);
	blitter.write(B::WidthRegister, 4);
	blitter.write(B::HeightRegister, 3);
	blitter.write(B::ColorRegister, 0x00123456u);
	blitter.write(B::CommandRegister, B::CommandFill);
	CHECK_EQ(blitter.read(B::PixelsRegister), 12u);
	CHECK(px(0x10000) == 0x00123456u && px(0x10000 + 2 * 16 + 12) == 0x00123456u);
	// A 2x1 sprite with a transparent pixel, copied keyed at (1, 1).
	vm.memory().writeUnchecked<u32>(Address(0x20000), 0x00FF00FFu);          // the key
	vm.memory().writeUnchecked<u32>(Address(0x20004), 0x00ABCDEFu);
	blitter.write(B::SrcAddressRegister, 0x20000);
	blitter.write(B::SrcStrideRegister, 8);
	blitter.write(B::DstAddressRegister, 0x10000 + 16 + 4);
	blitter.write(B::WidthRegister, 2);
	blitter.write(B::HeightRegister, 1);
	blitter.write(B::ColorRegister, 0x00FF00FFu);
	blitter.write(B::CommandRegister, B::CommandCopyKeyed);
	CHECK_EQ(blitter.read(B::PixelsRegister), 1u);
	CHECK(px(0x10000 + 16 + 4) == 0x00123456u && px(0x10000 + 16 + 8) == 0x00ABCDEFu);
	// Scaled x2: one source pixel becomes a 2x2 block.
	blitter.write(B::SrcAddressRegister, 0x20004);
	blitter.write(B::DstAddressRegister, 0x30000);
	blitter.write(B::DstStrideRegister, 8);
	blitter.write(B::WidthRegister, 1);
	blitter.write(B::HeightRegister, 1);
	blitter.write(B::ScaleRegister, 2);
	blitter.write(B::CommandRegister, B::CommandCopyScaled);
	CHECK_EQ(blitter.read(B::PixelsRegister), 4u);
	CHECK(px(0x30000) == 0x00ABCDEFu && px(0x30004) == 0x00ABCDEFu && px(0x30008) == 0x00ABCDEFu && px(0x3000C) == 0x00ABCDEFu);
	// Indexed through a palette, index 0 left out.
	vm.memory().writeUnchecked<u32>(Address(0x40000 + 3 * 4), 0x00777777u);  // palette[3]
	vm.memory().writeUnchecked<u8>(Address(0x50000), 3);
	vm.memory().writeUnchecked<u8>(Address(0x50001), 0);
	blitter.write(B::PaletteAddressRegister, 0x40000);
	blitter.write(B::SrcAddressRegister, 0x50000);
	blitter.write(B::SrcStrideRegister, 2);
	blitter.write(B::DstAddressRegister, 0x10000);
	blitter.write(B::DstStrideRegister, 16);
	blitter.write(B::WidthRegister, 2);
	blitter.write(B::ColorRegister, 0);
	blitter.write(B::CommandRegister, B::CommandCopyIndexedKeyed);
	CHECK(px(0x10000) == 0x00777777u && px(0x10004) == 0x00123456u);
	// An overlapping copy one row down: rows go bottom-up, so every row arrives intact.
	blitter.write(B::SrcAddressRegister, 0x10000);
	blitter.write(B::SrcStrideRegister, 16);
	blitter.write(B::DstAddressRegister, 0x10010);
	blitter.write(B::WidthRegister, 4);
	blitter.write(B::HeightRegister, 2);
	blitter.write(B::CommandRegister, B::CommandCopy);
	CHECK(px(0x10010) == 0x00777777u && px(0x10020 + 8) == 0x00ABCDEFu);
	// Outside RAM: the error bit, and the interrupt when asked for.
	blitter.write(B::ControlRegister, B::ControlInterrupt);
	blitter.write(B::DstAddressRegister, static_cast<u32>(vm.memory().size()) - 8);
	blitter.write(B::CommandRegister, B::CommandFill);
	CHECK_EQ(blitter.read(B::StatusRegister), B::StatusError);
	CHECK((vm.interrupts().pendingMask() & (u64{ 1 } << static_cast<u8>(B::Interrupt))) != 0);
}
