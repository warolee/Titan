cmake_minimum_required(VERSION 3.16)
project(elemental_shaman VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(SDK_FOUND FALSE)

get_filename_component(CLIENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

get_filename_component(SDK_ROOT "${CLIENT_ROOT}/sdk" ABSOLUTE)
if(EXISTS "${SDK_ROOT}/include/sdk/sdk.h" AND EXISTS "${SDK_ROOT}/lib/rotation_api.lib")
    message(STATUS "SDK Mode: Packaged SDK (user installation)")
    set(SDK_INCLUDE_DIR "${SDK_ROOT}/include")
    set(ROTATION_API_INCLUDE_DIR "${SDK_ROOT}/include/rotation_api/include")
    set(ROTATION_API_SRC_DIR "${SDK_ROOT}/include/rotation_api/src")
    set(SDK_LIB_DIR "${SDK_ROOT}/lib")
    set(ROTATION_API_LIBRARY "${SDK_LIB_DIR}/rotation_api.lib")
    set(SDK_FOUND TRUE)
    set(USING_PACKAGED_SDK TRUE)
endif()

if(NOT SDK_FOUND)
    get_filename_component(DEV_SDK_ROOT "${CLIENT_ROOT}/src/rotations/sdk" ABSOLUTE)
    get_filename_component(DEV_ROTATION_API_ROOT "${CLIENT_ROOT}/src/rotation_api" ABSOLUTE)
    get_filename_component(DEV_BUILD_DIR "${CLIENT_ROOT}/build" ABSOLUTE)

    if(EXISTS "${DEV_SDK_ROOT}/include/sdk/sdk.h")
        message(STATUS "SDK Mode: Developer (source tree)")
        set(SDK_INCLUDE_DIR "${DEV_SDK_ROOT}/include")
        set(ROTATION_API_INCLUDE_DIR "${DEV_ROTATION_API_ROOT}/include")
        set(ROTATION_API_SRC_DIR "${DEV_ROTATION_API_ROOT}/src")

        find_library(ROTATION_API_LIBRARY
            NAMES rotation_api
            PATHS
                "${DEV_BUILD_DIR}/Debug"
                "${DEV_BUILD_DIR}/Release"
                "${DEV_BUILD_DIR}/src/rotation_api/Debug"
                "${DEV_BUILD_DIR}/src/rotation_api/Release"
            NO_DEFAULT_PATH
        )

        if(ROTATION_API_LIBRARY)
            set(SDK_FOUND TRUE)
        endif()
    endif()
endif()

if(NOT SDK_FOUND)
    message(FATAL_ERROR "Could not find Rotation SDK!")
endif()

if(NOT ROTATION_API_LIBRARY)
    message(FATAL_ERROR "Could not find rotation_api.lib")
endif()

message(STATUS "Found rotation_api library: ${ROTATION_API_LIBRARY}")

add_library(elemental_shaman SHARED
    elemental_shaman.cpp
)

target_include_directories(elemental_shaman PRIVATE
    "${SDK_INCLUDE_DIR}"
)

if(ROTATION_API_INCLUDE_DIR)
    target_include_directories(elemental_shaman PRIVATE
        "${ROTATION_API_INCLUDE_DIR}"
    )
endif()

if(ROTATION_API_SRC_DIR)
    target_include_directories(elemental_shaman PRIVATE
        "${ROTATION_API_SRC_DIR}"
    )
endif()

target_link_libraries(elemental_shaman PRIVATE
    ${ROTATION_API_LIBRARY}
)

target_compile_definitions(elemental_shaman PRIVATE
    ROTATION_DLL_EXPORTS
)

if(USING_PACKAGED_SDK)
    target_compile_definitions(elemental_shaman PRIVATE
        ROTATION_SDK_BUILD
    )
endif()

set_target_properties(elemental_shaman PROPERTIES
    OUTPUT_NAME "ElementalShaman"
    PREFIX ""
    VERSION ""
    SOVERSION ""
)

add_custom_command(TARGET elemental_shaman POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:elemental_shaman>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../External/SethElementalShaman.dll"
    COMMENT "Installing SethElementalShaman.dll for Titan External Rotations"
)

if(WIN32)
    target_compile_definitions(elemental_shaman PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

install(TARGETS elemental_shaman
    RUNTIME DESTINATION rotations
    LIBRARY DESTINATION rotations
)

message(STATUS "SethElementalShaman Rotation Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  SDK Include: ${SDK_INCLUDE_DIR}")
