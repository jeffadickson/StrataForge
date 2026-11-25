# Bare-metal MicroBlaze toolchain for AMD/Xilinx Kintex (XQRKU060 / KU+) TMR soft-core shell
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR microblaze)

set(STRATA_ARCH_TRIPLE "mb" CACHE STRING "MicroBlaze bare-metal triplet prefix")
set(CMAKE_C_COMPILER   "${STRATA_ARCH_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${STRATA_ARCH_TRIPLE}-g++")
set(CMAKE_ASM_COMPILER "${STRATA_ARCH_TRIPLE}-gcc")
set(CMAKE_OBJCOPY      "${STRATA_ARCH_TRIPLE}-objcopy")
set(CMAKE_SIZE         "${STRATA_ARCH_TRIPLE}-size")

set(CMAKE_CXX_FLAGS_INIT "-mlittle-endian -mxl-barrel-shift -mxl-pattern-compare -ffreestanding -fno-common")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
