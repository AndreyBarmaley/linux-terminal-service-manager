cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -O0 -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG -D_GLIBCXX_DEBUG -D_LIBCPP_ENABLE_DEBUG_MODE")
set(CMAKE_CXX_FLAGS_PROFILER "-O2 -pg -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall -Wno-sign-compare -Wno-unused-function -Wno-unused-variable")

option(LTSM_BUILD_XCB_ERRORS "usage xcb_errors" OFF)
option(LTSM_BUILD_SERVER "build server" OFF)
option(LTSM_BUILD_CLIENT "build client" ON)
option(LTSM_GSSAPI "GSSAPI security" OFF)
option(LTSM_FFMPEG "ffmpeg encoding support" OFF)
option(LTSM_BUILD_SYSTEMD "build with systemd" OFF)

option(LTSM_BUILD_PROFILING "build with profiling" OFF)
option(LTSM_BUILD_SANITIZE "build with sanitize" OFF)
option(LTSM_BUILD_TESTS "build test utilities" OFF)
option(LTSM_BUILD_FB_FAST_CYCLE "build FrameBuffer plain cycles" OFF)
option(LTSM_PKCS11_AUTH "ltsm pkcs11 auth support" ON)

option(LTSM_BUILD_VNC2IMAGE "build vnc2image utility" OFF)
option(LTSM_BUILD_X11VNC "build x11vnc utility" OFF)
#option(LTSM_BUILD_COVERAGE_TESTS "sonarcloud coverage tests" OFF)
#option(LTSM_WITH_STD_MAP "usage std::unordered_map" OFF)

if(LTSM_BUILD_SANITIZE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment")
endif()

if(LTSM_BUILD_PROFILING)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

if(LTSM_BUILD_FB_FAST_CYCLE)
    set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} -DFB_FAST_CYCLE")
endif()

if(LTSM_BUILD_CLIENT)
    if(LTSM_FFMPEG)
        set(LTSM_CLIENT_DECODING_H264 ON CACHE BOOL "add client decoding: H264")
        set(LTSM_CLIENT_DECODING_AV1 ON CACHE BOOL "add client decoding: AV1")
        set(LTSM_CLIENT_DECODING_VP8 ON CACHE BOOL "add client decoding: VP8")
    else()
        unset(LTSM_CLIENT_DECODING_H264 CACHE)
        unset(LTSM_CLIENT_DECODING_AV1 CACHE)
        unset(LTSM_CLIENT_DECODING_VP8 CACHE)
    endif()
    set(LTSM_CLIENT_DECODING_LZ4 ON CACHE BOOL "add client decoding: LZ4")
    set(LTSM_CLIENT_DECODING_QOI ON CACHE BOOL "add client decoding: QOI")
    set(LTSM_CLIENT_DECODING_TJPG ON CACHE BOOL "add client decoding: TJPG")
else()
    unset(LTSM_CLIENT_DECODING_H264 CACHE)
    unset(LTSM_CLIENT_DECODING_AV1 CACHE)
    unset(LTSM_CLIENT_DECODING_VP8 CACHE)
    unset(LTSM_CLIENT_DECODING_LZ4 CACHE)
    unset(LTSM_CLIENT_DECODING_QOI CACHE)
    unset(LTSM_CLIENT_DECODING_TJPG CACHE)
endif()

if(LTSM_BUILD_SERVER)
    set(LTSM_WITH_RDP ON CACHE BOOL "add rdp protocol support")
    set(LTSM_BUILD_SERVICE ON CACHE BOOL "build ltsm service")
    set(LTSM_BUILD_CONNECTOR ON CACHE BOOL "build ltsm connector")
    set(LTSM_BUILD_SDL2X11 ON CACHE BOOL "build sdl2x11 utility")
    set(LTSM_BUILD_SESSIONS ON CACHE BOOL "build sessions manager utility")
    set(LTSM_BUILD_HELPER ON CACHE BOOL "build helper utility")
    set(LTSM_CUPS_BACKEND ON CACHE BOOL "cups backend build")
    set(LTSM_FUSE2SESSION ON CACHE BOOL "session fuse support")
    set(LTSM_AUDIO2SESSION ON CACHE BOOL "session audio support")
    set(LTSM_PCSC2SESSION ON CACHE BOOL "session PCSC support")
