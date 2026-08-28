cmake_minimum_required(VERSION 3.14)
project(VersionUpdatePlugin VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(RPC_GATEWAY_PLUGIN_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}"
    CACHE PATH "Output directory for compiled rpc_gateway plugins")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# --------- Dependencies ---------
include(cmake/CPM.cmake)

if(EXISTS "${CMAKE_SOURCE_DIR}/3rd/fmt-11.2.0/CMakeLists.txt")
  message(STATUS "Using local fmt")
  CPMAddPackage(
    NAME fmt
    VERSION 11.2.0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rd/fmt-11.2.0"
  )
else()
  message(STATUS "Downloading fmt via CPM")
  CPMAddPackage(
    NAME fmt
    VERSION 11.2.0
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 11.2.0
  )
endif()


find_package(xplugin-dev REQUIRED)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)


file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp")
add_library(${PROJECT_NAME} SHARED ${sources})
target_compile_definitions(${PROJECT_NAME} PRIVATE
  PLUGIN_LOG_NAME=VersionUpdatePlugin)
target_include_directories(${PROJECT_NAME} PRIVATE
  ${LibArchive_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
  fmt::fmt-header-only xplugin::sdk CURL::libcurl ${LibArchive_LIBRARIES} OpenSSL::Crypto
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
  CXX_VISIBILITY_PRESET hidden
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${config} "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${config} "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
  )
endforeach()

add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RPC_GATEWAY_PLUGIN_OUTPUT_DIR}"
)
