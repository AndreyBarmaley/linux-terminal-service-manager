# LTSM service

find_library(PAM_LIBRARIES NAMES pam)
find_library(PAM_MISC_LIBRARIES NAMES pam_misc)
find_path(PAM_INCLUDE_PATH NAMES security/pam_appl.h pam_appl.h)

target_sources(${CUR_TARGET} PRIVATE ltsm_service.cpp)
target_include_directories(${CUR_TARGET} PRIVATE ${PAM_INCLUDE_PATH})
target_link_libraries(${CUR_TARGET} PRIVATE ${PAM_LIBRARIES} ${PAM_MISC_LIBRARIES})

if(LTSM_BUILD_COVERAGE_TESTS)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_BUILD_COVERAGE_TESTS)
endif()

