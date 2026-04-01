/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

// shm access flags
#include <sys/stat.h>

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <thread>
#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>

#ifdef LTSM_WITH_SYSTEMD
#include <systemd/sd-login.h>
#include <systemd/sd-daemon.h>
#endif

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_connector.h"
#include "ltsm_font_psf.h"

#include "ltsm_connector_proto.h"
#ifdef LTSM_WITH_RDP
#include "ltsm_connector_rdp.h"
#endif

#include "ltsm_render_primitives.h"

using namespace std::chrono_literals;

namespace LTSM::Connector {
    //
    void connectorHelp(const char* prog) {
#ifdef LTSM_WITH_RDP
        auto proto = { "LTSM", "VNC" /* deprecated */, "RDP", "AUTO" };
#else
        auto proto = { "LTSM", "VNC" /* deprecated */ };
#endif

        std::cout << "usage: " << prog << " --config <path> --type <" <<
                  Tools::join(proto, "|") << ">" << std::endl;
    }

    int autoDetectType(void) {
        auto fd = fileno(stdin);
        struct pollfd fds = {};
        fds.fd = fd;
        fds.events = POLLIN;

        // has input
        if(0 < poll(& fds, 1, 1)) {
            int val = std::fgetc(stdin);
            std::ungetc(val, stdin);
            return val;
        }

        return -1;
    }

    std::string homeRuntime(void) {
        std::string home("/tmp");

        if(auto info = Tools::getUidInfo(getuid()); info->home() != nullptr) {
            home.assign(info->home());
        }

        return home;
    }

#ifdef LTSM_WITH_AUDIT
    void AuditConnector::auditRemoteConnected(const std::string & ipaddr) const {
        auditUserMessage(AUDIT_SERVICE_START, "remote connected", nullptr, ipaddr.c_str(), nullptr, 1);
    }

    void AuditConnector::auditRemoteDisconnected(const std::string & ipaddr) const {
        auditUserMessage(AUDIT_SERVICE_STOP, "remote disconnected", nullptr, ipaddr.c_str(), nullptr, 1);
    }
#endif

    /* DBusProxy */
    DBusProxy::DBusProxy(const ConnectorType & type, const std::filesystem::path & confile, bool debug)
        : ApplicationJsonConfig("ltsm_connector", confile),
#ifdef SDBUS_2_0_API
        ProxyInterfaces(sdbus::createSystemBusConnection(), sdbus::ServiceName {LTSM::dbus_manager_service_name}, sdbus::ObjectPath {LTSM::dbus_manager_service_path})
#else
        ProxyInterfaces(sdbus::createSystemBusConnection(), LTSM::dbus_manager_service_name, LTSM::dbus_manager_service_path)
#endif
    {
        _remoteaddr.assign("local");

        switch(type) {
            case ConnectorType::RDP:
                _conntype = "rdp";
                break;

            case ConnectorType::VNC:
                _conntype = "vnc";
                break;

            case ConnectorType::LTSM:
                _conntype = "ltsm";
                break;
        }

        if(auto env = std::getenv("REMOTE_ADDR")) {
            _remoteaddr.assign(env);
        }

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }

#ifdef LTSM_WITH_AUDIT
        auditLog = std::make_unique<AuditConnector>();
        auditLog->auditRemoteConnected(_remoteaddr);
#endif

        registerProxy();

        Application::info("service started, uid: {}, gid: {}, pid: {}, version: {}",
                        getuid(), getgid(), getpid(), LTSM::service_version);

        if(auto home = homeRuntime(); 0 == chdir(home.c_str())) {
            Application::info("{}: working dir: `{}'", NS_FuncNameV, home);
        } else {
            Application::warning("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "chdir", strerror(errno), errno);
        }
    }

    DBusProxy::~DBusProxy() {
#ifdef LTSM_WITH_AUDIT
        auditLog->auditRemoteDisconnected(_remoteaddr);
#endif
        unregisterProxy();
    }

    const std::string & DBusProxy::connectorType(void) const {
        return _conntype;
    }

    bool DBusProxy::xcbConnect(int screen, XCB::RootDisplay & xcbDisplay) {
        std::string xauthFile = busDisplayAuthFile(screen);
        Application::info("{}: display: {}, xauthfile: {}", NS_FuncNameV, screen, xauthFile);
        setenv("XAUTHORITY", xauthFile.c_str(), 1);
        std::filesystem::path socketPath = Tools::x11UnixPath(screen);

        const uint32_t sessTimeout = configGetInteger("session:timeout", 5000);

        // wait display starting
        bool waitSocket = Tools::waitCallable<std::chrono::milliseconds>(sessTimeout, 100, [ &socketPath ]() {
            return Tools::checkUnixSocket(socketPath);
        });

        if(! waitSocket) {
            Application::error("{}: checkUnixSocket failed, `{}'", NS_FuncNameV, socketPath);
            return false;
        }

        try {
            xcbDisplay.displayReconnect(screen);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            return false;
        }

        _xcbDisplayNum = screen;
        return true;
    }

