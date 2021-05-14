# linux-terminal-service-manager
Linux Terminal Service Manager (LTSM) is a set of service programs that allows remote computers to connect to a Linux operating system computer using a remote terminal session (over VNC or RDP)


# Схема взаимодействия компонентов
![ltsm_diagram](https://user-images.githubusercontent.com/8620726/118247282-884e7480-b492-11eb-92a8-d8db95656eee.png)

# LTSM_service
основная служба, менеджер dbus ltsm.service.manager, получает команды от LTSM_connector, запускает login и users сессии на базе Xvfb (Лицензия GPLv3)

# LTSM_connector
сетевой частью занимается служебный xinetd, и это хорошо, LTSM_сonnector является толко обработчиком сетевого протокола VNC и RDP, также он является клиентом dbus ltsm.service.manager, подключается к Xvfb через механизм shared memory (Лицензия AGPLv3)

# LTSM_helper
![ltsm_helper](https://user-images.githubusercontent.com/8620726/118249135-9ac9ad80-b494-11eb-9a5c-ddff59048293.png)

графическая утилита входа в систему, является клиентом dbus ltsm.service.manager (Лицензия GPLv3)

# LTSM_admins
графическая утилита управления сессиями пользователей, является клиентом dbus ltsm.service.manager
