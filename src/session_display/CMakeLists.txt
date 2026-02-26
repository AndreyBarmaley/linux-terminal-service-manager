# LTSM session

target_compile_options(${CUR_TARGET} PUBLIC "-Wno-narrowing")
target_sources(${CUR_TARGET} PRIVATE ltsm_display_session.cpp)
 