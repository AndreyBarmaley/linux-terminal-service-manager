cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -O0 -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG")
set(CMAKE_CXX_FLAGS_PROFILER "-O2 -pg -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall -Wno-sign-compare -Wno-unused-function -Wno-unused-variable")

option(LTSM_BUILD_STD_MAP "usage std::unordered_map" OFF)
option(LTSM_BUILD_XCB_ERRORS "usage xcb_errors" OFF)
option(LTSM_BUILD_X11VNC "build x11vnc utility" OFF)
option(LTSM_BUILD_VNC2IMAGE "build vnc2image utility" OFF)
option(LTSM_BUILD_VNC2SDL "build vnc2sdl client" OFF)
option(LTSM_BUILD_TESTS "build test utilities" OFF)
option(LTSM_BUILD_COVERAGE_TESTS "sonarcloud coverage tests" OFF)
option(LTSM_CHANNELS "ltsm channels support (dev. only)" OFF)
#option(LTSM_CHANNEL_RUTOKEN "rutoken support (dev)" OFF)
option(LTSM_CUPS_BACKEND "cups support" OFF)
 
if(LTSM_BUILD_COVERAGE_TESTS)
    set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} --coverage -DLTSM_COVERAGE_TEST")
    set(LTSM_BUILD_TESTS ON CACHE BOOL "build test utilities" FORCE)
    set(LTSM_BUILD_X11VNC ON CACHE BOOL "build x11vnc utility" FORCE)
    set(LTSM_BUILD_VNC2IMAGE ON CACHE BOOL "build vnc2image utility" FORCE)
endif()

include(FindPkgConfig)
set(CMAKE_FIND_FRAMEWORK LAST)

# LTSM common lib
project(LTSM_common VERSION  20221004.1)

pkg_search_module(SDL2 REQUIRED sdl2>=2.0.1)
pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image>=2.0.1)
pkg_search_module(XCB REQUIRED xcb)
pkg_search_module(XCB_SHM REQUIRED xcb-shm)
pkg_search_module(XCB_DAMAGE REQUIRED xcb-damage)
pkg_search_module(XCB_XFIXES REQUIRED xcb-xfixes)
pkg_search_module(XCB_XTEST REQUIRED xcb-xtest)
pkg_search_module(XCB_RANDR REQUIRED xcb-randr)

add_compile_options(${XCB_CFLAGS})
add_compile_options(${XCB_SHM_CFLAGS})
add_compile_options(${XCB_XFIXES_CFLAGS})
add_compile_options(${XCB_XTEST_CFLAGS})
add_compile_options(${XCB_RANDR_CFLAGS})
add_compile_options(-DJSMN_STATIC)

if(LTSM_BUILD_STD_MAP)
    add_compile_options(-DLTSM_BUILD_STD_MAP)
endif()


if(LTSM_BUILD_XCB_ERRORS)
    add_compile_options(-DLTSM_BUILD_XCB_ERRORS)
endif()

if(LTSM_CHANNELS)
    include_directories(src/channels)
endif()

add_library(LTSM_common STATIC)
add_subdirectory(src/common)

pkg_search_module(XCB_XKB REQUIRED xcb-xkb)
pkg_search_module(XKBCOMMON_X11 REQUIRED xkbcommon-x11)
add_compile_options(${XCB_XKB_CFLAGS})
add_compile_options(${XKBCOMMON_X11_CFLAGS})
link_libraries(${XCB_XKB_LIBRARIES})
link_libraries(${XKBCOMMON_X11_LIBRARIES})

link_libraries(${XCB_LIBRARIES})
link_libraries(${XCB_SHM_LIBRARIES})
link_libraries(${XCB_DAMAGE_LIBRARIES})
link_libraries(${XCB_XFIXES_LIBRARIES})
link_libraries(${XCB_XTEST_LIBRARIES})
link_libraries(${XCB_RANDR_LIBRARIES})
link_libraries(pthread)
link_libraries(stdc++fs)

# LTSM service
project(LTSM_service VERSION 20221004.1)

find_library(PAM_LIBRARIES NAMES pam)
find_package(sdbus-c++ REQUIRED)
find_path(PAM_INCLUDE_PATH NAMES security/pam_appl.h pam_appl.h)

add_executable(LTSM_service src/service/ltsm_service.cpp)

