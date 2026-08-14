cmake_minimum_required(VERSION 3.15)
project(CalmDownGPU CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "CalmDownGPU is Windows-only.")
endif()

add_executable(CalmDownGPU WIN32
    main.cpp
    resource.rc
)

target_link_libraries(CalmDownGPU PRIVATE
    shell32 user32 gdi32
)

if(MSVC)
    target_compile_options(CalmDownGPU PRIVATE /W4 /utf-8)
    # Manifest is embedded via resource.rc; suppress the linker's auto-manifest.
    set_target_properties(CalmDownGPU PROPERTIES LINK_FLAGS "/MANIFEST:NO")
else()
    target_compile_options(CalmDownGPU PRIVATE -municode -Wall -Wextra)
endif()
