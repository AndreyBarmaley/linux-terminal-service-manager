function(CommonDefaults)
    set(LTSM_WITH_FB_PARALLELS ON PARENT_SCOPE)
    set(LTSM_WITH_XCB_ERRORS ON PARENT_SCOPE)

    list(APPEND OPTS SDBUS JSON PNG SDL GNUTLS GSSAPI PKCS11 SYSTEMD FFMPEG DECODER ENCODER)

    foreach(ARG IN ITEMS ${OPTS})
        string(JOIN "" PARAM LTSM_WITH_ ${ARG})
        set(${PARAM} OFF PARENT_SCOPE)
    endforeach()

    foreach(ARG IN ITEMS ${ARGV})
        if(ARG IN_LIST OPTS)
            string(JOIN "" PARAM LTSM_WITH_ ${ARG})
            set(${PARAM} ON PARENT_SCOPE)
        endif()
    endforeach()

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(LTSM_WITH_STD_MAP ON PARENT_SCOPE)
    else()
        set(LTSM_WITH_STD_MAP OFF PARENT_SCOPE)
    endif()
endfunction()

add_custom_command(
    OUTPUT ltsm_service_adaptor.h
    COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_service.xml
                    --adaptor=${CMAKE_BINARY_DIR}/src/ltsm_service_adaptor.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_service.xml
    COMMENT "Generating D-Bus LTSM_SERVICE adaptor interface"
)

add_custom_command(
    OUTPUT ltsm_service_proxy.h
    COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_service.xml
                    --proxy=${CMAKE_BINARY_DIR}/src/ltsm_service_proxy.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_service.xml
    COMMENT "Generating D-Bus LTSM_SERVICE proxy interface"
)

add_custom_command(
    OUTPUT ltsm_fuse_adaptor.h
    COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_fuse.xml
                    --adaptor=${CMAKE_BINARY_DIR}/src/ltsm_fuse_adaptor.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_fuse.xml
    COMMENT "Generating D-Bus LTSM_FUSE adaptor interface"
)

add_custom_command(
    OUTPUT ltsm_audio_adaptor.h
    COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_audio.xml
                    --adaptor=${CMAKE_BINARY_DIR}/src/ltsm_audio_adaptor.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_audio.xml
    COMMENT "Generating D-Bus LTSM_AUDIO adaptor interface"
)

add_custom_command(
    OUTPUT ltsm_pcsc_adaptor.h
    COMMAND sdbus-c++-xml2cpp ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_pcsc.xml
                    --adaptor=${CMAKE_BINARY_DIR}/src/ltsm_pcsc_adaptor.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/ltsm_pcsc.xml
    COMMENT "Generating D-Bus LTSM_PCSC adaptor interface"
)

include_directories(${CMAKE_BINARY_DIR}/src)

