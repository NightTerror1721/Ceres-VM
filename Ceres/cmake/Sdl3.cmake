# SDL3, pulled in only when a windowed host is wanted.
#
# Ceres builds with nothing but a C++23 compiler today, and that must stay true: SDL3 is an opt-in
# dependency (CERES_ENABLE_SDL), off by default. When it is on, prefer a system install and fall
# back to building SDL3 from source at a pinned release tag - self-contained, so the CI that has
# never seen SDL can still produce the windowed host.

include_guard(GLOBAL)
include(FetchContent)

find_package(SDL3 QUIET)

if(NOT SDL3_FOUND)
	FetchContent_Declare(SDL3
		GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
		GIT_TAG        release-3.4.16
		GIT_SHALLOW    TRUE
		OVERRIDE_FIND_PACKAGE)
	FetchContent_MakeAvailable(SDL3)
endif()