else()
    unset(LTSM_WITH_RDP CACHE)
    unset(LTSM_BUILD_SERVICE CACHE)
    unset(LTSM_BUILD_CONNECTOR CACHE)
    unset(LTSM_BUILD_SDL2X11 CACHE)
    unset(LTSM_BUILD_SESSIONS CACHE)
    unset(LTSM_BUILD_HELPER CACHE)
    unset(LTSM_CUPS_BACKEND CACHE)
    unset(LTSM_FUSE2SESSION CACHE)
    unset(LTSM_AUDIO2SESSION CACHE)
    unset(LTSM_PCSC2SESSION CACHE)
endif()

if(LTSM_BUILD_COVERAGE_TESTS)
    set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} --coverage -DLTSM_COVERAGE_TEST")

    set(LTSM_WITH_RDP ON CACHE BOOL "add rdp protocol support" FORCE)
    set(LTSM_BUILD_SERVICE ON CACHE BOOL "build ltsm service" FORCE)
    set(LTSM_BUILD_CONNECTOR ON CACHE BOOL "build ltsm connector" FORCE)

    set(LTSM_BUILD_SDL2X11 ON CACHE BOOL "build sdl2x11 utility" FORCE)
    set(LTSM_BUILD_SESSIONS ON CACHE BOOL "build sessions manager utility" FORCE)
    set(LTSM_BUILD_HELPER ON CACHE BOOL "build helper utility" FORCE)

    set(LTSM_BUILD_TESTS ON CACHE BOOL "build test utilities" FORCE)
    set(LTSM_BUILD_X11VNC ON CACHE BOOL "build x11vnc utility" FORCE)
    set(LTSM_BUILD_VNC2IMAGE ON CACHE BOOL "build vnc2image utility" FORCE)
endif()

if(LTSM_BUILD_TESTS)
    set(LTSM_FFMPEG ON CACHE BOOL "ffmpeg encoding support")
    set(LTSM_CLIENT_DECODING_H264 ON CACHE BOOL "add client decoding: H264")
    set(LTSM_CLIENT_DECODING_AV1 ON CACHE BOOL "add client decoding: AV1")
    set(LTSM_CLIENT_DECODING_VP8 ON CACHE BOOL "add client decoding: VP8")
    set(LTSM_CLIENT_DECODING_LZ4 ON CACHE BOOL "add client decoding: LZ4")
    set(LTSM_CLIENT_DECODING_QOI ON CACHE BOOL "add client decoding: QOI")
    set(LTSM_CLIENT_DECODING_TJPG ON CACHE BOOL "add client decoding: TJPG")
endif()

include(FindPkgConfig)
set(CMAKE_FIND_FRAMEWORK LAST)

# LTSM common lib
project(LTSM_common)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    add_definitions(-D__LINUX__)
endif()

if(MINGW)
    set(CMAKE_CXX_FLAGS "-fpermissive")
endif()

pkg_search_module(SDL2 REQUIRED sdl2>=2.0.1)
pkg_search_module(P11KIT REQUIRED p11-kit-1)

if(LTSM_WITH_SYSTEMD)
    pkg_search_module(SYSTEMD REQUIRED libsystemd)
    add_compile_options(-DWITH_SYSTEMD)
endif()

if(LTSM_PKCS11_AUTH)
    add_compile_options(-DLTSM_PKCS11_AUTH)
    include_directories(src/session_pkcs11 ${P11KIT_INCLUDE_DIRS})
endif()

add_compile_options(-DWITH_JSON -DJSMN_STATIC)

if(LTSM_WITH_STD_MAP)
    add_compile_options(-DLTSM_BUILD_STD_MAP)
endif()

if(LTSM_BUILD_XCB_ERRORS)
    add_compile_options(-DLTSM_BUILD_XCB_ERRORS)
endif()

if(UNIX)
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
else()
    unset(LTSM_BUILD_XCB_ERRORS CACHE)
    unset(LTSM_WITH_SYSTEMD CACHE)
endif()

add_library(LTSM_common STATIC)
add_subdirectory(src/common)

if(UNIX)
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
endif()

if(LTSM_WITH_SYSTEMD)
    link_libraries(${SYSTEMD_LIBRARIES})
endif()

link_libraries(pthread)
link_libraries(stdc++fs)
link_libraries(z)

