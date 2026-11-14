cmake_minimum_required(VERSION 3.20)

if(WIN32)
    # Official OBS plugin build system.
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

    project(${_name} VERSION ${_version} LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    option(ENABLE_FRONTEND_API "Use OBS frontend API" ON)
    option(ENABLE_QT "Use Qt functionality" OFF)

    include(compilerconfig)
    include(defaults)
    include(helpers)

    add_library(${CMAKE_PROJECT_NAME} MODULE
        src/chat.cpp
        src/alerts.cpp
        src/goals.cpp
        src/chatterbox.cpp
    )

    find_package(libobs REQUIRED)
    find_package(obs-frontend-api REQUIRED)
    find_package(CURL REQUIRED)
    find_package(nlohmann_json 3.11 REQUIRED)

    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        OBS::libobs
        OBS::obs-frontend-api
        CURL::libcurl
        nlohmann_json::nlohmann_json
    )

    # Prevent Windows min/max macros from breaking std::min/std::max.
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX)

    set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES
        OUTPUT_NAME BeepBoop
    )

else()
    # Existing Linux build system.
    project(BeepBoop VERSION 1.0.0 LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)

    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBOBS REQUIRED libobs)
    pkg_check_modules(LIBCURL REQUIRED libcurl)
    pkg_check_modules(OBS_FRONTEND REQUIRED obs-frontend-api)

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

    target_include_directories(BeepBoop PRIVATE
        ${LIBOBS_INCLUDE_DIRS}
        ${LIBCURL_INCLUDE_DIRS}
        ${OBS_FRONTEND_INCLUDE_DIRS}
    )

    target_link_directories(BeepBoop PRIVATE
        ${LIBOBS_LIBRARY_DIRS}
        ${LIBCURL_LIBRARY_DIRS}
        ${OBS_FRONTEND_LIBRARY_DIRS}
    )

    target_compile_options(BeepBoop PRIVATE
        ${OBS_CXX_FLAGS}
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(BeepBoop PRIVATE
            -Wall
            -Wextra
            -Wno-pedantic
        )
    endif()

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
        DESTINATION
        "$ENV{HOME}/.config/obs-studio/plugins/BeepBoop/data"
    )
endif()
