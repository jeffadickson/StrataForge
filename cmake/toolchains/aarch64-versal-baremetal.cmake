# Bare-metal AArch64 toolchain for AMD/Xilinx Versal Cortex-A72 (EL2 hypervisor / EL1 guests)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(STRATA_ARCH_TRIPLE "aarch64-none-elf" CACHE STRING "AArch64 bare-metal triplet")
set(CMAKE_C_COMPILER   "${STRATA_ARCH_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${STRATA_ARCH_TRIPLE}-g++")
set(CMAKE_ASM_COMPILER "${STRATA_ARCH_TRIPLE}-gcc")
set(CMAKE_OBJCOPY      "${STRATA_ARCH_TRIPLE}-objcopy")
set(CMAKE_SIZE         "${STRATA_ARCH_TRIPLE}-size")

set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a72 -mstrict-align -ffreestanding -fno-common")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