if(LTSM_BUILD_SERVICE)
    # LTSM service
    project(LTSM_service)

    add_custom_command(
        OUTPUT ltsm_service_adaptor.h
        COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus/ltsm_service.xml --adaptor=ltsm_service_adaptor.h
        DEPENDS src/dbus/ltsm_service.xml
        COMMENT "Generating D-Bus LTSM_SERVICE adaptor interface"
    )

    find_library(PAM_LIBRARIES NAMES pam)
    find_library(PAM_MISC_LIBRARIES NAMES pam_misc)

    find_package(sdbus-c++ REQUIRED)
    find_path(PAM_INCLUDE_PATH NAMES security/pam_appl.h pam_appl.h)

    add_executable(LTSM_service
        ltsm_service_adaptor.h
        src/service/ltsm_service.cpp
        src/channels/channel_system.cpp)

    if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
        target_compile_definitions(LTSM_service PRIVATE -DSDBUS_ADDRESS_SUPPORT)
    endif()

    target_include_directories(LTSM_service PRIVATE
            src/common src/channels src/session_fuse src/session_audio src/session_pcsc src/librfb
            ${PAM_INCLUDE_PATH} ${CMAKE_BINARY_DIR})

    target_link_libraries(LTSM_service
            LTSM_common SDBusCpp::sdbus-c++
            ${PAM_LIBRARIES} ${PAM_MISC_LIBRARIES})

    set_target_properties(LTSM_service PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_CONNECTOR)
    # LTSM connector
    project(LTSM_connector)

    add_custom_command(
        OUTPUT ltsm_service_proxy.h
        COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus/ltsm_service.xml --proxy=ltsm_service_proxy.h
        DEPENDS src/dbus/ltsm_service.xml
        COMMENT "Generating D-Bus LTSM_SERVICE proxy interface"
    )

    find_package(sdbus-c++ REQUIRED)
    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX gnutlsxx)

    pkg_search_module(SDBUS_CPP REQUIRED sdbus-c++)
    pkg_search_module(LZ4 REQUIRED liblz4)
    pkg_search_module(TURBOJPEG REQUIRED libturbojpeg)

    add_executable(LTSM_connector
        ltsm_service_proxy.h
        src/connector/ltsm_connector.cpp
        src/connector/ltsm_connector_vnc.cpp
        src/channels/channel_system.cpp
        src/librfb/librfb_encodings.cpp
        src/librfb/librfb_extclip.cpp
        src/librfb/librfb_server.cpp
        src/librfb/librfb_x11server.cpp
        src/librfb/ltsm_librfb.cpp)

    target_compile_options(LTSM_connector PRIVATE -DLTSM_ENCODING)

    if(LTSM_FFMPEG)
        pkg_search_module(AVDEVICE REQUIRED libavdevice)
        pkg_search_module(AVFORMAT REQUIRED libavformat)
        pkg_search_module(AVCODEC REQUIRED libavcodec)
        pkg_search_module(AVSWSCALE REQUIRED libswscale)
        pkg_search_module(AVSWRESAMPLE REQUIRED libswresample)
        pkg_search_module(AVUTIL REQUIRED libavutil)

        target_sources(LTSM_connector PRIVATE src/librfb/ffmpeg_tools.cpp src/librfb/librfb_ffmpeg.cpp)

        target_compile_options(LTSM_connector PRIVATE
                ${AVDEVICE_CFLAGS} ${AVFORMAT_CFLAGS} ${AVCODEC_CFLAGS}
                ${AVSWSCALE_CFLAGS} ${AVSWRESAMPLE_CFLAGS} ${AVUTIL_CFLAGS}
                -DLTSM_ENCODING_FFMPEG)

        target_link_options(LTSM_connector PRIVATE
                ${AVDEVICE_LDFLAGS} ${AVFORMAT_LDFLAGS} ${AVCODEC_LDFLAGS}
                ${AVSWSCALE_LDFLAGS} ${AVSWRESAMPLE_LDFLAGS} ${AVUTIL_LDFLAGS})

        target_link_libraries(LTSM_connector
                ${AVDEVICE_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES}
                ${AVSWSCALE_LIBRARIES} ${AVSWRESAMPLE_LIBRARIES} ${AVUTIL_LIBRARIES})
    endif()

    if(LTSM_GSSAPI)
	pkg_search_module(GSSAPI REQUIRED krb5-gssapi)
        target_sources(LTSM_connector PRIVATE src/common/ltsm_gsslayer.cpp)

	target_compile_definitions(LTSM_connector PRIVATE -DLTSM_WITH_GSSAPI)
	target_compile_options(LTSM_connector PRIVATE ${GSSAPI_CFLAGS})
	target_include_directories(LTSM_connector PRIVATE include ${GSSAPI_INCLUDE_DIR})
	target_link_libraries(LTSM_connector ${GSSAPI_LIBRARIES})
    endif()

    if(LTSM_WITH_RDP)
        pkg_search_module(FreeRDP2 REQUIRED freerdp2)
        pkg_search_module(WinPR2 REQUIRED winpr2)
        target_sources(LTSM_connector PRIVATE src/connector/ltsm_connector_rdp.cpp)

        target_compile_definitions(LTSM_connector PRIVATE -DLTSM_WITH_RDP)
        target_compile_options(LTSM_connector PRIVATE ${FreeRDP2_CFLAGS} ${WinPR2_CFLAGS})
        target_link_libraries(LTSM_connector ${FreeRDP2_LIBRARIES} ${WinPR2_LIBRARIES})
    endif()

    if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
        target_compile_options(LTSM_connector PRIVATE -DSDBUS_ADDRESS_SUPPORT)
    endif()

    target_compile_definitions(LTSM_connector PRIVATE -DLTSM_WITH_GNUTLS -DLTSM_WITH_SDBUS
        ${GNUTLS_DEFINITIONS} ${sdbus-c++_DEFINITIONS})

    target_compile_options(LTSM_connector PRIVATE ${LZ4_CFLAGS} ${TURBOJPEG_CFLAGS})

    target_include_directories(LTSM_connector PRIVATE src/common src/channels src/librfb src/dbus 
        ${GNUTLS_INCLUDE_DIR} ${LZ4_INCLUDE_DIR} ${TURBOJPEG_INCLUDE_DIR} ${CMAKE_BINARY_DIR})

    target_link_libraries(LTSM_connector LTSM_common SDBusCpp::sdbus-c++
        ${GNUTLS_LIBRARIES} ${GNUTLSXX} ${LZ4_LIBRARIES} ${TURBOJPEG_LIBRARIES} z)

    set_target_properties(LTSM_connector PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_SDL2X11)
    # LTSM sdl2x11
    project(LTSM_sdl2x11)

    add_executable(LTSM_sdl2x11 src/sdl2x11/ltsm_sdl2x11.cpp src/sdl2x11/ltsm_sdl_wrapper.cpp)

    target_compile_options(LTSM_sdl2x11 PRIVATE ${SDL2_CFLAGS})
    target_include_directories(LTSM_sdl2x11 PRIVATE src/common)
    target_link_libraries(LTSM_sdl2x11 LTSM_common ${SDL2_LIBRARIES})

    set_target_properties(LTSM_sdl2x11 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_HELPER)
    # LTSM helper
    project(LTSM_helper)

    add_custom_command(
        OUTPUT ltsm_service_proxy.h
        COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus/ltsm_service.xml --proxy=ltsm_service_proxy.h
        DEPENDS src/dbus/ltsm_service.xml
        COMMENT "Generating D-Bus LTSM_SERVICE proxy interface"
    )

    find_package(Qt5 COMPONENTS Core Gui Widgets Network REQUIRED)
    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX gnutlsxx)

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

    find_package(sdbus-c++ REQUIRED)

    add_executable(LTSM_helper
        ltsm_service_proxy.h
        src/qt_helper/main.cpp
        src/qt_helper/ltsm_helperwindow.cpp
        src/qt_helper/ltsm_ldap_wrapper.cpp
        src/qt_helper/ltsm_helperwindow.ui
        src/qt_helper/ltsm_helper.qrc)

    if(LTSM_PKCS11_AUTH)
        target_sources(LTSM_helper PRIVATE
            src/session_pkcs11/ltsm_pkcs11_session.cpp
            src/session_pkcs11/ltsm_pkcs11_wrapper.cpp)

        find_library(LDAP_LIBRARY ldap REQUIRED)
        find_library(LBER_LIBRARY lber REQUIRED)

        target_link_libraries(LTSM_helper ${LBER_LIBRARY} ${LDAP_LIBRARY} dl)
    endif()

    target_compile_definitions(LTSM_helper PRIVATE -DLTSM_WITH_GNUTLS ${GNUTLS_DEFINITIONS})

    target_include_directories(LTSM_helper PRIVATE src/common src/dbus src/session_pkcs11
         ${GNUTLS_INCLUDE_DIR} ${CMAKE_BINARY_DIR})

    target_link_libraries(LTSM_helper LTSM_common
        Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network
        SDBusCpp::sdbus-c++ ${GNUTLS_LIBRARIES} ${GNUTLSXX})

    set_target_properties(LTSM_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_SESSIONS)
    # LTSM sessions
    project(LTSM_sessions)

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
endif()

