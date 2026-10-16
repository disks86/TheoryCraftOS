# Cross toolchain for x86_64 UEFI applications.
# mingw-w64 emits PE32+ directly and uses the Microsoft x64 calling
# convention, which is exactly the UEFI ABI.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TRIPLE x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${TRIPLE}-g++)
set(CMAKE_ASM_COMPILER ${TRIPLE}-gcc)
set(CMAKE_AR           ${TRIPLE}-ar)
set(CMAKE_RANLIB       ${TRIPLE}-ranlib)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX ".efi")
