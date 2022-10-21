# linux-terminal-service-manager
Linux Terminal Service Manager (LTSM) is a set of service programs that allows remote computers to connect to a Linux operating system computer using a remote terminal session (over VNC or RDP)

# Linux Only!
main dependencies:  
  - systemd, sd-bus  
  - [sdbus-cpp](https://github.com/Kistler-Group/sdbus-cpp)  
  - [json parser](https://github.com/zserge/jsmn)  
  - [freerdp](https://github.com/FreeRDP/FreeRDP)
  - system libs: gnutls, xcb, zlib  

## Developer indicators
[![Build Status](https://github.com/AndreyBarmaley/linux-terminal-service-manager/actions/workflows/cmake.yml/badge.svg)](https://github.com/AndreyBarmaley/linux-terminal-service-manager/actions)
[![Bugs](https://sonarcloud.io/api/project_badges/measure?project=AndreyBarmaley_linux-terminal-service-manager&metric=bugs)](https://sonarcloud.io/summary/new_code?id=AndreyBarmaley_linux-terminal-service-manager)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=AndreyBarmaley_linux-terminal-service-manager&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=AndreyBarmaley_linux-terminal-service-manager)
[![Security Rating](https://sonarcloud.io/api/project_badges/measure?project=AndreyBarmaley_linux-terminal-service-manager&metric=security_rating)](https://sonarcloud.io/summary/new_code?id=AndreyBarmaley_linux-terminal-service-manager)
[![Reliability Rating](https://sonarcloud.io/api/project_badges/measure?project=AndreyBarmaley_linux-terminal-service-manager&metric=reliability_rating)](https://sonarcloud.io/summary/new_code?id=AndreyBarmaley_linux-terminal-service-manager)
[![OpenHub Rating](https://openhub.net/p/linux-terminal-service-manager/widgets/project_thin_badge?format=gif)](https://openhub.net/p/linux-terminal-service-manager)

# Demo access
```
--- vnc
vncviewer 62.109.24.208
--- rdp
xfreerdp /v:62.109.24.208

logins: demo1, demo2, demo3, demo4
pass: demo
```

# Docker demonstration
```
docker pull docker.io/ltsm/devel
docker run -i -t docker.io/ltsm/devel
```

# The scheme of interaction of components  
![ltsm_diagram](https://user-images.githubusercontent.com/8620726/118247282-884e7480-b492-11eb-92a8-d8db95656eee.png)  
The following components are implemented:  

# LTSM_service
The main service, dbus owner *ltsm.service.manager*, receives commands from LTSM_connector, and starts login and users sessions based on Xvfb (GPLv3 license)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-service  

# LTSM_connector
It is just a graphics protocol handler, and the main network part is handled by the service xinetd/(systemd sockets), and it is also a dbus client *ltsm.manager.service*, it connects to Xvfb via the shared memory mechanism (Affero GPLv3 license)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-connector  

# LTSM_helper
![ltsm_helper](https://user-images.githubusercontent.com/8620726/123924335-66914a00-d979-11eb-9025-9d6bcf3fa250.png)  
GUI login utility, and it is a dbus client *ltsm.manager.service* (GPLv3 license)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-config-(full-description)  

# LTSM_sessions
![ltsm_session](https://user-images.githubusercontent.com/8620726/119793454-23e5d900-bec6-11eb-9978-ee31f44360ae.png)  
GUI users sessions management utility, and it is a dbus client *ltsm.manager.service* (GPLv3 license)  
![ltsm_show_session](https://user-images.githubusercontent.com/8620726/123924343-67c27700-d979-11eb-9802-723d043f9f6f.png)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-administrator  

# LTSM_vnc2sdl

This is an experimental graphical client that implements the mechanism of multiple data channels, up to a maximum of 253.

**Main improvements implemented:**
* Works utf8 clipboard in both directions (problem for most VNC clients)
* Automatic keyboard layout, client-side layout always takes precedence (nothing needs to be configured on the server-side)
* File transfer via drag & drop (client side to remote virtual session)
* Implemented file printing (using an additional backend for cups)
* Implemented audio redirect via pulseaudio
* Implemented pkcs11 support via pcscd redirect
 
The mechanism of pipes is implemented through the abstract schemes unix://, file://, socket://, and the access mode ReadOnly, WriteOnly, ReadWrite.  
For example, for a normal file transfer, a typical channel is created (client-server): file:///src_file1 (ReadOnly) file:///dst_file2 (WriteOnly), then in the user session, informational GUI dialogs are launched about the transfer and selection of the destination folder, after which the file automatically saved in the remote session.  
Also, using this mechanism, it is possible to transfer any data stream in both directions, but the initiator of creating a channel is always the server.  

So printing from the server side (in a remote user session) is implemented in this way - on the server, cups adds its own backend to configure the printer, which knows which unix socket to print in the user session, from the client side, the stream can be sent to the socket:// network printer 127.0.0.1:9100, also to local cups or file:///dev/usb/lp0. In this scheme, the system administrator configures the printer only once per server. The audio and pcscd redirect works according to a similar scheme.  

You can test all implemented features in the docker version.  

Currently in development to add a scanning redirect (SANE backend) and folder redirection (from FUSE) and video recording of all sessions.  
