get_filename_component(host "${CMAKE_CURRENT_SOURCE_DIR}/../audcore" ABSOLUTE)
get_filename_component(framework "${host}/../audcore-plugins/framework" ABSOLUTE)
set(vst "${framework}/vst3")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/bilibili-vst3")
file(MAKE_DIRECTORY "${generated}")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(node NAMES node REQUIRED)
set(definition "${bilibili_source}/definition.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${definition}"
 "${host}/scripts/generate-aud76-vst3.py" "${vst}/generate-identities.cjs")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${host}/scripts/generate-aud76-vst3.py"
 "${definition}" "${generated}/RackVst3Data.h" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${node}" "${vst}/generate-identities.cjs" "${generated}"
 "${definition}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${node}" "${vst}/generate-ui.cjs" "${generated}"
 --effect EQ COMMAND_ERROR_IS_FATAL ANY)
file(CONFIGURE OUTPUT "${generated}/AudioCoreVersion.h"
 CONTENT "#define AUDIOCORE_PLUGIN_VERSION \"@PROJECT_VERSION@\"\n" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/Version.rc.in" "${generated}/Version.rc" @ONLY)

add_library(BilibiliVst3Sdk STATIC EXCLUDE_FROM_ALL "${vst}/Interfaces.cpp"
 "${vst}/sdk/pluginterfaces/base/funknown.cpp" "${vst}/sdk/pluginterfaces/base/coreiids.cpp")
target_include_directories(BilibiliVst3Sdk PUBLIC "${vst}/sdk")
target_compile_definitions(BilibiliVst3Sdk PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

get_target_property(fxSources BILIBILILocalFx SOURCES)
get_target_property(fxIncludes BILIBILILocalFx INCLUDE_DIRECTORIES)
get_target_property(fxDefines BILIBILILocalFx COMPILE_DEFINITIONS)
get_target_property(fxLibraries BILIBILILocalFx LINK_LIBRARIES)
list(REMOVE_ITEM fxDefines AUDCORE_FX_EXPORTS)
add_library(BilibiliVst3 MODULE EXCLUDE_FROM_ALL ${fxSources}
 "${vst}/NativePlugin.cpp" "${vst}/ui/Components.cpp" "${vst}/ui/EventsWindows.cpp"
 "${framework}/core/State.cpp" "${framework}/core/StateWindows.cpp" "${generated}/Version.rc")
target_include_directories(BilibiliVst3 PRIVATE ${fxIncludes} "${generated}" "${framework}" "${vst}")
target_compile_definitions(BilibiliVst3 PRIVATE ${fxDefines} AUDCORE_RACK_VST3 AUDCORE_RACK_PERSIST_STATE
 AUDCORE_RACK_BACKGROUND_VISUAL AUDCORE_RACK_VISUAL_ON_OPEN AUDCORE_EQ_ONLY AUDCORE_HAS_REVISION AUDCORE_PLUGIN_CODE="BILIBILI"
 AUDCORE_RACK_PRODUCT_NAME="Bilibili")
target_link_libraries(BilibiliVst3 PRIVATE BilibiliVst3Sdk ${fxLibraries} shell32)
target_link_options(BilibiliVst3 PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE)
set_target_properties(BilibiliVst3 PROPERTIES PREFIX "" SUFFIX ".vst3" OUTPUT_NAME "Bilibili-v${PROJECT_VERSION}"
 RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/package"
 LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/package")

add_executable(BilibiliVst3Tests EXCLUDE_FROM_ALL "${CMAKE_CURRENT_LIST_DIR}/Test.cpp")
target_link_libraries(BilibiliVst3Tests PRIVATE BilibiliVst3Sdk)
target_include_directories(BilibiliVst3Tests PRIVATE "${vst}")
foreach(target BilibiliVst3 BilibiliVst3Sdk BilibiliVst3Tests)
 set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
 target_compile_options(${target} PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/Gy;/Gw;/Zc:inline;/experimental:deterministic>"
  "$<$<COMPILE_LANGUAGE:CXX>:/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=bilibili>"
  "$<$<COMPILE_LANGUAGE:CXX>:/pathmap:${framework}=framework>")
endforeach()
