project(LTSM_test_framebuf2)
set(CUR_TARGET LTSM_test_framebuf2)

CommonDefaults(FFMPEG ENCODER JSON)

add_executable(${CUR_TARGET} main.cpp)

target_include_directories(${CUR_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/channels
    ${CMAKE_SOURCE_DIR}/src/librfb)

add_subdirectory(${CMAKE_SOURCE_DIR}/src/common ${CUR_TARGET}_common)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/channels ${CUR_TARGET}_channels)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/librfb ${CUR_TARGET}_librfb)

pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image)
target_link_libraries(${CUR_TARGET} PRIVATE ${SDL2_IMAGE_LIBRARIES})

set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