    int DBusProxy::displayNum(void) const {
        return _xcbDisplayNum;
    }

    void DBusProxy::xcbDisableMessages(bool f) {
        _xcbDisable = f;
    }

    bool DBusProxy::xcbAllowMessages(void) const {
        return ! _xcbDisable;
    }

    std::string DBusProxy::checkFileOption(const std::string & param) const {
        auto fileName = config().getString(param);
        std::error_code err;

        if(! fileName.empty() && ! std::filesystem::exists(fileName, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                            NS_FuncNameV, "exists", err.value(), err.message(), fileName);
            fileName.clear();
        }

        return fileName;
    }

    void DBusProxy::onClearRenderPrimitives(const int32_t & display) {
        if(display == displayNum()) {
            Application::debug(DebugType::Dbus, "{}: display: {}", NS_FuncNameV, display);

            for(const auto & ptr : _renderPrimitives) {
                if(auto prim = ptr.get()) {
                    serverScreenUpdateRequest(prim->xcbRegion());
                }
            }

            _renderPrimitives.clear();
        }
    }

    void DBusProxy::onAddRenderRect(const int32_t & display,
                                    const TupleRegion & rect, const TupleColor & color, const bool & fill) {
        if(display == displayNum()) {
            Application::debug(DebugType::Dbus, "{}: display: {}", NS_FuncNameV, display);

            _renderPrimitives.emplace_back(std::make_unique<RenderRect>(rect, color, fill));
            serverScreenUpdateRequest(tupleRegionToXcbRegion(rect));
        }
    }

    void DBusProxy::onAddRenderText(const int32_t & display, const std::string & text,
                                    const TuplePosition & pos, const TupleColor & color) {
        if(display == displayNum()) {
            Application::debug(DebugType::Dbus, "{}: display: {}", NS_FuncNameV, display);

            const TupleRegion rect = std::make_tuple(std::get<0>(pos), std::get<1>(pos),
                                     _systemfont.width * text.size(), _systemfont.height);

            _renderPrimitives.emplace_back(std::make_unique<RenderText>(text, rect, color));
            serverScreenUpdateRequest(tupleRegionToXcbRegion(rect));
        }
    }

    void DBusProxy::onPingConnector(const int32_t & display) {
        if(display == displayNum()) {
            Application::debug(DebugType::Dbus, "{}: display: {}",
                               NS_FuncNameV, display);

            std::thread([this, display]() {
                this->busConnectorAlive(display);
            }).detach();
        }
    }

    void DBusProxy::renderPrimitivesToFB(FrameBuffer & fb) const {
        for(const auto & ptr : _renderPrimitives) {
            if(auto prim = static_cast<const RenderRect*>(ptr.get())) {
                prim->renderTo(fb);
            }
        }
    }

    void DBusProxy::checkIdleTimeout(void) {
        if(_idleTimeoutSec &&
           _idleTimeoutSec < std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - _idleSessionTp).count()) {
            busSessionIdleTimeout(displayNum());
            _idleSessionTp = std::chrono::steady_clock::now();
        }
    }

    /* Connector::startService */
    int startService(int argc, const char** argv) {
        std::string type{"auto"};
        std::filesystem::path confile;
        bool debug = false;
        
        for(int it = 1; it < argc; ++it) {
            if(0 == std::strcmp(argv[it], "--type") && it + 1 < argc) {
                type = Tools::lower(argv[it + 1]);
                it = it + 1;
            } else if(0 == std::strcmp(argv[it], "--config") && it + 1 < argc) {
                confile.assign(argv[it + 1]);
                it = it + 1;
            } else if(0 == std::strcmp(argv[it], "--debug")) {
                debug = true;
            } else {
                connectorHelp(argv[0]);
                return EXIT_SUCCESS;
            }
        }

        Application::setDebugTarget(DebugTarget::Syslog, "ltsm_connector");
        Application::setDebugLevel(DebugLevel::Info);

        // signals
        signal(SIGPIPE, SIG_IGN);
        std::unique_ptr<DBusProxy> connector;

#ifdef LTSM_WITH_RDP

        // protocol up
        if(type == "auto") {
            if(int first = autoDetectType(); first == 0x03) {
                connector = std::make_unique<ConnectorRdp>(confile, debug);
            }
        } else if(type == "rdp") {
            connector = std::make_unique<ConnectorRdp>(confile, debug);
        }

#endif

        if(! connector) {
            connector = std::make_unique<ConnectorLtsm>(confile, debug);
        }

        int res = 0;

        try {
#ifdef LTSM_WITH_SYSTEMD
            sd_notify(0, "READY=1");
#endif
            res = connector->communication();
#ifdef LTSM_WITH_SYSTEMD
            sd_notify(0, "STOPPING=1");
#endif
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            // terminated connection: exit normal
            res = EXIT_SUCCESS;
        }

        return res;
    }

} // namespace LTSM::Connector

using namespace LTSM;

int main(int argc, const char** argv) {
    try {
        return Connector::startService(argc, argv);
    } catch(const sdbus::Error & err) {
        Application::error("sdbus exception: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return EXIT_FAILURE;
}
