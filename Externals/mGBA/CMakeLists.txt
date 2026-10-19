set(LIBMGBA_ONLY ON)
set(USE_LZMA ON)
add_subdirectory(mgba EXCLUDE_FROM_ALL)
dolphin_disable_warnings(mgba)

target_compile_definitions(mgba PUBLIC HAVE_CRC32 ENABLE_VFS ENABLE_DIRECTORIES)
target_link_libraries(mgba ZLIB::ZLIB)

if(NOT MSVC)
  target_compile_options(mgba PRIVATE -Wno-unused-parameter -Wno-unused-result -Wno-unused-variable)
endif()

if(WINDOWS_STORE)
  # UWP projects enable /sdl, which turns mGBA's uninitialized-pointer warnings into errors.
  target_compile_options(mgba PRIVATE /sdl-)
  target_sources(mgba PRIVATE UWP/KnownFolders.cpp UWP/KnownFolders.h)
  set_source_files_properties(mgba/src/core/config.c TARGET_DIRECTORY mgba PROPERTIES
    COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/UWP/KnownFolders.h")
endif()

if(ANDROID)
  target_compile_definitions(mgba PUBLIC ENABLE_VFS_FD)
  target_compile_definitions(mgba PRIVATE -Dfutimes=futimens)
endif()

add_library(mGBA::mgba ALIAS mgba)