if(LTSM_BUILD_X11VNC)
    # LTSM x11vnc
    project(LTSM_x11vnc)

    add_executable(LTSM_x11vnc
        src/x11vnc/ltsm_connector_x11vnc.cpp
        src/x11vnc/ltsm_x11vnc.cpp
        src/librfb/librfb_encodings.cpp
        src/librfb/librfb_extclip.cpp
        src/librfb/librfb_server.cpp
        src/librfb/librfb_x11server.cpp
        src/channels/channel_system.cpp
        src/librfb/ltsm_librfb.cpp)

    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX gnutlsxx)

    target_compile_options(LTSM_x11vnc PRIVATE -DLTSM_WITH_GNUTLS ${GNUTLS_DEFINITIONS})
    target_include_directories(LTSM_x11vnc PRIVATE src/common src/channels src/librfb src/x11vnc src/connector ${GNUTLS_INCLUDE_DIR})
    target_link_libraries(LTSM_x11vnc LTSM_common ${GNUTLS_LIBRARIES} ${GNUTLSXX} z)

    set_target_properties(LTSM_x11vnc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_VNC2IMAGE)
    # LTSM vnc2image
    project(LTSM_vnc2image)

    add_executable(LTSM_vnc2image
        src/vnc2image/ltsm_vnc2image.cpp
        src/channels/channel_system.cpp
        src/librfb/librfb_decodings.cpp
        src/librfb/librfb_extclip.cpp
        src/librfb/librfb_client.cpp
        src/librfb/ltsm_librfb.cpp)

    find_package(PNG REQUIRED)
    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX gnutlsxx)

    target_compile_definitions(LTSM_vnc2image PRIVATE -DLTSM_WITH_GNUTLS -DLTSM_WITH_PNG ${GNUTLS_DEFINITIONS} ${PNG_DEFINITIONS})
    target_compile_options(LTSM_vnc2image PRIVATE ${SDL2_CFLAGS})
    target_include_directories(LTSM_vnc2image PRIVATE src/common src/channels src/librfb src/vnc2image src/connector ${GNUTLS_INCLUDE_DIR} ${PNG_INCLUDE_DIR})
    target_link_libraries(LTSM_vnc2image LTSM_common ${GNUTLS_LIBRARIES} ${GNUTLSXX} ${SDL2_LIBRARIES} ${PNG_LIBRARIES} z)

    set_target_properties(LTSM_vnc2image PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_TESTS)
    add_subdirectory(src/tests)