if(LTSM_BUILD_SERVER)

    set(LTSM_SERVER_BUILD_SERVICE ON CACHE BOOL "server: build service")
    set(LTSM_SERVER_BUILD_CONNECTOR ON CACHE BOOL "server: build connector")
    set(LTSM_SERVER_CONNECTOR_RDP ON CACHE BOOL "server: connector RDP support")
    set(LTSM_SERVER_BUILD_HELPER ON CACHE BOOL "server: build helper")
    set(LTSM_SERVER_BUILD_SESMAN ON CACHE BOOL "server: build session manager")
    set(LTSM_SERVER_BUILD_CUPS ON CACHE BOOL "server: build CUPS backend")
    set(LTSM_SERVER_BUILD_SESSION_AUDIO ON CACHE BOOL "server: build session audio")
    set(LTSM_SERVER_BUILD_SESSION_FUSE ON CACHE BOOL "server: build session fuse")
    set(LTSM_SERVER_BUILD_SESSION_PCSC ON CACHE BOOL "server: build session pcsc")

    if(LTSM_SERVER_BUILD_SERVICE)
        project(LTSM_service)
        set(CUR_TARGET LTSM_service)

        CommonDefaults(SDBUS JSON SYSTEMD)

        add_executable(${CUR_TARGET} ltsm_service_adaptor.h)

        target_include_directories(${CUR_TARGET} PRIVATE common channels librfb
                session_fuse session_audio session_pcsc)

        add_subdirectory(dbus ${CUR_TARGET}_dbus)
        add_subdirectory(common ${CUR_TARGET}_common)
        add_subdirectory(channels ${CUR_TARGET}_channels)
        add_subdirectory(service ${CUR_TARGET}_service)

        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

    if(LTSM_SERVER_BUILD_CONNECTOR)
        project(LTSM_connector)
        set(CUR_TARGET LTSM_connector)

        CommonDefaults(SDBUS JSON SYSTEMD ENCODER)

        set(LTSM_CONNECTOR_GSSAPI_AUTH ON CACHE BOOL "connector: kerberos auth")
        set(LTSM_CONNECTOR_GNUTLS_SUPPORT ON CACHE BOOL "connector: tls support")
        set(LTSM_CONNECTOR_FFMPEG_SUPPORT ON CACHE BOOL "connector: ffmpeg decoder")

        set(LTSM_WITH_GNUTLS ${LTSM_CONNECTOR_GNUTLS_SUPPORT})
        set(LTSM_WITH_GSSAPI ${LTSM_CONNECTOR_GSSAPI_AUTH})
        set(LTSM_WITH_FFMPEG ${LTSM_CONNECTOR_FFMPEG_SUPPORT})

        add_executable(${CUR_TARGET} ltsm_service_proxy.h)
        target_include_directories(${CUR_TARGET} PRIVATE common channels librfb)

        add_subdirectory(dbus ${CUR_TARGET}_dbus)
        add_subdirectory(librfb ${CUR_TARGET}_librfb)
        add_subdirectory(common ${CUR_TARGET}_common)
        add_subdirectory(channels ${CUR_TARGET}_channels)
        add_subdirectory(connector ${CUR_TARGET}_connector)
    
        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

    if(LTSM_SERVER_BUILD_HELPER)
        project(LTSM_helper)
        set(CUR_TARGET LTSM_helper)

        set(LTSM_HELPER_PKCS11_AUTH ON CACHE BOOL "helper: pkcs11 auth")

        CommonDefaults(SDBUS JSON SYSTEMD GNUTLS)

        set(LTSM_WITH_PKCS11 ${LTSM_HELPER_PKCS11_AUTH})

        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        add_executable(${CUR_TARGET} ltsm_service_proxy.h)

        find_package(sdbus-c++ REQUIRED)
        find_package(Qt5 COMPONENTS Core Gui Widgets Network REQUIRED)

        add_subdirectory(dbus ${CUR_TARGET}_dbus)
        add_subdirectory(common ${CUR_TARGET}_common)
        add_subdirectory(qt_helper ${CUR_TARGET}_helper)

        target_include_directories(${CUR_TARGET} PRIVATE common channels)

        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

    if(LTSM_SERVER_BUILD_SESMAN)
        add_subdirectory(tools/qt_sessions)
    endif()

    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)

    if(LTSM_SERVER_BUILD_CUPS)
        project(LTSM_cups_backend)
        set(CUR_TARGET LTSM_cups_backend)

        CommonDefaults(SYSTEMD)

        add_executable(${CUR_TARGET} cups_backend/main.cpp)
        find_package(Cups REQUIRED)

        target_include_directories(${CUR_TARGET} PRIVATE common ${CUPS_INCLUDE_DIR})
        add_subdirectory(common ${CUR_TARGET}_common)

        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

    if(LTSM_SERVER_BUILD_SESSION_PCSC)
        project(LTSM_pcsc2session)
        set(CUR_TARGET LTSM_pcsc2session)

        CommonDefaults(SDBUS SYSTEMD)

        add_executable(${CUR_TARGET} ltsm_pcsc_adaptor.h)
        target_include_directories(${CUR_TARGET} PRIVATE common channels)

        add_subdirectory(dbus ${CUR_TARGET}_dbus)
        add_subdirectory(common ${CUR_TARGET}_common)
        add_subdirectory(session_pcsc ${CUR_TARGET}_pcsc)

        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

    if(LTSM_SERVER_BUILD_SESSION_FUSE)
        project(LTSM_fuse2session)
        set(CUR_TARGET LTSM_fuse2session)

        CommonDefaults(SDBUS SYSTEMD)

        add_executable(${CUR_TARGET} ltsm_fuse_adaptor.h)
        target_include_directories(${CUR_TARGET} PRIVATE common channels)

        add_subdirectory(dbus ${CUR_TARGET}_dbus)
        add_subdirectory(common ${CUR_TARGET}_common)
        add_subdirectory(session_fuse ${CUR_TARGET}_fuse)

        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

    if(LTSM_SERVER_BUILD_SESSION_AUDIO)
        project(LTSM_audio2session)
        set(CUR_TARGET LTSM_audio2session)

        CommonDefaults(SDBUS SYSTEMD)

        add_executable(${CUR_TARGET} ltsm_audio_adaptor.h)
        target_include_directories(${CUR_TARGET} PRIVATE common channels)

        add_subdirectory(dbus ${CUR_TARGET}_dbus)
        add_subdirectory(common ${CUR_TARGET}_common)
        add_subdirectory(session_audio ${CUR_TARGET}_audio)

        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()

else()
    unset(LTSM_SERVER_BUILD_SERVICE CACHE)
    unset(LTSM_SERVER_BUILD_CONNECTOR CACHE)
    unset(LTSM_SERVER_CONNECTOR_RDP CACHE)
    unset(LTSM_SERVER_BUILD_HELPER CACHE)
    unset(LTSM_SERVER_BUILD_SESMAN CACHE)
    unset(LTSM_SERVER_BUILD_CUPS CACHE)
    unset(LTSM_SERVER_BUILD_SESSION_AUDIO CACHE)
    unset(LTSM_SERVER_BUILD_SESSION_FUSE CACHE)
    unset(LTSM_SERVER_BUILD_SESSION_PCSC CACHE)
endif()

