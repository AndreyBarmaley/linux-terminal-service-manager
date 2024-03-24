#!/bin/bash

sdbus-c++-xml2cpp service.xml --adaptor=ltsm_service_adaptor.h --proxy=ltsm_service_proxy.h
sdbus-c++-xml2cpp fuse.xml --adaptor=ltsm_fuse_adaptor.h --proxy=ltsm_fuse_proxy.h
sdbus-c++-xml2cpp audio.xml --adaptor=ltsm_audio_adaptor.h --proxy=ltsm_audio_proxy.h
