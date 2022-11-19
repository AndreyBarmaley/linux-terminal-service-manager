/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_FUSE_
#define _LTSM_FUSE_

#include <map>
#include <list>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>

#include "ltsm_fuse_adaptor.h"
#define LTSM_FUSE2SESSION_VERSION 20221110

#define FUSE_USE_VERSION 34
#include <fuse.h>

namespace LTSM
{
    struct fuse_error : public std::runtime_error
    {
        explicit fuse_error(const std::string & what) : std::runtime_error(what){}
        explicit fuse_error(const char* what) : std::runtime_error(what){}
    };

    struct ReplyBase;

    class FuseApiWrapper
    {
        fuse_operations oper = {0};
        struct fuse* ptr = nullptr;

    public:
        FuseApiWrapper(const std::string &);
        ~FuseApiWrapper();
    };

    class FuseSessionBus : public sdbus::AdaptorInterfaces<Session::FUSE_adaptor>
    {
        std::unique_ptr<FuseApiWrapper> api;
        std::atomic<uint32_t> sid{0};

        std::list<std::unique_ptr<ReplyBase>> replies;
        std::mutex lockrp;

    public:
        FuseSessionBus(sdbus::IConnection &);
        virtual ~FuseSessionBus();

        uint32_t getCookie(void) { return ++sid; }
        std::unique_ptr<ReplyBase> getReply(uint32_t cookie);

        int32_t getVersion(void) override;
        void shutdown(void) override;

        bool mount(const std::string& point) override;
        void umount(void) override;

        void replyGetAttr(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie, const std::map<std::string, int32_t>& stat) override;
        void replyReadDir(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie, const std::vector<std::string>& names) override;
        void replyOpen(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie) override;
        void replyRead(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie, const std::string& data) override;
    };
}

#endif // _LTSM_FUSE_
