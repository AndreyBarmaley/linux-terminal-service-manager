#!/bin/bash

sdbus-c++-xml2cpp service.xml --adaptor=ltsm_service_adaptor.h --proxy=ltsm_service_proxy.h
sdbus-c++-xml2cpp fuse.xml --adaptor=ltsm_fuse_adaptor.h --proxy=ltsm_fuse_proxy.h
