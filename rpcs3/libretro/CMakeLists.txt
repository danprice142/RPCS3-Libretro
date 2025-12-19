# RPCS3 Libretro Core CMakeLists.txt

# Build as a shared library
add_library(rpcs3_libretro SHARED
    libretro_core.cpp
    libretro_video.cpp
    libretro_audio.cpp
    libretro_input.cpp
    libretro_pad_handler.cpp
    libretro_frontend.cpp
    libretro_firmware.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/rpcs3_version.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/product_info.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ps_move_config.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/ps_move_tracker.cpp
    ${CMAKE_SOURCE_DIR}/rpcs3/Input/pad_thread.cpp
)

# Set output name without 'lib' prefix on non-Windows
set_target_properties(rpcs3_libretro PROPERTIES
    OUTPUT_NAME "rpcs3_libretro"
    PREFIX ""
)

# Platform-specific suffix
if(WIN32)
    set_target_properties(rpcs3_libretro PROPERTIES SUFFIX ".dll")
elseif(APPLE)
    set_target_properties(rpcs3_libretro PROPERTIES SUFFIX ".dylib")
else()
    set_target_properties(rpcs3_libretro PROPERTIES SUFFIX ".so")
endif()

# Include directories - must match rpcs3_emu includes
target_include_directories(rpcs3_libretro PRIVATE
    ${CMAKE_SOURCE_DIR}/rpcs3
    ${CMAKE_SOURCE_DIR}/rpcs3/Emu
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/3rdparty
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib
    ${CMAKE_BINARY_DIR}/3rdparty/zlib/zlib
)

# Link against emucore and required libraries
target_link_libraries(rpcs3_libretro PRIVATE
    rpcs3_emu
    3rdparty::fusion
)

# GLEW for OpenGL extension loading
if(NOT WIN32)
    target_link_libraries(rpcs3_libretro PRIVATE 3rdparty::glew)
endif()

# OpenGL
find_package(OpenGL REQUIRED)
target_link_libraries(rpcs3_libretro PRIVATE OpenGL::GL)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(rpcs3_libretro PRIVATE
        opengl32
        ws2_32
        Iphlpapi
        Winmm
        Psapi
        gdi32
        setupapi
        DbgHelp
    )
    target_compile_definitions(rpcs3_libretro PRIVATE UNICODE _UNICODE)
elseif(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND)
        target_link_libraries(rpcs3_libretro PRIVATE X11::X11)
    endif()
    target_link_libraries(rpcs3_libretro PRIVATE ${CMAKE_DL_LIBS})
    
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(rpcs3_libretro PRIVATE Threads::Threads)
elseif(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL)
    target_link_libraries(rpcs3_libretro PRIVATE ${OPENGL_FRAMEWORK})
    target_link_libraries(rpcs3_libretro PRIVATE iconv)
endif()

# Compile definitions
target_compile_definitions(rpcs3_libretro PRIVATE
    LIBRETRO_CORE
)

# Hide symbols by default (expose only libretro API)
if(NOT MSVC)
    target_compile_options(rpcs3_libretro PRIVATE -fvisibility=hidden)
else()
    # Enable standard-conforming preprocessor for __VA_OPT__ support
    target_compile_options(rpcs3_libretro PRIVATE /Zc:preprocessor)
endif()

# C++20 standard
target_compile_features(rpcs3_libretro PRIVATE cxx_std_20)

# Use precompiled headers if enabled
if(USE_PRECOMPILED_HEADERS)
    target_precompile_headers(rpcs3_libretro PRIVATE ${CMAKE_SOURCE_DIR}/rpcs3/stdafx.h)
endif()

# Installation
install(TARGETS rpcs3_libretro
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

message(STATUS "RPCS3 Libretro core configured")
