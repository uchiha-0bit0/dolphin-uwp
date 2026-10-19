set(DOLPHIN_UWP_SCREENSCRAPER_DEV_ID "" CACHE STRING "ScreenScraper developer id for the UWP frontend")
set(DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD "" CACHE STRING
  "ScreenScraper developer password for the UWP frontend")
mark_as_advanced(DOLPHIN_UWP_SCREENSCRAPER_DEV_ID DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD)
# A local screenscraper_credentials.cmake, which Git ignores, can set both.
include("${CMAKE_CURRENT_SOURCE_DIR}/screenscraper_credentials.cmake" OPTIONAL)
if(NOT DOLPHIN_UWP_SCREENSCRAPER_DEV_ID OR NOT DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD)
  message(WARNING "ScreenScraper is disabled because its developer credentials are not set")
endif()

add_library(imgui_dx11 STATIC
  "${CMAKE_SOURCE_DIR}/Externals/imgui/imgui/backends/imgui_impl_dx11.cpp"
)
target_include_directories(imgui_dx11 PUBLIC "${CMAKE_SOURCE_DIR}/Externals/imgui/imgui/backends")
target_link_libraries(imgui_dx11 PUBLIC imgui PRIVATE common d3d11.lib d3dcompiler.lib dxgi.lib)
dolphin_disable_warnings(imgui_dx11)

