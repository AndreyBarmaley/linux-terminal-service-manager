# LTSM helper

target_sources(${CUR_TARGET} PRIVATE main.cpp ltsm_helperwindow.cpp
        ltsm_ldap_wrapper.cpp ltsm_helperwindow.ui ltsm_helper.qrc)

target_link_libraries(${CUR_TARGET} PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network)

find_library(LDAP_LIBRARY ldap REQUIRED)
find_library(LBER_LIBRARY lber REQUIRED)

target_link_libraries(${CUR_TARGET} PRIVATE ${LBER_LIBRARY} ${LDAP_LIBRARY})

if(LTSM_WITH_PKCS11)
    target_sources(${CUR_TARGET} PRIVATE ltsm_pkcs11_session.cpp)
endif()
