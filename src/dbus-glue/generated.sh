#!/bin/bash

sdbus-c++-xml2cpp bindings.xml --adaptor=ltsm_dbus_adaptor.h --proxy=ltsm_dbus_proxy.h
