cmake_minimum_required(VERSION 3.10)
project(texture_compiler)

include(cmake/shaderc.cmake)
include(cmake/deploy.cmake)

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

# Compile shaders.

build_shaders("fragment" "${CMAKE_SOURCE_DIR}/*.fragment.sc" "${CMAKE_SOURCE_DIR}/src/headers/")
build_shaders("vertex" "${CMAKE_SOURCE_DIR}/*.vertex.sc" "${CMAKE_SOURCE_DIR}/src/headers/")
add_dependencies(texture_compiler build_fragment_shaders build_vertex_shaders)

# Deploy shared libraries.

deploy_shared_library("sdl2"
        "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/"
        "${CMAKE_SOURCE_DIR}/lib/SDL2/Windows/bin/SDL2.dll"
        "${CMAKE_SOURCE_DIR}/lib/SDL2/Darwin/bin/SDL2.dylib"
        "${CMAKE_SOURCE_DIR}/lib/SDL2/Linux/bin/SDL2.so"
)
deploy_shared_library("nvtt"
        "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/"
        "${CMAKE_SOURCE_DIR}/lib/nvtt/bin/Windows/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/nvtt.dll"
        "${CMAKE_SOURCE_DIR}/lib/nvtt/bin/Darwin/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/nvtt.dylib"
        "${CMAKE_SOURCE_DIR}/lib/nvtt/bin/Linux/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>/nvtt.so"
)
add_dependencies(texture_compiler deploy_sdl2 deploy_nvtt)
