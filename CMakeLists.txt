cmake_minimum_required(VERSION 3.16)
project(aula-f75max VERSION 0.1.0 LANGUAGES C)

# Use the C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find libusb-1.0 using pdg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

find_library(GIFLIB_LIBRARIES NAMES gif)
find_path(GIFLIB_INCLUDE_DIRS NAMES gif_lib.h)

if (NOT GIFLIB_LIBRARIES OR NOT GIFLIB_INCLUDE_DIRS)
	message(FATAL_ERROR "giflib not found. Please install")
endif()

# Collect all source files
set(SOURCES
    src/main.c
    src/device.c
    src/display.c
    src/reset.c
    src/time_sync.c
)

# Define the executable
add_executable(aula-cli ${SOURCES})

# Tell compiler where to find our headers and libusb headers
target_include_directories(aula-cli PRIVATE
    include/
    ${LIBUSB_INCLUDE_DIRS}
	${GIFLIB_INCLUDE_DIRS}
)

# Link against libusb
target_link_libraries(aula-cli PRIVATE
    ${LIBUSB_LIBRARIES}
	${GIFLIB_LIBRARIES}
)

# Installation rule - puts binary in /usr/bin
install(TARGETS aula-cli DESTINATION bin)
