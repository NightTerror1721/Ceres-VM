# ceres-devices - the peripherals.
#
# Terminal, timer, system control, disk and framebuffer: everything that implements IODevice and
# talks to the host. Separated from the VM so that adding a peripheral doesn't force touching the
# CPU, and so a build without a terminal or a disk is possible.
#
# It's the easiest of the five cuts to postpone: if it gets in the way, its sources can move back
# into ceres-vm without anything else changing.

ceres_add_library(devices DEPENDS core vm)
ceres_add_tests(devices)
