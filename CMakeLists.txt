cmake_minimum_required(VERSION 3.20)
project(ryuuostinstaller CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ryuuostinstaller main.cpp)
target_compile_definitions(ryuuostinstaller PRIVATE UNICODE _UNICODE)
target_link_libraries(ryuuostinstaller PRIVATE winhttp bcrypt shell32 advapi32)

if(MSVC)
  # Original was built with the static CRT and /guard:cf
  set_property(TARGET ryuuostinstaller PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(ryuuostinstaller PRIVATE /W4 /EHsc /utf-8 /guard:cf)
  target_link_options(ryuuostinstaller PRIVATE /GUARD:CF)
endif()
