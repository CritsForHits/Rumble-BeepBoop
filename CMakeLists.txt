cmake_minimum_required(VERSION 3.20)

project(BeepBoop VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBOBS REQUIRED libobs)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(OBS_FRONTEND REQUIRED obs-frontend-api)

# OBS provides some C-specific compiler flags.
# Remove those that are invalid or undesirable for C++.
set(OBS_CXX_FLAGS ${LIBOBS_CFLAGS_OTHER})

list(REMOVE_ITEM OBS_CXX_FLAGS
    "-std=gnu17"
    "-Werror"
)

add_library(BeepBoop MODULE
    src/chat.cpp
    src/alerts.cpp
    src/goals.cpp
    src/chatterbox.cpp
)

# Prevent Windows headers from defining min/max macros that conflict with std::min/std::max.
if(WIN32)
    target_compile_definitions(BeepBoop PRIVATE NOMINMAX)
endif()

target_include_directories(BeepBoop PRIVATE
    /usr/include/obs
    /usr/include/nlohmann
    ${LIBOBS_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${OBS_FRONTEND_INCLUDE_DIRS}
)

target_link_directories(BeepBoop PRIVATE
    ${LIBOBS_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
    ${OBS_FRONTEND_LIBRARY_DIRS}
    ${OBS_FRONTEND_LINK_LIBRARIES}
)

target_compile_options(BeepBoop PRIVATE
    ${OBS_CXX_FLAGS}
    -Wall
    -Wextra
    -Wno-pedantic
)

target_link_libraries(BeepBoop PRIVATE
    ${LIBOBS_LINK_LIBRARIES}
    ${LIBCURL_LINK_LIBRARIES}
    ${OBS_FRONTEND_LINK_LIBRARIES}
)

set_target_properties(BeepBoop PROPERTIES
    PREFIX ""
    OUTPUT_NAME "BeepBoop"
)

install(TARGETS BeepBoop
    LIBRARY DESTINATION
    "$ENV{HOME}/.config/obs-studio/plugins/BeepBoop/bin/64bit"
)
install(DIRECTORY data/
    DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/BeepBoop/data"
)