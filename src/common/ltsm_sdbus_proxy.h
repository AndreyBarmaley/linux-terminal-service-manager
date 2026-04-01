/***********************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_SDBUS_PROXY_
#define _LTSM_SDBUS_PROXY_

#include <sdbus-c++/sdbus-c++.h>

#include "ltsm_application.h"

#include <cstdint>
#include <unordered_map>
#include <memory>
#include <string>

namespace LTSM::SDBus {
    class ProxyBase {
      protected:
        std::unique_ptr<sdbus::IProxy> proxy;
        std::string interface;

      public:
        ProxyBase(std::unique_ptr<sdbus::IConnection> conn, const std::string &name, const std::string &path,
                     const std::string &inter) : interface(inter) {
#ifdef SDBUS_2_0_API
            proxy = sdbus::createProxy(std::move(conn), sdbus::ServiceName {name}, sdbus::ObjectPath {path});
#else
            proxy = sdbus::createProxy(std::move(conn), name, path);
#endif
        }

        virtual ~ProxyBase() {}

        ProxyBase(const ProxyBase &) = delete;
        ProxyBase & operator=(const ProxyBase &) = delete;

        ProxyBase(ProxyBase &&) = default;
        ProxyBase & operator=(ProxyBase &&) = default;


        template <typename Type>
        Type getProperty(const std::string &prop) const {
            try {
                return static_cast<Type>(proxy->getProperty(prop).onInterface(interface));
            } catch(const sdbus::Error &err) {
                Application::error("{}: failed, sdbus error: {}, msg: {}",
                                   NS_FuncNameV, err.getName(), err.getMessage());
            }

            return {};
        }

        template <typename Ret, typename... Args>
        Ret CallProxyMethod(const std::string &methodName,
                            Args &&...args) const {
            Ret res;

            try {
                proxy->callMethod(methodName)
                .onInterface(interface)
                .withArguments(std::forward<Args>(args)...)
                .storeResultsTo(res);
            } catch(const sdbus::Error &err) {
                Application::error("{}: failed, sdbus error: {}, msg: {}",
                                   NS_FuncNameV, err.getName(), err.getMessage());
            }

            return res;
        }

        template <typename... Args>
        void CallProxyMethodNoResult(const std::string &methodName,
                                     Args &&...args) const {
            try {
                proxy->callMethod(methodName)
                .onInterface(interface)
                .withArguments(std::forward<Args>(args)...);
            } catch(const sdbus::Error &err) {
                Application::error("{}: failed, sdbus error: {}, msg: {}",
                                   NS_FuncNameV, err.getName(), err.getMessage());
            }
        }
    };

    class SystemProxy : public ProxyBase {
      public:
        SystemProxy(const std::string &name, const std::string &path, const std::string &inter)
            :  ProxyBase(sdbus::createSystemBusConnection(), name, path, inter) {}
    };

    class SessionProxy : public ProxyBase {
      public:
        SessionProxy(const std::string &name, const std::string &path, const std::string &inter)
            :  ProxyBase(sdbus::createSessionBusConnection(), name, path, inter) {}

        SessionProxy(const std::string &addr, const std::string &name, const std::string &path, const std::string &inter)
            : ProxyBase(sdbus::createSessionBusConnectionWithAddress(addr), name, path, inter) {}
    };

    class SessionProxySignals : public SessionProxy {
      protected:
        std::unordered_map<std::string, sdbus::SignalSubscriber> signals;

      public:
        SessionProxySignals(const std::string &name, const std::string &path,
                            const std::string &inter) : SessionProxy(name, path, inter) {}

        template <typename Func>
        void registerSignal(const std::string &signalName, Func &&signalHandler) {
            signals.emplace(signalName, proxy->uponSignal(signalName)
                            .onInterface(interface)
                            .call(std::forward<Func>(signalHandler)));
        }

        inline void finishRegistration(void) {
#ifndef SDBUS_2_0_API
            proxy->finishRegistration();
#endif
        }

        void unregisterSignal(const std::string &signalName) {
            signals.erase(signalName);
        }
    };
}

#endif
