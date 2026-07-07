cmake_minimum_required(VERSION 3.20)
project(HyoExam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # Minimize binary size, maximize speed: no exceptions/RTTI overhead we don't need,
  # static CRT so there's no redistributable to ship, LTCG for a tighter final binary.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  add_compile_options(/W3 /GR- /O2 /GL /utf-8)
  add_link_options(/LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFESTINPUT:${CMAKE_SOURCE_DIR}/src/app.manifest)
  add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

add_executable(HyoExam WIN32
  src/main.cpp
  src/update.cpp
  src/time_sync.cpp
  src/schedule.cpp
  src/settings.cpp
  src/app.rc
)

target_include_directories(HyoExam PRIVATE src)

target_link_libraries(HyoExam PRIVATE
  d2d1
  dwrite
  ws2_32
  windowscodecs
  shlwapi
  comctl32
  urlmon
  bcrypt
)

add_custom_command(TARGET HyoExam POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/data
          $<TARGET_FILE_DIR:HyoExam>/data
)
