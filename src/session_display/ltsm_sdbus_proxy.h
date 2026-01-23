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
#include <forward_list>
#include <memory>
#include <string>

namespace LTSM::SDBus {
    class SessionProxy {
      protected:
        std::forward_list<std::string> signals;
        std::unique_ptr<sdbus::IProxy> proxy;
        const std::string interface;

      public:
        SessionProxy(const std::string &name, const std::string &path,
                     const std::string &inter) : interface(inter) {
            auto conn = sdbus::createSessionBusConnection();
            proxy = sdbus::createProxy(std::move(conn), name, path);
        }

        virtual ~SessionProxy() {
            for(const auto& signal : signals) {
                proxy->unregisterSignalHandler(interface, signal);
            }
        }

        SessionProxy(const SessionProxy &) = delete;
        SessionProxy & operator=(const SessionProxy &) = delete;

        SessionProxy(SessionProxy &&) = default;
        SessionProxy & operator=(SessionProxy &&) = default;

        template <typename Func>
        void registerSignal(const std::string &signalName, Func &&signalHandler) {
            proxy->uponSignal(signalName)
            .onInterface(interface)
            .call(std::move(signalHandler));
            signals.push_front(signalName);
        }

        inline void finishRegistration(void) {
            proxy->finishRegistration();
        }

        void unregisterSignal(const std::string &signalName) {
            proxy->unregisterSignalHandler(interface, signalName);
            signals.remove(signalName);
        }

        template <typename Type>
        Type getProperty(const std::string &prop) const {
            try {
                return proxy->getProperty(prop).onInterface(interface);
            } catch(const sdbus::Error &err) {
                Application::error("%s: failed, sdbus error: %s, msg: %s",
                                   __FUNCTION__, err.getName().c_str(), err.getMessage().c_str());
            }

            return "";
        }

        template <typename... Args>
        sdbus::ObjectPath CallProxyMethod(const std::string &methodName,
                                          Args &&...args) const {
            sdbus::ObjectPath res;

            try {
                proxy->callMethod(methodName)
                .onInterface(interface)
                .withArguments(std::forward<Args>(args)...)
                .storeResultsTo(res);
            } catch(const sdbus::Error &err) {
                Application::error("%s: failed, sdbus error: %s, msg: %s",
                                   __FUNCTION__, err.getName().c_str(), err.getMessage().c_str());
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
                Application::error("%s: failed, sdbus error: %s, msg: %s",
                                   __FUNCTION__, err.getName().c_str(), err.getMessage().c_str());
            }
        }
    };
}

#endif
