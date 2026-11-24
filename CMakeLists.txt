cmake_minimum_required(VERSION 3.18)
cmake_policy(SET CMP0091 NEW)

option(CVP_ENABLE_DEBUG "Enables the debug scheme using the Partiels application" ON)

### Models only ###
set(CVP_MODELS_ONLY OFF CACHE BOOL "Only downloads the gguf models (used by the CI cache, without Clap.cpp and the other dependencies)")

### Manual only ###
set(CVP_MANUAL_ONLY OFF CACHE BOOL "Only generates the documentation (without the models, Clap.cpp and the other dependencies)")

set(CVP_TESTER_ENABLE ON CACHE BOOL "Enables the tests (the vamp plugin tester and the analysis test)")
set(CVP_TESTER_TIMEOUT 600 CACHE STRING "The timeout in seconds of the test with the vamp plugin tester (zero for no timeout)")

### Sccache ###
set(CVP_USE_SCCACHE OFF CACHE BOOL "Uses sccache as the compiler launcher")
if(CVP_USE_SCCACHE AND NOT CVP_MODELS_ONLY)
  if(NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CVP_SCCACHE_PROGRAM sccache)
    if(CVP_SCCACHE_PROGRAM)
      set(CMAKE_C_COMPILER_LAUNCHER "${CVP_SCCACHE_PROGRAM}" CACHE STRING "C compiler launcher" FORCE)
      set(CMAKE_CXX_COMPILER_LAUNCHER "${CVP_SCCACHE_PROGRAM}" CACHE STRING "C++ compiler launcher" FORCE)
      set(CVP_SCCACHE_LAUNCHER_MANAGED ON CACHE INTERNAL "Whether the Clap Vamp Plugin enabled the compiler launcher with sccache")
      message(VERBOSE "sccache found, compiler launcher enabled")
    else()
      message(VERBOSE "sccache not found, continuing without compiler cache")
    endif()
  endif()
elseif(CVP_SCCACHE_LAUNCHER_MANAGED)
  unset(CMAKE_C_COMPILER_LAUNCHER CACHE)
  unset(CMAKE_CXX_COMPILER_LAUNCHER CACHE)
  unset(CVP_SCCACHE_LAUNCHER_MANAGED CACHE)
  message(VERBOSE "sccache compiler launcher disabled")
endif()

set(CMAKE_XCODE_GENERATE_SCHEME ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum OS X deployment version")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Default value for MSVC_RUNTIME_LIBRARY of targets" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE CACHE BOOL "Default value for POSITION_INDEPENDENT_CODE of targets" FORCE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE CACHE BOOL "Default value for INTERPROCEDURAL_OPTIMIZATION of targets" FORCE)

if(NOT CVP_MODELS_ONLY AND NOT CVP_MANUAL_ONLY)
### Include Vamp (IVE) ###
set(IVE_BUILD_HOST_LIBRARY OFF)
set(IVE_BUILD_PLUGIN_LIBRARY ON)
add_subdirectory(ircam-vamp-extension EXCLUDE_FROM_ALL)

### Include Clap ###
set(BUILD_SHARED_LIBS OFF CACHE BOOL "ggml: build shared libraries" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "build the clap.cpp tests" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "ggml: optimize the build for the current system" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "ggml: use dynamic backend loading" FORCE)
option(GGML_OPENMP "ggml: use OpenMP" OFF)
option(GGML_CCACHE "ggml: use ccache" OFF)
option(GGML_CPU  "ggml: enable CPU backend" ON)
if(APPLE)
  set(GGML_METAL ON CACHE BOOL "ggml: use Metal" FORCE)
  option(GGML_METAL_EMBED_LIBRARY "ggml: embed Metal library" ON)
  option(GGML_BLAS "ggml: use Accelerate framework" OFF)
else()
  set(GGML_METAL OFF CACHE BOOL "ggml: use Metal" FORCE)
endif()
set(CLAP_BUILD_CLI OFF CACHE BOOL "clap: build the clap command-line executable" FORCE)

