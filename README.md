# linux-terminal-service-manager
Linux Terminal Service Manager (LTSM) is a set of service programs that allows remote computers to connect to a Linux operating system computer using a remote terminal session (over VNC or RDP)

# Linux Only!
main dependencies:  
  -systemd  
  -sd-bus, sdbus-cpp (https://github.com/Kistler-Group/sdbus-cpp)  
  -libdeflate (https://github.com/ebiggers/libdeflate)  
  -XCB system libs  

# Demo access
in the near future, it is being prepared, expect it.  
if you have the ability to public VDS (4 core, 2Gb memory, 4Gb disk), please let me know.  

# Схема взаимодействия компонентов
![ltsm_diagram](https://user-images.githubusercontent.com/8620726/118247282-884e7480-b492-11eb-92a8-d8db95656eee.png)

# LTSM_service
основная служба, менеджер dbus ltsm.service.manager, получает команды от LTSM_connector, запускает login и users сессии на базе Xvfb (Лицензия GPLv3)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-service

# LTSM_connector
сетевой частью занимается служебный xinetd, и это хорошо, LTSM_сonnector является только обработчиком сетевого протокола VNC и RDP, также он является клиентом dbus ltsm.manager.service, подключается к Xvfb через механизм shared memory (Лицензия AGPLv3)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-connector

# LTSM_helper
![ltsm_helper](https://user-images.githubusercontent.com/8620726/118249135-9ac9ad80-b494-11eb-9a5c-ddff59048293.png)

графическая утилита входа в систему, является клиентом dbus ltsm.manager.service (Лицензия GPLv3)  
see also: https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-config-(full-description)

# LTSM_admins
графическая утилита управления сессиями пользователей, является клиентом dbus ltsm.manager.service
main features:  
  -connected to users display  
  -disconnecting a session  
  -in development  