if(NOT LTSM_SERVER_BUILD_HELPER)
    unset(LTSM_HELPER_PKCS11_AUTH CACHE)
endif()

if(NOT LTSM_SERVER_BUILD_CONNECTOR)
    unset(LTSM_CONNECTOR_GSSAPI_AUTH CACHE)
    unset(LTSM_CONNECTOR_GNUTLS_SUPPORT CACHE)
    unset(LTSM_CONNECTOR_FFMPEG_SUPPORT CACHE)
endif()

if(LTSM_BUILD_CLIENT)
    project(LTSM_client)
    set(CUR_TARGET LTSM_client)

    if(NOT MINGW)
        set(LTSM_CLIENT_PKCS11_AUTH ON CACHE BOOL "client: pkcs11 auth")
    endif()
    set(LTSM_CLIENT_GSSAPI_AUTH ON CACHE BOOL "client: kerberos auth")
    set(LTSM_CLIENT_GNUTLS_SUPPORT ON CACHE BOOL "client: tls support")
    set(LTSM_CLIENT_FFMPEG_SUPPORT ON CACHE BOOL "client: ffmpeg decoder")
    set(LTSM_CLIENT_SCARD_REDIRECT ON CACHE BOOL "client: smartcard redirect (pcsc)")
    set(LTSM_CLIENT_FOLDER_REDIRECT ON CACHE BOOL "client: folder redirect (fuse)")

    CommonDefaults(JSON PNG SDL DECODER SYSTEMD)

    set(LTSM_WITH_FFMPEG ${LTSM_CLIENT_FFMPEG_SUPPORT})
    set(LTSM_WITH_GSSAPI ${LTSM_CLIENT_GSSAPI_AUTH})
    set(LTSM_WITH_GNUTLS ${LTSM_CLIENT_GNUTLS_SUPPORT})
    set(LTSM_WITH_PKCS11 ${LTSM_CLIENT_PKCS11_AUTH})

    if(APPLE)
        add_executable(${CUR_TARGET} MACOSX_BUNDLE client/ltsm_vnc2sdl.cpp)
    else()
        add_executable(${CUR_TARGET} client/ltsm_vnc2sdl.cpp)
    endif()

    target_compile_definitions(${CUR_TARGET} PRIVATE -DLTSM_CLIENT)
    target_include_directories(${CUR_TARGET} PRIVATE common channels librfb
            session_audio session_fuse session_pcsc)

    if(UNIX AND NOT APPLE)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_X11)
    endif()

    add_subdirectory(common ${CUR_TARGET}_common)
    add_subdirectory(channels ${CUR_TARGET}_channels)
    add_subdirectory(librfb ${CUR_TARGET}_librfb)
    add_subdirectory(client ${CUR_TARGET}_client)

    if(NOT APPLE)
        set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
    endif()
else()
    unset(LTSM_CLIENT_PKCS11_AUTH CACHE)
    unset(LTSM_CLIENT_GSSAPI_AUTH CACHE)
    unset(LTSM_CLIENT_GNUTLS_SUPPORT CACHE)
    unset(LTSM_CLIENT_FFMPEG_SUPPORT CACHE)
    unset(LTSM_CLIENT_SCARD_REDIRECT CACHE)
    unset(LTSM_CLIENT_FOLDER_REDIRECT CACHE)
endif()

if(LTSM_BUILD_TOOLS)
    # vnc2image
    project(LTSM_vnc2image)
    set(CUR_TARGET LTSM_vnc2image)

    CommonDefaults(PNG SDL JSON DECODER)

    add_executable(${CUR_TARGET} tools/vnc2image/ltsm_vnc2image.cpp)
    target_include_directories(${CUR_TARGET} PRIVATE common channels librfb)

    add_subdirectory(common ${CUR_TARGET}_common)
    add_subdirectory(channels ${CUR_TARGET}_channels)
    add_subdirectory(librfb ${CUR_TARGET}_librfb)

    set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # x112vnc
    project(LTSM_x11vnc)
    set(CUR_TARGET LTSM_x11vnc)

    CommonDefaults(GNUTLS JSON ENCODER)

    add_executable(${CUR_TARGET} tools/x11vnc/ltsm_x11vnc.cpp tools/x11vnc/ltsm_connector_x11vnc.cpp)
    target_include_directories(${CUR_TARGET} PRIVATE common channels librfb)

    add_subdirectory(common ${CUR_TARGET}_common)
    add_subdirectory(channels ${CUR_TARGET}_channels)
    add_subdirectory(librfb ${CUR_TARGET}_librfb)

    set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # sdl2x11
    project(LTSM_libsdl2x11)
    set(CUR_TARGET LTSM_sdl2x11)

    CommonDefaults(SDL)

    add_executable(${CUR_TARGET} tools/sdl2x11/ltsm_sdl2x11.cpp)

    target_include_directories(${CUR_TARGET} PRIVATE common)
    add_subdirectory(common ${CUR_TARGET}_common)
    
    set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

endif()

if(LTSM_BUILD_TESTS)
    add_subdirectory(tests)
endif()

