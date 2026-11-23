cmake_minimum_required(VERSION 3.22)
project(BilibiliFx VERSION 0.2.9 LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
set(bilibili_source "${CMAKE_CURRENT_LIST_DIR}/src")
set(bilibili_assets "${CMAKE_CURRENT_BINARY_DIR}/generated/local/BILIBILI/Assets.h")
set(bilibili_inputs definition.json)
list(TRANSFORM bilibili_inputs PREPEND "${bilibili_source}/")
add_custom_command(OUTPUT "${bilibili_assets}"
 COMMAND "${CMAKE_COMMAND}" "-DOUT=${bilibili_assets}" "-DSOURCE=${bilibili_source}"
  -P "${bilibili_source}/Pack.cmake"
 DEPENDS "${bilibili_source}/Pack.cmake" ${bilibili_inputs}
 VERBATIM)

set(bilibili_notice "${CMAKE_CURRENT_BINARY_DIR}/generated/BilibiliNotices.rc")
file(CONFIGURE OUTPUT "${bilibili_notice}"
 CONTENT "101 RCDATA \"@CMAKE_CURRENT_SOURCE_DIR@/vendor/LICENSE.json\"\n" @ONLY)
set_property(SOURCE "${bilibili_notice}" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/LICENSE.json")

add_library(BILIBILILocalFx MODULE EXCLUDE_FROM_ALL
 "${bilibili_source}/Api.cpp" "${bilibili_source}/Decoder.cpp" "${bilibili_source}/Mp4Stream.cpp" "${bilibili_source}/Panel.cpp" "${bilibili_source}/Rack.cpp" "${bilibili_source}/Server.cpp" "${bilibili_source}/Text.cpp" "${bilibili_assets}" "${bilibili_notice}")
target_include_directories(BILIBILILocalFx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/generated/local/BILIBILI")
target_compile_definitions(BILIBILILocalFx PRIVATE AUDCORE_FX_EXPORTS AUDCORE_FX_RACK_ONLY
 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE BILIBILI_VERSION="${PROJECT_VERSION}")
target_compile_options(BILIBILILocalFx PRIVATE /utf-8 /W4 /Gy /Gw /Zc:inline /experimental:deterministic
 "/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=bilibili")
target_link_options(BILIBILILocalFx PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE)
target_link_libraries(BILIBILILocalFx PRIVATE ole32 user32 gdi32 gdiplus comctl32 ws2_32 winhttp bcrypt mfplat mfreadwrite mfuuid d3d11 dxgi d2d1 dwrite windowscodecs shell32 shlwapi xmllite)
set_target_properties(BILIBILILocalFx PROPERTIES PREFIX "" SUFFIX ".fx.audcore" OUTPUT_NAME "bilibili-v${PROJECT_VERSION}"
 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
 RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/package"
 LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/package")

add_executable(BilibiliTests EXCLUDE_FROM_ALL
 "${bilibili_source}/Api.cpp" "${bilibili_source}/Decoder.cpp" "${bilibili_source}/Mp4Stream.cpp" "${bilibili_source}/Test.cpp" "${bilibili_source}/Server.cpp" "${bilibili_source}/Text.cpp" "${bilibili_assets}")
target_include_directories(BilibiliTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor" "${CMAKE_CURRENT_BINARY_DIR}/generated/local/BILIBILI")
target_compile_definitions(BilibiliTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(BilibiliTests PRIVATE /utf-8 /W4)
target_link_libraries(BilibiliTests PRIVATE ws2_32 winhttp bcrypt ole32 mfplat mfreadwrite mfuuid d3d11 dxgi d2d1 dwrite windowscodecs shlwapi xmllite)
