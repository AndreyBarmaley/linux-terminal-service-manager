target_sources(${CUR_TARGET} PRIVATE ltsm_vnc2sdl.cpp)

add_subdirectory(pcsc ${CUR_TARGET}_pcsc)
add_subdirectory(fuse ${CUR_TARGET}_fuse)
add_subdirectory(audio ${CUR_TARGET}_audio)
add_subdirectory(pkcs11 ${CUR_TARGET}_pkcs11)
