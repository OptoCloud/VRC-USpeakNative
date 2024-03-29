cmake_minimum_required (VERSION 3.5)

set (project USpeakNative)
project (${project} LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_POSITION_INDEPENDENT_CODE ON)

if (${CMAKE_BUILD_TYPE} MATCHES "Release")
    set (CMAKE_CXX_FLAGS " /MD /DEBUG:NONE /O2 /Ob2")
else ()
    set (CMAKE_CXX_FLAGS " /MDd /DEBUG:FULL /Od /Ob0 /Wall")
endif ()

add_subdirectory(external/fmt)
add_subdirectory(external/opus)
add_subdirectory(external/libnyquist)

set (THREADS_PREFER_PTHREAD_FLAG ON)
find_package (Threads REQUIRED)

add_library(${project} STATIC
    helpers.h
    uspeaklite.cpp
    uspeaklite.h
    uspeakpacket.h
    uspeakframecontainer.cpp
    uspeakframecontainer.h
    uspeakvolume.cpp
    uspeakvolume.h
    uspeakresampler.cpp
    uspeakresampler.h
    opuscodec/opuscodec.h
    opuscodec/opuscodec.cpp
    opuscodec/opuserror.h
    opuscodec/bandmode.h
    opuscodec/bitrates.h
    opuscodec/opusapp.h
    opuscodec/opusframetime.h
    internal/scopedspinlock.h
    internal/scopedtrylock.h
)

target_include_directories(${project} PRIVATE
    external/libnyquist/include
    external/opus/include
    external/fmt/include
)

target_link_libraries(${project} PRIVATE
    ${CMAKE_DL_LIBS}
    Threads::Threads
    # Opus::opus # Libnyquist already links against opus
    fmt::fmt
    libnyquist
)


add_executable(USpeakTest
    test/main.cpp
    test/base64.h
)

add_subdirectory(test/json)

target_include_directories(USpeakTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    external/libnyquist/include
    test/json/include
)
target_link_libraries(USpeakTest PRIVATE
    ${project}
    nlohmann_json
    libnyquist
)
