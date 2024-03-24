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
#include <stdexcept>
#include <forward_list>

#include "ltsm_fuse.h"
#include "ltsm_fuse_adaptor.h"

namespace LTSM
{
    struct fuse_error : public std::runtime_error
    {
        explicit fuse_error(const std::string & what) : std::runtime_error(what){}
        explicit fuse_error(const char* what) : std::runtime_error(what){}
    };

    struct FuseSession;

    class FuseSessionBus : public sdbus::AdaptorInterfaces<Session::FUSE_adaptor>, public Application
    {
        std::forward_list<std::unique_ptr<FuseSession>> childs;

    public:
        FuseSessionBus(sdbus::IConnection &);
        virtual ~FuseSessionBus();

        int start(void) override;

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;

        bool mountPoint(const std::string& localPoint, const std::string& remotePoint, const std::string& fuseSocket) override;
        void umountPoint(const std::string& point) override;
    };
}

#endif // _LTSM_FUSE_
