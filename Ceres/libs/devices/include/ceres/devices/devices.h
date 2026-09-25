#pragma once

// Every device, for a user that wants them all. Each device has a header of its own, grouped by what it does:
// audio/, input/, storage/, system/, terminal/, video/ (plan/v2 F1).
//
// Every device follows the same register layout convention: scalar registers sit at low,
// word-aligned offsets (0x00, 0x04, 0x08, ...) within the device's 64 KiB MMIO slot - the direct
// replacement for what used to be a handful of single-byte port numbers, just with room to
// spare. A device that also moves blocks of memory (the disk, the framebuffer) additionally
// claims three registers near the top of its slot - BLOCK_ADDR/BLOCK_LEN/BLOCK_CMD at
// 0xF0/0xF4/0xF8 - so a bulk transfer is still one MMIO write to trigger, the same shape `outm`/
// `inm` used to give it, just addressed like everything else now.

#include <ceres/devices/audio/audio.h>
#include <ceres/devices/input/gamepad.h>
#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/input/mouse.h>
#include <ceres/devices/storage/disk.h>
#include <ceres/devices/storage/host_fs.h>
#include <ceres/devices/storage/peripherals.h>
#include <ceres/devices/system/dma.h>
#include <ceres/devices/system/system_control.h>
#include <ceres/devices/system/timer.h>
#include <ceres/devices/terminal/terminal.h>
#include <ceres/devices/video/blitter.h>
#include <ceres/devices/video/default_font.h>
#include <ceres/devices/video/display.h>
#include <ceres/devices/video/text_framebuffer.h>
#include <ceres/devices/video/text_renderer.h>