if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
    target_compile_options(LTSM_service PRIVATE -DSDBUS_ADDRESS_SUPPORT)
endif()

if(LTSM_CHANNELS)
    target_compile_options(LTSM_service PRIVATE -DLTSM_CHANNELS)
    target_sources(LTSM_service PRIVATE src/channels/channel_system.cpp)
endif()

target_include_directories(LTSM_service PRIVATE src/common src/librfb src/channels src/dbus-glue ${PAM_INCLUDE_PATH})
target_link_libraries(LTSM_service LTSM_common SDBusCpp::sdbus-c++ ${PAM_LIBRARIES})

set_target_properties(LTSM_service PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM connector
project(LTSM_connector VERSION 20221004.1)

option(LTSM_WITH_RDP "add rdp protocol support" ON)

find_package(sdbus-c++ REQUIRED)
find_package(GnuTLS REQUIRED)
find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

add_executable(LTSM_connector
    src/connector/ltsm_connector.cpp
    src/connector/ltsm_connector_vnc.cpp
    src/librfb/librfb_server.cpp
    src/librfb/librfb_x11server.cpp
    src/librfb/ltsm_librfb.cpp)

if(LTSM_WITH_RDP)
    pkg_search_module(FreeRDP2 REQUIRED freerdp2)
    pkg_search_module(WinPR2 REQUIRED winpr2)
    target_compile_options(LTSM_connector PRIVATE ${FreeRDP2_CFLAGS} ${WinPR2_CFLAGS} -DLTSM_WITH_RDP)
    target_link_libraries(LTSM_connector ${FreeRDP2_LIBRARIES} ${WinPR2_LIBRARIES})
    target_sources(LTSM_connector PRIVATE src/connector/ltsm_connector_rdp.cpp)
endif()

if(LTSM_CHANNELS)
    target_compile_options(LTSM_connector PRIVATE -DLTSM_CHANNELS)
    target_sources(LTSM_connector PRIVATE src/channels/channel_system.cpp)
endif()

target_compile_options(LTSM_connector PRIVATE -DLTSM_SOCKET_TLS -DLTSM_SOCKET_ZLIB -DLTSM_WITH_SDBUS ${GNUTLS_DEFINITIONS})
target_include_directories(LTSM_connector PRIVATE src/common src/channels src/librfb src/dbus-glue ${GNUTLS_INCLUDE_DIR})
target_link_libraries(LTSM_connector LTSM_common SDBusCpp::sdbus-c++ ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES} z)
set_target_properties(LTSM_connector PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM sdl2x11
project(LTSM_sdl2x11 VERSION 20221004.1)
add_executable(LTSM_sdl2x11 src/sdl2x11/ltsm_sdl2x11.cpp src/sdl2x11/ltsm_sdl_wrapper.cpp)

target_compile_options(LTSM_sdl2x11 PRIVATE ${SDL2_CFLAGS})
target_include_directories(LTSM_sdl2x11 PRIVATE src/common)
target_link_libraries(LTSM_sdl2x11 LTSM_common ${SDL2_LIBRARIES})

set_target_properties(LTSM_sdl2x11 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM helper
project(LTSM_helper VERSION 20221004.1)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(sdbus-c++ REQUIRED)
find_package(Qt5 COMPONENTS Core Gui DBus Widgets REQUIRED)

add_executable(LTSM_helper
    src/qt_helper/main.cpp
    src/qt_helper/ltsm_helperwindow.ui
    src/qt_helper/ltsm_helperwindow.cpp
    src/qt_helper/ltsm_helper.qrc)

target_include_directories(LTSM_helper PRIVATE src/common src/dbus-glue)
target_link_libraries(LTSM_helper LTSM_common Qt5::Core Qt5::Gui Qt5::Widgets SDBusCpp::sdbus-c++)
set_target_properties(LTSM_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM sessions
project(LTSM_sessions VERSION 20221004.1)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Core Gui DBus Widgets REQUIRED)

add_executable(LTSM_sessions
    src/qt_sessions/main.cpp
    src/qt_sessions/ltsm_sessions.ui
    src/qt_sessions/ltsm_sessions.cpp
    src/qt_sessions/ltsm_sessions.qrc)

target_link_libraries(LTSM_sessions Qt5::Core Qt5::Gui Qt5::DBus Qt5::Widgets)
set_target_properties(LTSM_sessions PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(LTSM_BUILD_X11VNC)
    # LTSM x11vnc
    project(LTSM_x11vnc VERSION 20221004.1)

    add_executable(LTSM_x11vnc
        src/x11vnc/ltsm_connector_x11vnc.cpp
        src/x11vnc/ltsm_x11vnc.cpp
        src/librfb/librfb_server.cpp
        src/librfb/librfb_x11server.cpp
        src/librfb/ltsm_librfb.cpp)

    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

    target_compile_options(LTSM_x11vnc PRIVATE -DLTSM_SOCKET_TLS -DLTSM_SOCKET_ZLIB ${GNUTLS_DEFINITIONS})
    target_include_directories(LTSM_x11vnc PRIVATE src/common src/channels src/librfb src/x11vnc src/connector ${GNUTLS_INCLUDE_DIR})
    target_link_libraries(LTSM_x11vnc LTSM_common ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES} z)

    set_target_properties(LTSM_x11vnc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_VNC2IMAGE)
    # LTSM vnc2image
    project(LTSM_vnc2image VERSION 20221004.1)

    add_executable(LTSM_vnc2image
        src/vnc2image/ltsm_vnc2image.cpp
        src/librfb/librfb_client.cpp
        src/librfb/ltsm_librfb.cpp)

    find_package(PNG REQUIRED)
    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

    target_compile_options(LTSM_vnc2image PRIVATE -DLTSM_SOCKET_TLS -DLTSM_SOCKET_ZLIB -DLTSM_WITH_PNG ${GNUTLS_DEFINITIONS} ${PNG_DEFINITIONS})
    target_include_directories(LTSM_vnc2image PRIVATE src/common src/channels src/librfb src/vnc2image src/connector ${GNUTLS_INCLUDE_DIR} ${PNG_INCLUDE_DIR})
    target_link_libraries(LTSM_vnc2image LTSM_common ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES} ${PNG_LIBRARIES} z)

    set_target_properties(LTSM_vnc2image PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_TESTS)
    add_subdirectory(src/tests)
endif()

if(LTSM_BUILD_VNC2SDL)
    # LTSM vnc2sdl
    project(LTSM_vnc2sdl VERSION 20221004.1)

    add_executable(LTSM_vnc2sdl
        src/vnc2sdl/ltsm_vnc2sdl.cpp
        src/sdl2x11/ltsm_sdl_wrapper.cpp
        src/librfb/librfb_client.cpp
        src/librfb/ltsm_librfb.cpp)

    find_package(PNG REQUIRED)
    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

    if(LTSM_CHANNELS)
        find_library(XKBFILE_LIBRARY NAMES xkbfile libxkbfile REQUIRED)
        find_library(X11_LIBRARY NAMES X11 libX11 REQUIRED)
        target_compile_options(LTSM_vnc2sdl PRIVATE -DLTSM_CHANNELS)
        target_sources(LTSM_vnc2sdl PRIVATE src/channels/channel_system.cpp)
        target_link_libraries(LTSM_vnc2sdl LTSM_common ${XKBFILE_LIBRARY} ${X11_LIBRARY})
    endif()

    target_compile_options(LTSM_vnc2sdl PRIVATE -DLTSM_SOCKET_TLS -DLTSM_SOCKET_ZLIB -DLTSM_WITH_PNG ${SDL2_CFLAGS} ${GNUTLS_DEFINITIONS} ${PNG_DEFINITIONS})
    target_include_directories(LTSM_vnc2sdl PRIVATE src/common src/channels src/librfb src/sdl2x11 src/vnc2sdl src/connector ${GNUTLS_INCLUDE_DIR} ${PNG_INCLUDE_DIR})
    target_link_libraries(LTSM_vnc2sdl LTSM_common ${SDL2_LIBRARIES} ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES} ${PNG_LIBRARIES} z)

    set_target_properties(LTSM_vnc2sdl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_CUPS_BACKEND)
    # LTSM_cups_backend
    project(LTSM_cups_backend VERSION 20221004.1)

    add_executable(LTSM_cups_backend
        src/cups_backend/main.cpp)

    find_package(Cups REQUIRED)

    target_include_directories(LTSM_cups_backend PRIVATE src/common ${CUPS_INCLUDE_DIR})
    target_link_libraries(LTSM_cups_backend LTSM_common z)

    set_target_properties(LTSM_cups_backend PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

