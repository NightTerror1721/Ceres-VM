# ceres-sdl - the windowed host. A HostBackend that puts the display's pixels in a real window and
# feeds real keyboard/mouse events into the devices. The only library in the tree that links SDL3,
# and the only one that must not build unless CERES_ENABLE_SDL is on.

ceres_add_library(sdl DEPENDS driver devices)

target_link_libraries(ceres_sdl PUBLIC SDL3::SDL3)