add_subdirectory(clap.cpp EXCLUDE_FROM_ALL)

# Set specific compile options and properties for ggml and clap targets
function(set_ggml_options)
  foreach(target IN LISTS ARGN)
    if(TARGET ${target})
      if(CMAKE_GENERATOR STREQUAL "Xcode")
        target_compile_options(${target} PRIVATE -Wno-shorten-64-to-32 -Wno-implicit-float-conversion -Wno-shadow-field-in-constructor -Wno-missing-noreturn -Wno-unused-parameter)
        set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_64_TO_32_BIT_CONVERSION "NO")
        set_target_properties(${target} PROPERTIES STATIC_LIBRARY_OPTIONS "-no_warning_for_no_symbols")
      elseif(UNIX)
        target_compile_options(${target} PRIVATE -Wno-unused-function -Wno-unused-parameter -Wno-c99-extensions)
      elseif(MSVC)
        target_compile_options(${target} PRIVATE /wd4005 /wd4244 /wd4305 /wd4068 /wd4047 /wd4024)
      endif()
    endif()
  endforeach()
endfunction()
endif()

### Project ###
project(ClapVampPlugin VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CVP_MODELS_ONLY AND NOT CVP_MANUAL_ONLY)
# The options of the ggml and clap targets are set after project() because the
# variables that depend on the compiler (such as MSVC) are not defined before.
set_ggml_options(ggml ggml-metal ggml-cpu ggml-base ggml-blas clap clap-icu-uc clap-icu-i18n)

if(MSVC)
  set_target_properties(ggml PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_target_properties(clap PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
endif()

### Version ###
execute_process(COMMAND git log -1 --format=%h WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR} OUTPUT_VARIABLE GIT_COMMIT_ID OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git status --porcelain WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR} OUTPUT_VARIABLE GIT_HAS_DIFF OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT GIT_HAS_DIFF STREQUAL "")
  set(GIT_COMMIT_ID "${GIT_COMMIT_ID}-head")
endif()

if(NOT CVP_BUILD_TAG OR CVP_BUILD_TAG STREQUAL "")
  set(CVP_BUILD_TAG ${CMAKE_PROJECT_VERSION})
endif()
message(STATUS "Clap Vamp Plugin: " ${CVP_BUILD_TAG} " (" ${GIT_COMMIT_ID} ")")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt ${CVP_BUILD_TAG})