set(DOLPHIN_UWP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/Package.appxmanifest")
set_source_files_properties("${DOLPHIN_UWP_MANIFEST}" PROPERTIES VS_DEPLOYMENT_CONTENT 1)

file(GLOB DOLPHIN_UWP_PACKAGE_ASSETS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*.png")
set_source_files_properties(${DOLPHIN_UWP_PACKAGE_ASSETS} PROPERTIES
  VS_DEPLOYMENT_CONTENT 1
  VS_DEPLOYMENT_LOCATION "Assets"
)

function(dolphin_uwp_deploy_tree root prefix out_files)
  file(GLOB_RECURSE files CONFIGURE_DEPENDS "${root}/*")
  foreach(file IN LISTS files)
    file(RELATIVE_PATH relative "${root}" "${file}")
    get_filename_component(directory "${relative}" DIRECTORY)
    if(directory STREQUAL "")
      set(location "${prefix}")
    else()
      set(location "${prefix}/${directory}")
    endif()
    set_source_files_properties("${file}" PROPERTIES
      VS_DEPLOYMENT_CONTENT 1
      VS_DEPLOYMENT_LOCATION "${location}"
      VS_TOOL_OVERRIDE "None"
    )
  endforeach()
  set(${out_files} ${files} PARENT_SCOPE)
endfunction()

set(DOLPHIN_UWP_WINDOWS_KITS_DIR "C:/Program Files (x86)/Windows Kits/10" CACHE PATH
  "Windows Kits 10 root used for UWP redistributables")
find_file(DOLPHIN_UWP_DBGCORE dbgcore.dll
  PATHS "${DOLPHIN_UWP_WINDOWS_KITS_DIR}/Debuggers/x64"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH
)
if(DOLPHIN_UWP_DBGCORE)
  set_source_files_properties("${DOLPHIN_UWP_DBGCORE}" PROPERTIES
    VS_DEPLOYMENT_CONTENT 1
    VS_TOOL_OVERRIDE "None"
  )
else()
  message(WARNING "dbgcore.dll was not found; the UWP app will not write crash dumps")
  set(DOLPHIN_UWP_DBGCORE "")
endif()

set(DOLPHIN_UWP_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/Externals/FFmpeg-uwp")
file(GLOB DOLPHIN_UWP_FFMPEG_DLLS CONFIGURE_DEPENDS "${DOLPHIN_UWP_FFMPEG_DIR}/bin/*.dll")
set_source_files_properties(${DOLPHIN_UWP_FFMPEG_DLLS} PROPERTIES
  VS_DEPLOYMENT_CONTENT 1
  VS_TOOL_OVERRIDE "None"
)
set(DOLPHIN_UWP_FFMPEG_LICENSE "${DOLPHIN_UWP_FFMPEG_DIR}/LICENSE.txt")
set_source_files_properties("${DOLPHIN_UWP_FFMPEG_LICENSE}" PROPERTIES
  VS_DEPLOYMENT_CONTENT 1
  VS_DEPLOYMENT_LOCATION "licenses/ffmpeg"
  VS_TOOL_OVERRIDE "None"
)

# Dolphin's translations, compiled as DolphinQt's build does.
set(DOLPHIN_UWP_MSGFMT "${CMAKE_SOURCE_DIR}/Externals/gettext/msgfmt.exe")
file(GLOB DOLPHIN_UWP_PO_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Languages/po/*.po")
set(DOLPHIN_UWP_MO_FILES)
foreach(po IN LISTS DOLPHIN_UWP_PO_FILES)
  get_filename_component(lang "${po}" NAME_WE)
  set(mo_dir "${CMAKE_CURRENT_BINARY_DIR}/Languages/${lang}")
  set(mo "${mo_dir}/dolphin-emu.mo")
  if(NOT EXISTS "${mo}" OR "${po}" IS_NEWER_THAN "${mo}")
    file(MAKE_DIRECTORY "${mo_dir}")
    execute_process(COMMAND "${DOLPHIN_UWP_MSGFMT}" -o "${mo}" "${po}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(WARNING "The translation ${lang} could not be compiled")
      continue()
    endif()
  endif()
  set_source_files_properties("${mo}" PROPERTIES
    VS_DEPLOYMENT_CONTENT 1
    VS_DEPLOYMENT_LOCATION "Languages/${lang}"
    VS_TOOL_OVERRIDE "None"
  )
  list(APPEND DOLPHIN_UWP_MO_FILES "${mo}")
endforeach()

# Every image and font of the interface, including the ones the frontend asks for by default.
dolphin_uwp_deploy_tree("${CMAKE_CURRENT_SOURCE_DIR}/Resources" "resources" DOLPHIN_UWP_RESOURCES)
dolphin_uwp_deploy_tree("${CMAKE_SOURCE_DIR}/Data/Sys" "Sys" DOLPHIN_UWP_SYS_FILES)
list(FILTER DOLPHIN_UWP_SYS_FILES EXCLUDE REGEX "/Data/Sys/Themes/")

add_executable(dolphin-uwp WIN32
  App.cpp
  BackgroundTask.cpp
  BackgroundTask.h
  CrashHandler.cpp
  CrashHandler.h
  DebuggerNetwork.cpp
  DebuggerNetwork.h
  DebuggerView.cpp
  DebuggerView.h
  DiscToolsPage.cpp
  DiscToolsPage.h
  DisplayMetrics.cpp
  DisplayMetrics.h
  EmulationSession.cpp
  EmulationSession.h
  FifoAnalysis.cpp
  FifoAnalysis.h
  FrontendHost.cpp
  FrontendHost.h
  FrontendRenderer.cpp
  FrontendRenderer.h
  GameCheats.cpp
  GameCheats.h
  GameLibrary.cpp
  GameLibrary.h
  GameSettingsPage.cpp
  GameSettingsPage.h
  GameTextInput.cpp
  GameTextInput.h
  GBAControls.cpp
  GBAControls.h
  GBAScreens.cpp
  GBAScreens.h
  GraphicsMods.cpp
  GraphicsMods.h
  Host.cpp
  HostState.h
  HotkeyScheduler.cpp
  HotkeyScheduler.h
  InputMappingHost.cpp
  InputMappingHost.h
  InputSource.cpp
  InputSource.h
  Leaderboards.cpp
  Leaderboards.h
  LogBuffer.cpp
  LogBuffer.h
  MappingPage.cpp
  MappingPage.h
  MediaStreamer.cpp
  MediaStreamer.h
  MemoryCardPanel.cpp
  MemoryCardPanel.h
  NetPlayController.cpp
  NetPlayController.h
  NetPlayPage.cpp
  NetPlayPage.h
  PortalPanel.cpp
  PortalPanel.h
  Portals.cpp
  Portals.h
  PromptService.cpp
  PromptService.h
  QuickMenu.cpp
  QuickMenu.h
  Recordings.cpp
  Recordings.h
  SettingChoices.h
  SettingsPages.cpp
  SettingsPages.h
  ShellServices.cpp
  ShellServices.h
  ShellWidgets.cpp
  ShellWidgets.h
  SystemPrompts.cpp
  SystemPrompts.h
  TasView.cpp
  TasView.h
  TitlePages.cpp
  TitlePages.h
  ToolsPage.cpp
  ToolsPage.h
  Translation.cpp
  Translation.h
  UWPSettings.cpp
  UWPSettings.h
  WiiSystemTools.cpp
  WiiSystemTools.h
  "${DOLPHIN_UWP_MANIFEST}"
  ${DOLPHIN_UWP_PACKAGE_ASSETS}
  ${DOLPHIN_UWP_RESOURCES}
  ${DOLPHIN_UWP_SYS_FILES}
  ${DOLPHIN_UWP_MO_FILES}
  ${DOLPHIN_UWP_FFMPEG_DLLS}
  "${DOLPHIN_UWP_FFMPEG_LICENSE}"
  ${DOLPHIN_UWP_DBGCORE}
)

target_include_directories(dolphin-uwp SYSTEM PRIVATE "${DOLPHIN_UWP_FFMPEG_DIR}/include")

target_link_libraries(dolphin-uwp PRIVATE
  core
  uicommon
  UwpImGuiFrontend
  imgui_dx11
  windowsapp.lib
  d3d11.lib
  dxgi.lib
  windowscodecs.lib
)

if(DOLPHIN_UWP_SCREENSCRAPER_DEV_ID AND DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD)
  target_compile_definitions(dolphin-uwp PRIVATE
    DOLPHIN_UWP_SCREENSCRAPER_DEV_ID="${DOLPHIN_UWP_SCREENSCRAPER_DEV_ID}"
    DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD="${DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD}"
  )
endif()

if(MSVC)
  target_link_libraries(dolphin-uwp PRIVATE use_pch)
endif()

target_link_options(dolphin-uwp PRIVATE /APPCONTAINER /WINMD:NO)

set_target_properties(dolphin-uwp PROPERTIES
  OUTPUT_NAME DolphinUWP
  VS_WINRT_COMPONENT OFF
  VS_WINRT_EXTENSIONS OFF
  VS_GLOBAL_CharacterSet "Unicode"
  VS_GLOBAL_DefaultLanguage "en-US"
  VS_GLOBAL_AppContainerApplication "true"
  VS_GLOBAL_ApplicationType "Windows Store"
  VS_GLOBAL_ApplicationTypeRevision "10.0"
  VS_GLOBAL_WindowsTargetPlatformMinVersion "10.0.19041.0"
  VS_GLOBAL_AppxBundle "Never"
)

set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT dolphin-uwp)
