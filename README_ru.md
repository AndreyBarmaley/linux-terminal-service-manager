# linux-terminal-service-manager
Linux Terminal Service Manager (LTSM) это набор программ для организации доступа к рабочему столу (сервер Linux) на основе терминальных сессий (с использованием протокола VNC или RDP)

# Основные зависимости:  
  - systemd, sd-bus  
  - [sdbus-cpp](https://github.com/Kistler-Group/sdbus-cpp)  
  - [json parser](https://github.com/zserge/jsmn)  
  - [freerdp](https://github.com/FreeRDP/FreeRDP)  
  - system libs: gnutls, xcb, zlib  

# Демо доступ
```
vncviewer 62.109.24.208
logins: demo1, demo2, demo3, demo4
pass: demo
```

# Docker демонстрация
```
docker pull docker.io/ltsm/devel
docker run -i -t docker.io/ltsm/devel
```

# Схема взаимодействия компонентов
![ltsm_diagram](https://user-images.githubusercontent.com/8620726/118247282-884e7480-b492-11eb-92a8-d8db95656eee.png)  
реализованы следующие компоненты:

# LTSM_service
основная служба, менеджер dbus ltsm.service.manager, получает команды от LTSM_connector, запускает login и users сессии на базе Xvfb (Лицензия GPLv3)  
see also: [wiki: LTSM service](https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-service)  

# LTSM_connector
является только обработчиком сетевого протокола VNC и RDP, а основной сетевой частью занимается служебный xinetd/(systemd sockets), также он является клиентом dbus ltsm.manager.service, подключается к Xvfb через механизм shared memory (Лицензия на коннектор Affero GPLv3)  
see also: [wiki: LTSM connector](https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-connector)  

# LTSM_helper
![ltsm_helper](https://user-images.githubusercontent.com/8620726/123924335-66914a00-d979-11eb-9025-9d6bcf3fa250.png)  
графическая утилита входа в систему, является клиентом dbus ltsm.manager.service (Лицензия GPLv3)  
see also: [wiki: LTSM config](https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-config-(full-description))  

# LTSM_sessions
![ltsm_session](https://user-images.githubusercontent.com/8620726/119793454-23e5d900-bec6-11eb-9978-ee31f44360ae.png)  
графическая утилита управления сессиями пользователей, является клиентом dbus ltsm.manager.service (Лицензия GPLv3)  
see also: [wiki: LTSM administrator](https://github.com/AndreyBarmaley/linux-terminal-service-manager/wiki/LTSM-administrator)  

# LTSM_vnc2sdl
 
Это экспериментальный графический клиент, в котором реализован механизм множественных каналов данных, максимально до 253.  

**Основные реализованные улучшения:**
* Работает буфер обмена **utf8** в обе стороны (проблема для большинства клиентов VNC)
* Автоматическая раскладка клавиатуры, в приоритете всегда раскладка со стороны клиента (на стороне сервера ничего не нужно настраивать)
* Передача файлов через **drag & drop** (со стороны клиента на удаленную виртуальную сессию)
* Реализована печать файлов (с помощью дополнительного [backend для CUPS](https://github.com/AndreyBarmaley/linux-terminal-service-manager/tree/main/src/cups_backend))
* Реализован редирект звука через **pulseaudio**
* Реализована поддержка **pkcs11** через редирект **pcscd**
* Реализовано сканирование документов (с помощью дополнительного [backend для SANE](https://github.com/AndreyBarmaley/linux-terminal-service-manager/tree/main/src/sane_backend))
 
Механизм каналов реализован через абстрактные схемы ```unix://, file://, socket://```, и режим доступа ```ReadOnly, WriteOnly, ReadWrite```.  
Например для обычной передачи файла создается типовой канал (клиент сервер): ```file:///src_file1 (ReadOnly)``` и ```file:///dst_file2 (WriteOnly)```, далее в сессии пользователя запускаются информационные GUI диалоги о передаче и выборе папки назначения, после которых файл автоматически сохранится в удаленной сессии (дополнительно реализованы уведомления через dbus desktop notify).  
C помощью данной схемы каналов возможна передача любого потока данных в обе стороны, но инициатором создания канала всегда является сервер.  

Печать со стороны сервера (в удаленной сессии пользователя) реализуется таким образом - на сервере в **cups** добавляется собственный **backend** для печати, который знает в какой **unix сокет** печатать в сессии пользователя, со стороны клиента поток можно отправлять в сетевой принтер ```socket://127.0.0.1:9100```, также в локальный **cups** или в ```file:///dev/usb/lp0```. В этой схеме системный администратор настраивает принтер только один раз для сервера, например командой ```lpadmin -p ltsm -D "LTSM virtual printer" -E -v ltsm:///var/run/ltsm/cups/username -m raw```. Модуль для **cups** автоматически получает сокет из этой схемы, меняет **username** на реального и отправляет в него задание печати. Этот же формат сокета на стороне LTSM сервиса  назначается в конфигурационном файле. Сокеты на стороне сервера работают в **listen** режиме поэтому допускаются множественные подключения от клиента. Каждое подключение занимает один канал (253 макс). Как только с одной из сторон освободится дескриптор считается что передача данных завершена и канал можно использовать повторно. Редирект звука, сканирования и pcscd работает по **аналогичной схеме**.

Все эти реализованные возможности вы можете протестировать в версии **docker**.  

Ведутся работы по добавлению **folder redirection** (через **FUSE**), по записи видео всех рабочих сессий (видеофиксация), по ускорению графики с использованием GPU (Cuda API).  