endif()

if(LTSM_BUILD_CLIENT)
    # LTSM vnc2sdl
    project(LTSM_client)

    add_executable(LTSM_client
        src/vnc2sdl/ltsm_vnc2sdl.cpp
        src/sdl2x11/ltsm_sdl_wrapper.cpp
        src/channels/channel_system.cpp
        src/librfb/librfb_decodings.cpp
        src/librfb/librfb_client.cpp
        src/librfb/ltsm_librfb.cpp
        src/session_audio/ltsm_audio_client.cpp
        src/session_audio/ltsm_audio_decoder.cpp
        src/session_fuse/ltsm_fuse_client.cpp
        src/session_pcsc/ltsm_pcsc_client.cpp)

    if(UNIX)
        target_sources(LTSM_client PRIVATE src/librfb/librfb_extclip.cpp)
        target_sources(LTSM_client PRIVATE src/librfb/librfb_x11client.cpp)
    endif()

    if(MINGW)
        unset(LTSM_PKCS11_AUTH CACHE)
        target_include_directories(LTSM_client PRIVATE src/common/mingw)
        target_sources(LTSM_client PRIVATE src/librfb/librfb_extclip.cpp)
        target_sources(LTSM_client PRIVATE src/librfb/librfb_winclient.cpp)
        target_link_libraries(LTSM_client wsock32 winscard)
    endif()

    find_package(PNG REQUIRED)
    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX gnutlsxx)

    target_compile_definitions(LTSM_client PRIVATE -DLTSM_CLIENT -DLTSM_DECODING)

    if(LTSM_CLIENT_DECODING_LZ4)
        pkg_search_module(LZ4 REQUIRED liblz4)
        target_compile_options(LTSM_client PRIVATE ${LZ4_CFLAGS})
	target_compile_definitions(LTSM_client PRIVATE -DLTSM_DECODING_LZ4)
        target_include_directories(LTSM_client PRIVATE ${LZ4_INCLUDE_DIR})
        target_link_libraries(LTSM_client ${LZ4_LIBRARIES})
    endif()

    if(LTSM_CLIENT_DECODING_QOI)
	target_compile_definitions(LTSM_client PRIVATE -DLTSM_DECODING_QOI)
    endif()

    if(LTSM_CLIENT_DECODING_TJPG)
        pkg_search_module(TURBOJPEG REQUIRED libturbojpeg)
        target_compile_options(LTSM_client PRIVATE ${TURBOJPEG_CFLAGS})
	target_compile_definitions(LTSM_client PRIVATE -DLTSM_DECODING_TJPG)
        target_include_directories(LTSM_client PRIVATE ${TURBOJPEG_INCLUDE_DIR})
        target_link_libraries(LTSM_client ${TURBOJPEG_LIBRARIES})
    endif()

    pkg_search_module(P11KIT REQUIRED p11-kit-1)
    pkg_search_module(OPUS REQUIRED opus)
    pkg_search_module(OPENAL REQUIRED openal)

    if(LTSM_PKCS11_AUTH)
        target_sources(LTSM_client PRIVATE
            src/session_pkcs11/ltsm_pkcs11_client.cpp
            src/session_pkcs11/ltsm_pkcs11_wrapper.cpp)

        target_link_libraries(LTSM_client ${P11KIT_LIBRARIES} dl)
    endif()

    if(OPUS_FOUND)
        target_compile_options(LTSM_client PRIVATE -DLTSM_WITH_OPUS ${OPUS_CFLAGS})
        target_include_directories(LTSM_client PRIVATE ${OPUS_INCLUDE_DIR})
        target_link_libraries(LTSM_client ${OPUS_LIBRARIES})
    endif()

    # OpenAL playback preffered
    if(OPENAL_FOUND)
        target_sources(LTSM_client PRIVATE src/session_audio/ltsm_audio_openal.cpp)
        target_compile_definitions(LTSM_client PRIVATE -DLTSM_WITH_PAYBACK_OPENAL)

        # maybe linker error: incorrect size
        target_compile_options(LTSM_client PRIVATE -fPIC)

        target_compile_options(LTSM_client PRIVATE ${OPENAL_CFLAGS})
        target_include_directories(LTSM_client PRIVATE ${OPENAL_INCLUDE_DIR})
        target_link_libraries(LTSM_client ${OPENAL_LIBRARIES})
    else()
        # PulseAudio maybe use
        pkg_search_module(PULSEAUDIO REQUIRED libpulse)
        pkg_search_module(PULSESIMPLE REQUIRED libpulse-simple)

        target_sources(LTSM_client PRIVATE src/session_audio/ltsm_audio_pulse.cpp)
        target_compile_definitions(LTSM_client PRIVATE -DLTSM_WITH_PAYBACK_PULSE)

        target_compile_options(LTSM_client PRIVATE ${PULSEAUDIO_CFLAGS})
        target_include_directories(LTSM_client PRIVATE ${PULSEAUDIO_INCLUDE_DIR})
        target_link_libraries(LTSM_client ${PULSEAUDIO_LIBRARIES} ${PULSESIMPLE_LIBRARIES})
    endif()

    if(LTSM_FFMPEG)
        pkg_search_module(AVDEVICE REQUIRED libavdevice)
        pkg_search_module(AVFORMAT REQUIRED libavformat)
        pkg_search_module(AVCODEC REQUIRED libavcodec)
        pkg_search_module(AVSWSCALE REQUIRED libswscale)
        pkg_search_module(AVSWRESAMPLE REQUIRED libswresample)
        pkg_search_module(AVUTIL REQUIRED libavutil)

        if(LTSM_CLIENT_DECODING_H264)
	    target_compile_definitions(LTSM_client PRIVATE -DLTSM_DECODING_H264)
        endif()

        if(LTSM_CLIENT_DECODING_AV1)
	    target_compile_definitions(LTSM_client PRIVATE -DLTSM_DECODING_AV1)
        endif()

        if(LTSM_CLIENT_DECODING_VP8)
	    target_compile_definitions(LTSM_client PRIVATE -DLTSM_DECODING_VP8)
        endif()

        target_sources(LTSM_client PRIVATE src/librfb/ffmpeg_tools.cpp src/librfb/librfb_ffmpeg.cpp)
        target_compile_options(LTSM_client PRIVATE ${AVDEVICE_CFLAGS} ${AVFORMAT_CFLAGS} ${AVCODEC_CFLAGS} ${AVSWSCALE_CFLAGS} ${AVSWRESAMPLE_CFLAGS} ${AVUTIL_CFLAGS} -DLTSM_DECODING_FFMPEG)
        target_link_options(LTSM_client PRIVATE ${AVDEVICE_LDFLAGS} ${AVFORMAT_LDFLAGS} ${AVCODEC_LDFLAGS} ${AVSWSCALE_LDFLAGS} ${AVSWRESAMPLE_LDFLAGS} ${AVUTIL_LDFLAGS})
        target_link_libraries(LTSM_client ${AVDEVICE_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVSWSCALE_LIBRARIES} ${AVSWRESAMPLE_LIBRARIES} ${AVUTIL_LIBRARIES})
    endif()

    if(LTSM_GSSAPI)
	pkg_search_module(GSSAPI REQUIRED krb5-gssapi)
        target_sources(LTSM_client PRIVATE src/common/ltsm_gsslayer.cpp)

	target_compile_definitions(LTSM_client PRIVATE ${GSSAPI_DEFINITIONS} -DLTSM_WITH_GSSAPI)
	target_include_directories(LTSM_client PRIVATE include ${GSSAPI_INCLUDE_DIR})
	target_link_libraries(LTSM_client ${GSSAPI_LIBRARIES})
    endif()

    if(UNIX)
        find_library(XKBFILE_LIBRARY NAMES xkbfile libxkbfile REQUIRED)
        find_library(X11_LIBRARY NAMES X11 libX11 REQUIRED)

        pkg_search_module(PCSCLITE REQUIRED libpcsclite)

        target_compile_definitions(LTSM_client PRIVATE -DLTSM_X11)
        target_compile_options(LTSM_client PRIVATE ${PCSCLITE_CFLAGS})
        target_include_directories(LTSM_client PRIVATE ${PCSCLITE_INCLUDE_DIR})
        target_link_libraries(LTSM_client ${XKBFILE_LIBRARY} ${X11_LIBRARY} ${PCSCLITE_LIBRARIES})
    endif()

    target_compile_definitions(LTSM_client PRIVATE -DLTSM_WITH_GNUTLS -DLTSM_WITH_PNG
        ${GNUTLS_DEFINITIONS} ${PNG_DEFINITIONS})

    target_compile_options(LTSM_client PRIVATE ${SDL2_CFLAGS})

    target_include_directories(LTSM_client PRIVATE src/common src/channels src/session_audio
        src/session_pcsc src/librfb src/sdl2x11 src/vnc2sdl src/connector 
        ${GNUTLS_INCLUDE_DIR} ${PNG_INCLUDE_DIR})

    target_link_libraries(LTSM_client LTSM_common ${SDL2_LIBRARIES}
        ${GNUTLS_LIBRARIES} ${GNUTLSXX} ${PNG_LIBRARIES} z)

    set_target_properties(LTSM_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_CUPS_BACKEND)
    # LTSM_cups_backend
    project(LTSM_cups_backend)

    add_executable(LTSM_cups_backend
        src/cups_backend/main.cpp)

    find_package(Cups REQUIRED)

    target_include_directories(LTSM_cups_backend PRIVATE src/common ${CUPS_INCLUDE_DIR})
    target_link_libraries(LTSM_cups_backend LTSM_common z)

    set_target_properties(LTSM_cups_backend PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_FUSE2SESSION)
    # LTSM_fuse2session
    project(LTSM_fuse2session)

    add_custom_command(
        OUTPUT ltsm_fuse_adaptor.h
        COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus/ltsm_fuse.xml --adaptor=ltsm_fuse_adaptor.h
        DEPENDS src/dbus/ltsm_fuse.xml
        COMMENT "Generating D-Bus LTSM_FUSE adaptor interface"
    )

    find_package(sdbus-c++ REQUIRED)
    pkg_search_module(FUSE3 REQUIRED fuse3)   

    add_executable(LTSM_fuse2session
        ltsm_fuse_adaptor.h
        src/session_fuse/ltsm_fuse_session.cpp)

    if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
        target_compile_options(LTSM_fuse2session PRIVATE -DSDBUS_ADDRESS_SUPPORT)
    endif()

    target_compile_options(LTSM_fuse2session PRIVATE ${FUSE3_CFLAGS})
    target_include_directories(LTSM_fuse2session PRIVATE src/common src/dbus ${FUSE3_INCLUDE_DIR} ${CMAKE_BINARY_DIR})
    target_link_libraries(LTSM_fuse2session LTSM_common SDBusCpp::sdbus-c++ ${FUSE3_LIBRARIES})

    set_target_properties(LTSM_fuse2session PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_AUDIO2SESSION)
    # LTSM_audio2session
    project(LTSM_audio2session)

    add_custom_command(
        OUTPUT ltsm_audio_adaptor.h
        COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus/ltsm_audio.xml --adaptor=ltsm_audio_adaptor.h
        DEPENDS src/dbus/ltsm_audio.xml
        COMMENT "Generating D-Bus LTSM_AUDIO adaptor interface"
    )

    find_package(sdbus-c++ REQUIRED)
    pkg_search_module(PULSEAUDIO REQUIRED libpulse)
    pkg_search_module(OPUS REQUIRED opus)

    add_executable(LTSM_audio2session
        ltsm_audio_adaptor.h
        src/session_audio/ltsm_audio_pulse.cpp
        src/session_audio/ltsm_audio_encoder.cpp
        src/session_audio/ltsm_audio_session.cpp)

    if(OPUS_FOUND)
        target_compile_options(LTSM_audio2session PRIVATE -DLTSM_WITH_OPUS ${OPUS_CFLAGS})
        target_include_directories(LTSM_audio2session PRIVATE ${OPUS_INCLUDE_DIR})
        target_link_libraries(LTSM_audio2session ${OPUS_LIBRARIES})
    endif()

