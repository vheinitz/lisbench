# Cross build for Debian armhf (Raspberry Pi OS on a Pi 3B+), with Debian's
# cross compilers (g++-arm-linux-gnueabihf) and qemu-user to run the tests.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -L /usr/arm-linux-gnueabihf)
set(LISBENCH_DEB_ARCH armhf)
