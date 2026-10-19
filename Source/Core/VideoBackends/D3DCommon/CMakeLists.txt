add_library(videod3dcommon
  D3DCommon.cpp
  D3DCommon.h
  Shader.cpp
  Shader.h
  SwapChain.cpp
  SwapChain.h
)

target_link_libraries(videod3dcommon
PUBLIC
  common
  videocommon
  spirv_cross
)

if(WINDOWS_STORE)
  target_link_libraries(videod3dcommon PUBLIC dxgi.lib d3dcompiler.lib)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(videod3dcommon PRIVATE use_pch)
endif()
