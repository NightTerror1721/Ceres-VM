# ceres-sdl - the windowed host: a window that is the machine's input (HostInput), a presenter that shows its
# frames (VideoOutput) and the speakers (AudioOutput). The only library in the tree that links SDL3,
# and the only one that must not build unless CERES_ENABLE_SDL is on.

ceres_add_library(sdl DEPENDS driver devices)

target_link_libraries(ceres_sdl PUBLIC SDL3::SDL3)
