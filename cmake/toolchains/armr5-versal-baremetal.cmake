# Bare-metal ARMv7-R toolchain for AMD/Xilinx Versal Lockstep Cortex-R5F (Class B FDIR / PMC bridge)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(STRATA_ARCH_TRIPLE "arm-none-eabi" CACHE STRING "ARM Cortex-R5F bare-metal triplet")
set(CMAKE_C_COMPILER   "${STRATA_ARCH_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${STRATA_ARCH_TRIPLE}-g++")
set(CMAKE_ASM_COMPILER "${STRATA_ARCH_TRIPLE}-gcc")
set(CMAKE_OBJCOPY      "${STRATA_ARCH_TRIPLE}-objcopy")
set(CMAKE_SIZE         "${STRATA_ARCH_TRIPLE}-size")

set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-r5 -mfloat-abi=hard -mfpu=vfpv3-d16 -ffreestanding -fno-common")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
