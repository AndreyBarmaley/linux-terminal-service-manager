/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_FUSE_SESSION_
#define _LTSM_FUSE_SESSION_

#include <string>
#include <memory>
#include <forward_list>

#include "ltsm_fuse.h"
#include "ltsm_async_sdbus.h"
#include "ltsm_fuse_adaptor.h"
#include "ltsm_async_socket.h"

namespace LTSM {
    struct FuseSession;

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;
    using FuseSessionPtr = std::unique_ptr<FuseSession>;

    class FuseSessionBus : public ApplicationLog, public sdbus::AdaptorInterfaces<Session::Fuse_adaptor>, protected SDBus::AsioCoroConnector {
        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;
        boost::asio::cancellation_signal connect_cancel_;
        boost::asio::strand<boost::asio::any_io_executor> clients_strand_;

        std::forward_list<FuseSessionPtr> childs_;

      protected:
        boost::asio::awaitable<void> signalsHandler(void);
        boost::asio::awaitable<void> sdbusHandler(void);
        void stop(void) noexcept;

      public:
        FuseSessionBus(DBusConnectionPtr, bool debug = false);
        virtual ~FuseSessionBus();

        int start(void);

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        bool mountPoint(const std::string & localPoint, const std::string & remotePoint,
                        const std::string & fuseSocket) override;
        void umountPoint(const std::string & point) override;
    };
}

#endif // _LTSM_FUSE_
