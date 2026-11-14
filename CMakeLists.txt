cmake_minimum_required(VERSION 3.28...3.30)

# The GitHub Actions workflow copies the official OBS plugin-template CMake
# support files into this checkout before configuring the project.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

option(ENABLE_FRONTEND_API "Use obs-frontend-api" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/chat.cpp
    src/alerts.cpp
    src/goals.cpp
    src/chatterbox.cpp
)

# OBS APIs used by the plugin.
find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# External dependencies used by chat.cpp and chatterbox.cpp.
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# Applies OBS's platform-specific plugin output/install layout and includes
# the files under data/ in the plugin package.
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "BeepBoop"
)
