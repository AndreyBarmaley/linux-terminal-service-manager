project(ltsm_test_json)
set(CUR_TARGET ltsm_test_json)

CommonDefaults(JSON)

add_executable(${CUR_TARGET} main.cpp)

target_include_directories(${CUR_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common)

add_subdirectory(${CMAKE_SOURCE_DIR}/src/common ${CUR_TARGET}_common)
set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
