target_sources(${CUR_TARGET} PRIVATE main.cpp)

find_package(Cups REQUIRED)
target_include_directories(${CUR_TARGET} PRIVATE {CUPS_INCLUDE_DIR})