if(NOT CVP_MODELS_ONLY AND NOT CVP_MANUAL_ONLY)
### Package ###
set(VPP_NAME "Clap" CACHE PATH "The name of the project" FORCE)
set(VPP_COMPANY "Ircam" CACHE PATH "The name of the company" FORCE)
set(VPP_URL "www.ircam.fr" CACHE PATH "The URL of the project")
set(VPP_APPID "fdada846-ebe4-4984-bf70-551777a25cc3" CACHE PATH "The unique ID of the project")
set(VPP_BUILD_TAG "${CVP_BUILD_TAG}" CACHE STRING "The current tag of the project" FORCE)
set(VPP_ABOUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resource/About.txt" CACHE PATH "The about file to add to the distribution" FORCE)
set(VPP_CHANGELOG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resource/ChangeLog.txt" CACHE PATH "The changelog file to add to the distribution" FORCE)
set(VPP_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resource/Ircam-logo-noir-RS.png" CACHE PATH "The icon file to add to the distribution" FORCE)
set(VPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/package CACHE PATH "The directory of the package" FORCE)
include(vamp-plugin-packager/vamp-plugin-packager.cmake)
endif()

### Source ###

if(NOT CVP_MANUAL_ONLY)
### Models ###
# The gguf models are not compiled into the plugin: they are listed in the
# resource/models.txt file (one model per line with the name, the revision of
# the Hugging Face repository and the SHA256 of the file), downloaded here and
# copied into the Resources folder of the Vamp bundle (see the POST_BUILD command).
# The order of the models in this file must remain identical because the index
# of the model is saved in the documents of the hosts: the cvp_models.h header
# that lists the models in the plugin is generated from this file. The CI caches
# the downloaded models with a key that depends on this file (see the workflows),
# hence modifying the list of the models invalidates the cache.
set(CVP_MODELS_DIR "${CMAKE_CURRENT_BINARY_DIR}/models")
file(MAKE_DIRECTORY "${CVP_MODELS_DIR}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/resource/models.txt" CVP_MODEL_LINES)
set(CVP_MODEL_NAMES "")
foreach(CVP_MODEL_LINE IN LISTS CVP_MODEL_LINES)
  if(NOT CVP_MODEL_LINE MATCHES "^([^ ]+) ([0-9a-f]+) ([0-9a-f]+)$")
    message(FATAL_ERROR "Invalid line in resource/models.txt: ${CVP_MODEL_LINE}")
  endif()
  set(CVP_MODEL_NAME "${CMAKE_MATCH_1}")
  set(CVP_MODEL_REVISION "${CMAKE_MATCH_2}")
  set(CVP_MODEL_SHA256 "${CMAKE_MATCH_3}")
  list(APPEND CVP_MODEL_NAMES "${CVP_MODEL_NAME}")
  set(CVP_MODEL_PATH "${CVP_MODELS_DIR}/${CVP_MODEL_NAME}.gguf")
  # The model is downloaded in a temporary file that is only renamed once its
  # hash is verified, hence an existing model file is always valid.
  if(NOT EXISTS "${CVP_MODEL_PATH}")
    string(REGEX REPLACE "-f(16|32)$|-q([0-9]_k_m|8_0)$" "" CVP_MODEL_REPOSITORY ${CVP_MODEL_NAME})
    set(CVP_MODEL_URL "https://huggingface.co/gorse-io/${CVP_MODEL_REPOSITORY}-GGUF/resolve/${CVP_MODEL_REVISION}/${CVP_MODEL_NAME}.gguf")
    message(STATUS "Downloading model ${CVP_MODEL_NAME}.gguf from ${CVP_MODEL_URL}")
    file(DOWNLOAD "${CVP_MODEL_URL}" "${CVP_MODEL_PATH}.part" SHOW_PROGRESS STATUS CVP_MODEL_STATUS)
    list(GET CVP_MODEL_STATUS 0 CVP_MODEL_STATUS_CODE)
    if(NOT CVP_MODEL_STATUS_CODE EQUAL 0)
      file(REMOVE "${CVP_MODEL_PATH}.part")
      message(FATAL_ERROR "Failed to download the model ${CVP_MODEL_NAME}.gguf: ${CVP_MODEL_STATUS}")
    endif()
    file(SHA256 "${CVP_MODEL_PATH}.part" CVP_MODEL_HASH)
    if(NOT CVP_MODEL_HASH STREQUAL CVP_MODEL_SHA256)
      file(REMOVE "${CVP_MODEL_PATH}.part")
      message(FATAL_ERROR "Invalid SHA256 of the model ${CVP_MODEL_NAME}.gguf: ${CVP_MODEL_HASH} instead of ${CVP_MODEL_SHA256}")
    endif()
    file(RENAME "${CVP_MODEL_PATH}.part" "${CVP_MODEL_PATH}")
  endif()
endforeach()
# The whole models directory is copied into the bundle, hence the models that
# are no longer listed in resource/models.txt are removed.
file(GLOB CVP_MODEL_FILES "${CVP_MODELS_DIR}/*.gguf")
foreach(CVP_MODEL_FILE IN LISTS CVP_MODEL_FILES)
  # NAME_WLE only removes the last extension (the model names can contain dots).
  get_filename_component(CVP_MODEL_FILE_NAME "${CVP_MODEL_FILE}" NAME_WLE)
  if(NOT CVP_MODEL_FILE_NAME IN_LIST CVP_MODEL_NAMES)
    message(STATUS "Removing the model ${CVP_MODEL_FILE_NAME}.gguf that is not listed in resource/models.txt")
    file(REMOVE "${CVP_MODEL_FILE}")
  endif()
endforeach()
endif()

set(CVP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_reference.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_hierarchy.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_hierarchy.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_model.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_model.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_novelty.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cvp_prompt.cpp
)
source_group("sources" FILES ${CVP_SOURCES})

if(NOT CVP_MODELS_ONLY AND NOT CVP_MANUAL_ONLY)
### Target ###
# The header that lists the bundled models is generated from resource/models.txt
# (it is only rewritten when its content changes).
list(LENGTH CVP_MODEL_NAMES CVP_MODEL_COUNT)
list(JOIN CVP_MODEL_NAMES "\",\n        \"" CVP_MODEL_ENTRIES)
set(CVP_MODELS_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${CVP_MODELS_HEADER_DIR}/cvp_models.h" CONTENT "// Generated from resource/models.txt, do not edit.
#pragma once

#include <array>
#include <string_view>

namespace Cvp
{
    static constexpr std::array<std::string_view, ${CVP_MODEL_COUNT}> gBundledModels{
        \"${CVP_MODEL_ENTRIES}\"};
} // namespace Cvp
" @ONLY)

add_library(cvp MODULE ${CVP_SOURCES})
ive_prepare_plugin_target(cvp)
target_include_directories(cvp PRIVATE "${CVP_MODELS_HEADER_DIR}")
target_link_libraries(cvp PRIVATE clap)

if(CVP_TESTER_ENABLE)
  enable_testing()
endif()

vpp_add_plugin(cvp
  OUTPUT_NAME ircamclap
  CATFILE ${CMAKE_CURRENT_SOURCE_DIR}/resource/ircamclap.cat
  COPYRIGHT "Copyright 2026 Ircam. MIT License"
  VERSION ${PROJECT_VERSION_MAJOR}
  INSTALL ON
  DEBUG ${CVP_ENABLE_DEBUG}
  DEBUG_DOCUMENT ${CMAKE_CURRENT_SOURCE_DIR}/test/clap.ptldoc
  # The test C1 (Normal input) checks that the plugin loads (no dependency
  # issues) and that the default gguf model of the bundle is loaded. The tester
  # cannot provide the prompts (precomputed features of the Ircam Vamp Extension),
  # hence the inference is tested by the ClapAnalysis test below.
  TESTER ${CVP_TESTER_ENABLE}
  TESTER_ARGUMENTS -a -t C1
  TESTER_TIMEOUT ${CVP_TESTER_TIMEOUT}
)

# The models folder of the bundle is cleared first so that it never contains
# a model that is no longer listed in resource/models.txt.
add_custom_command(TARGET cvp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:cvp>/../Resources/models"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CVP_MODELS_DIR}" "$<TARGET_FILE_DIR:cvp>/../Resources/models"
)

# The licenses of the plugin, of the third-party libraries and of the models are
# copied into the Resources/licenses folder of the Vamp bundle (hence they are
# distributed with the bundle and the installers). The ThirdPartyNotices.txt file
# lists the components and refers to the license files. The Clap.cpp license
# (Apache License 2.0) also covers the embedded models.
set(CVP_LICENSES_DIR "$<TARGET_FILE_DIR:cvp>/../Resources/licenses")
add_custom_command(TARGET cvp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CVP_LICENSES_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/resource/ThirdPartyNotices.txt" "${CVP_LICENSES_DIR}/ThirdPartyNotices.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CVP_LICENSES_DIR}/Clap-Vamp-Plugin.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/clap.cpp/LICENSE" "${CVP_LICENSES_DIR}/Apache-2.0.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/clap.cpp/ggml/LICENSE" "${CVP_LICENSES_DIR}/ggml.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/clap.cpp/thirdparty/icu/LICENSE" "${CVP_LICENSES_DIR}/icu.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/clap.cpp/thirdparty/json/LICENSE.MIT" "${CVP_LICENSES_DIR}/nlohmann-json.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/clap.cpp/thirdparty/miniaudio/LICENSE" "${CVP_LICENSES_DIR}/miniaudio.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${VAMP_SDK_PATH}/COPYING" "${CVP_LICENSES_DIR}/vamp-plugin-sdk.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/ircam-vamp-extension/LICENSE" "${CVP_LICENSES_DIR}/ircam-vamp-extension.txt"
)

if(CVP_TESTER_ENABLE)
  # The analysis test compiles the sources of the plugin in an executable that
  # finds the downloaded models with the CLAPMODELSPATH environment variable.
  add_executable(cvp_test ${CMAKE_CURRENT_SOURCE_DIR}/test/cvp_test.cpp ${CVP_SOURCES})
  set_target_properties(cvp_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(cvp_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source" "${CVP_MODELS_HEADER_DIR}")
  target_compile_definitions(cvp_test PRIVATE PLUGIN_VERSION=${PROJECT_VERSION_MAJOR})
  target_link_libraries(cvp_test PRIVATE ive_plugin_static clap)
  if(MSVC)
    target_compile_definitions(cvp_test PRIVATE _USE_MATH_DEFINES)
    target_compile_options(cvp_test PRIVATE /wd4996 /wd4267)
  endif()
  add_test(NAME ClapAnalysis COMMAND cvp_test)
  set_tests_properties(ClapAnalysis PROPERTIES ENVIRONMENT "CLAPMODELSPATH=${CVP_MODELS_DIR}")
  if(CVP_TESTER_TIMEOUT GREATER 0)
    set_tests_properties(ClapAnalysis PROPERTIES TIMEOUT ${CVP_TESTER_TIMEOUT})
  endif()
endif()

# The sources are passed as a single list argument.
vpp_create_clang_format_targets(cvp "${CVP_SOURCES};${CMAKE_CURRENT_SOURCE_DIR}/test/cvp_test.cpp")
endif()

if(NOT CVP_MODELS_ONLY)
### Manual ###
set(CVP_MANUAL_DIR ${CMAKE_CURRENT_BINARY_DIR}/Manual)
file(MAKE_DIRECTORY ${CVP_MANUAL_DIR})

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/docs/clap-manual.md MANUAL_CONTENT)
string(REPLACE "APPVERSION" "${CVP_BUILD_TAG} (${GIT_COMMIT_ID})" MANUAL_CONTENT ${MANUAL_CONTENT})
string(REPLACE "src=\"../resource/" "src=\"${CMAKE_CURRENT_SOURCE_DIR}/resource/" MANUAL_CONTENT ${MANUAL_CONTENT})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/clap-manual.md ${MANUAL_CONTENT})

find_program(MDPDF_EXE "mdpdf")
if(MDPDF_EXE)
  add_custom_target(cvp_manual COMMAND ${MDPDF_EXE} ${CMAKE_CURRENT_BINARY_DIR}/clap-manual.md ${CVP_MANUAL_DIR}/clap-manual.pdf VERBATIM)
else()
  message(STATUS "Manual target cannot be generated because mdpdf is not found")
endif()

### Publishing ###
set(PROJECT_URL "https://github.com/Ircam-Partiels/clap-vamp-plugin")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/clap-release.md "## Downloads\n")
file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/clap-release.md "- [Clap-Linux.zip](${PROJECT_URL}/releases/download/${CVP_BUILD_TAG}/Clap-Linux.zip)\n")
file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/clap-release.md "- [Clap-MacOS.pkg](${PROJECT_URL}/releases/download/${CVP_BUILD_TAG}/Clap-MacOS.pkg)\n")
file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/clap-release.md "- [Clap-Windows.exe](${PROJECT_URL}/releases/download/${CVP_BUILD_TAG}/Clap-Windows.exe)\n")
file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/clap-release.md "- [Clap-Manual.zip](${PROJECT_URL}/releases/download/${CVP_BUILD_TAG}/Clap-Manual.zip)\n")
endif()
