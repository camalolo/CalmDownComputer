cmake_minimum_required(VERSION 3.20)
project(CalmDownGPU VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "CalmDownGPU is Windows-only.")
endif()

# Statically link the C/C++ runtime so the .exe runs on machines without
# the MSVC redistributable installed (Windows 10+ ships the UCRT system-wide).
option(CALMDOWN_STATIC_CRT "Statically link the C/C++ runtime" ON)

add_executable(CalmDownGPU WIN32
    main.cpp
    resource.rc
)

target_compile_definitions(CalmDownGPU PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(CalmDownGPU PRIVATE
    advapi32   # Registry persistence (RegOpenKeyExW & friends)
    shell32    # System tray (Shell_NotifyIcon)
    user32     # Win32 UI / GDI drawing context
    gdi32      # Icon rendering
)

if(MSVC)
    target_compile_options(CalmDownGPU PRIVATE
        /W4
        /utf-8
        /permissive-
        /EHsc
        $<$<CONFIG:Release>:/O2>
    )
    if(CALMDOWN_STATIC_CRT)
        # MSVC_RUNTIME_LIBRARY per-config: link /MT (static), Debug gets /MTd.
        set_property(TARGET CalmDownGPU PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # The admin-elevation manifest is embedded via resource.rc;
    # suppress the linker's default auto-generated manifest.
    set_target_properties(CalmDownGPU PROPERTIES LINK_FLAGS "/MANIFEST:NO")
elseif(MINGW)
    target_compile_options(CalmDownGPU PRIVATE
        -Wall -Wextra -municode
        $<$<CONFIG:Release>:-O2>
    )
    if(CALMDOWN_STATIC_CRT)
        target_link_options(CalmDownGPU PRIVATE -static)
    endif()
else()
    message(FATAL_ERROR "Unsupported compiler. Use MSVC or MinGW-w64.")
endif()

set_target_properties(CalmDownGPU PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS CalmDownGPU RUNTIME DESTINATION .)