cmake_minimum_required(VERSION 3.20)

project(HubVolume VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(HubVolume WIN32
  src/HubVolume.cpp
  src/HubVolume.rc
)

target_compile_definitions(HubVolume PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(HubVolume PRIVATE src)

if (MSVC)
  # CRT statis agar satu exe berjalan tanpa Visual C++ Redistributable.
  set_property(TARGET HubVolume PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_options(HubVolume PRIVATE /utf-8 /W4 /permissive- /O2)
else()
  target_compile_options(HubVolume PRIVATE -Wall -Wextra -O2)
  target_link_options(HubVolume PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(HubVolume PRIVATE
  gdiplus ole32 shell32 advapi32 version dwmapi gdi32 user32 uuid
)

set_target_properties(HubVolume PROPERTIES OUTPUT_NAME "HubVolume")
