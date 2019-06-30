cmake_minimum_required(VERSION 3.10)
project(texture_compiler)

set(CMAKE_CXX_STANDARD 17)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT texture_compiler)

# Compile texture compiler sources.

file(GLOB_RECURSE TEXTURE_COMPILER_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_executable(texture_compiler ${TEXTURE_COMPILER_SOURCES})

# Include thirdparty libraries.

target_include_directories(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/SDL2/${CMAKE_HOST_SYSTEM_NAME}/include/")
target_include_directories(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/bgfx/include/")
target_include_directories(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/nvtt/include/")
target_include_directories(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/Clara/include/")
target_include_directories(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/glm/include/")

# Link thirdpary libraries.

if(WIN32)
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/SDL2/Windows/lib/SDL2main.lib")
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/SDL2/Windows/lib/SDL2.lib")
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/bgfx/lib/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/bgfx.lib")
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/bgfx/lib/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/bimg.lib")
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/bgfx/lib/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/bx.lib")
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/bgfx/lib/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/astc-codec.lib")
    target_link_libraries(texture_compiler PRIVATE "${CMAKE_SOURCE_DIR}/lib/nvtt/lib/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/nvtt.lib")
elseif(APPLE)
    # TODO: MacOS
elseif(UNIX)
    # TODO: Linux
endif()

# Link against static runtime library.

if(WIN32)
    set(cxx_compiler_flags
            CMAKE_CXX_FLAGS
            CMAKE_CXX_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_C_FLAGS
            CMAKE_C_FLAGS_DEBUG
            CMAKE_C_FLAGS_RELEASE
    )
    foreach(cxx_compiler_flag ${cxx_compiler_flags})
        string(REPLACE "/MD" "/MT" ${cxx_compiler_flag} "${${cxx_compiler_flag}}")
    endforeach()
endif()

# Set up shared libraries location directory for MacOS and Linux.

if(APPLE)
    set_property(TARGET texture_compiler PROPERTY SKIP_BUILD_RPATH "ON")

    if(APPLE)
        set_property(TARGET texture_compiler APPEND PROPERTY LINK_FLAGS "-Wl,-rpath,\"@executable_path\"")
    else()
        set_property(TARGET texture_compiler APPEND PROPERTY LINK_FLAGS "-Wl,-rpath,\"$ORIGIN\"")
    endif()
endif()

# Deploy shared libraries.

set(shared_libraries "")

if(WIN32)
    # CMake generator expressions are not supported in add_custom_command OUTPUT yet.

    add_custom_command(
            DEPENDS "${CMAKE_SOURCE_DIR}/lib/SDL2/Windows/bin/SDL2.dll"
            OUTPUT "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/SDL2.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/lib/SDL2/Windows/bin/SDL2.dll" "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/SDL2.dll"
            COMMENT "Deploying SDL2.dll"
    )
    list(APPEND shared_libraries "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/SDL2.dll")

    add_custom_command(
            DEPENDS "${CMAKE_SOURCE_DIR}/lib/nvtt/bin/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/nvtt.dll"
            OUTPUT "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/nvtt.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/lib/nvtt/bin/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/nvtt.dll" "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/nvtt.dll"
            COMMENT "Deploying nvtt.dll"
    )
    list(APPEND shared_libraries "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/nvtt.dll")
elseif(APPLE)
    # TODO: MacOS
elseif(UNIX)
    # TODO: Linux
endif()

add_custom_target(deploy_shared_libraries ALL DEPENDS "${shared_libraries}")
add_dependencies(texture_compiler deploy_shared_libraries)
