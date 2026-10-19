set(UPNPC_BUILD_STATIC ON)
set(UPNPC_BUILD_SHARED OFF)
set(UPNPC_BUILD_TESTS OFF)
set(UPNPC_BUILD_SAMPLE OFF)
set(UPNPC_NO_INSTALL ON)
if(WINDOWS_STORE)
  # The UWP headers require the Windows 10 networking definitions.
  set(MINIUPNPC_TARGET_WINDOWS_VERSION "0x0A00" CACHE STRING "" FORCE)
endif()

add_subdirectory(miniupnp/miniupnpc)
dolphin_disable_warnings(libminiupnpc-static)

add_library(Miniupnpc::miniupnpc ALIAS libminiupnpc-static)