#    if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
#        target_compile_options(LTSM_audio2session PRIVATE -DSDBUS_ADDRESS_SUPPORT)
#    endif()

    target_compile_options(LTSM_audio2session PRIVATE ${PULSEAUDIO_CFLAGS})
    target_include_directories(LTSM_audio2session PRIVATE src/common src/dbus ${PULSEAUDIO_INCLUDE_DIR} ${CMAKE_BINARY_DIR})
    target_link_libraries(LTSM_audio2session LTSM_common SDBusCpp::sdbus-c++ ${PULSEAUDIO_LIBRARIES})

    set_target_properties(LTSM_audio2session PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_PCSC2SESSION)
    # LTSM_pcsc2session
    project(LTSM_pcsc2session)

    add_custom_command(
        OUTPUT ltsm_pcsc_adaptor.h
        COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus/ltsm_pcsc.xml --adaptor=ltsm_pcsc_adaptor.h
        DEPENDS src/dbus/ltsm_pcsc.xml
        COMMENT "Generating D-Bus LTSM_PCSC adaptor interface"
    )

    find_package(sdbus-c++ REQUIRED)
    pkg_search_module(PCSCLITE REQUIRED libpcsclite)

    add_executable(LTSM_pcsc2session
        ltsm_pcsc_adaptor.h
        src/session_pcsc/ltsm_pcsc_session.cpp)

#    if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
#        target_compile_options(LTSM_pcsc2session PRIVATE -DSDBUS_ADDRESS_SUPPORT)
#    endif()

    target_compile_options(LTSM_pcsc2session PRIVATE ${PCSCLITE_CFLAGS})
    target_include_directories(LTSM_pcsc2session PRIVATE src/common src/dbus ${PCSCLITE_INCLUDE_DIR} ${CMAKE_BINARY_DIR})
    target_link_libraries(LTSM_pcsc2session LTSM_common SDBusCpp::sdbus-c++)

    set_target_properties(LTSM_pcsc2session PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
