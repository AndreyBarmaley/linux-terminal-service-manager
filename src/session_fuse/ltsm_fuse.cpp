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

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>
#include <exception>

#include "ltsm_fuse.h"
#include "ltsm_tools.h"
#include "ltsm_streambuf.h"
#include "ltsm_application.h"

#include <fuse_lowlevel.h>

using namespace std::chrono_literals;

namespace LTSM
{
    struct ReplyBase
    {
        std::string path;
        uint32_t cookie = 0;
        int32_t errno2 = 0;
        bool error = false;

        ReplyBase() = default;
        virtual ~ReplyBase() = default;

        ReplyBase(bool err, int32_t eno2, const std::string & str, uint32_t sid) : path(str), cookie(sid), errno2(eno2), error(err) {}
    };

    struct ReplyRead : ReplyBase
    {
        std::string data;

        ReplyRead(bool err, int32_t eno2, const std::string & str, uint32_t sid, const std::string & v) : ReplyBase(err, eno2, str, sid), data(v)
        {
        }
    };

    struct ReplyOpen : ReplyBase
    {
        ReplyOpen(bool err, int32_t eno2, const std::string & str, uint32_t sid) : ReplyBase(err, eno2, str, sid)
        {
        }
    };

    struct ReplyReadDir : ReplyBase
    {
        std::vector<std::string> names;

        ReplyReadDir(bool err, int32_t eno2, const std::string & str, uint32_t sid, const std::vector<std::string> & v) : ReplyBase(err, eno2, str, sid), names(v)
        {
        }
    };

    struct ReplyGetAttr : ReplyBase
    {
        struct stat st = {0};

        ReplyGetAttr(bool err, int32_t eno2, const std::string & str, uint32_t sid, const std::map<std::string, int32_t> & v) : ReplyBase(err, eno2, str, sid)
        {
            for(auto & [key, val ]: v)
            {
                if(key == "st_dev")
                    st.st_dev = val;
                else
                if(key == "st_ino")
                    st.st_ino = val;
                else
                if(key == "st_mode")
                    st.st_mode = val;
                else
                if(key == "st_nlink")
                    st.st_nlink = val;
                else
                if(key == "st_uid")
                    st.st_uid = val;
                else
                if(key == "st_gid")
                    st.st_gid = val;
                else
                if(key == "st_rdev")
                    st.st_rdev = val;
                else
                if(key == "st_size")
                    st.st_size = val;
                else
                if(key == "st_blksize")
                    st.st_blksize = val;
                else
                if(key == "st_blocks")
                    st.st_blocks = val;
                else
                if(key == "st_atime")
                    st.st_atime = val;
                else
                if(key == "st_mtime")
                    st.st_mtime = val;
                else
                if(key == "st_ctime")
                    st.st_ctime = val;
            }
        }
    };

    const size_t blocksz = 4096 * 4;
    const char* argv2[] = { "ltsm_fuse", nullptr };
    fuse_args args = { .argc = 1, .argv = const_cast<char**>(argv2), .allocated = 0 };

    std::unique_ptr<sdbus::IConnection> conn;
    std::unique_ptr<LTSM::FuseSessionBus> session;

    void* ll_init(struct fuse_conn_info* fcon, struct fuse_config* cfg)
    {
        return nullptr;
    }

    int ll_getattr(const char* path, struct stat* st, struct fuse_file_info* fi)
    {
        if(session)
        {
            auto cookie = session->getCookie();
            session->emitRequestGetAttr(path, cookie);

            std::unique_ptr<ReplyBase> reply;

            // wait reply
            while(session)
            {
                if(reply = session->getReply(cookie); reply)
                    break;

                std::this_thread::sleep_for(100ms);
            }

            if(reply)
            {
                if(reply->error)
                    return -reply->errno2;

                auto ptr = static_cast<ReplyGetAttr*>(reply.get());

                //st->st_dev = ptr->st.st_dev;
                //st->st_ino = ptr->st.st_ino;
                // st->st_mode = ptr->st.st_mode;
                st->st_nlink = ptr->st.st_nlink;
                //st->st_uid = // getuid();
                //st->st_gid = // getgid();
                //st->st_rdev = ptr->st.st_rdev;
                st->st_size = ptr->st.st_size;
                //st->st_blksize = ptr->st.st_blksize;
                st->st_blocks = ptr->st.st_blocks;
                st->st_atime = ptr->st.st_atime;
                st->st_mtime = ptr->st.st_mtime;
                st->st_ctime = ptr->st.st_ctime;

                st->st_mode = 1 < st->st_nlink ? S_IFDIR | 0555 : S_IFREG | 0444;

                return 0;
            }
        }

        return -EFAULT;
    }

    int ll_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags)
    {
        if(session)
        {
            auto cookie = session->getCookie();
            session->emitRequestReadDir(path, cookie);

            std::unique_ptr<ReplyBase> reply;

            // wait reply
            while(session)
            {
                if(reply = session->getReply(cookie); reply)
                    break;

                std::this_thread::sleep_for(100ms);
            }

            if(reply)
            {
                if(reply->error)
                    return -reply->errno2;

                filler(buf, ".", nullptr, 0, fuse_fill_dir_flags(0));
                filler(buf, "..", nullptr, 0, fuse_fill_dir_flags(0));

                auto ptr = static_cast<ReplyReadDir*>(reply.get());

                for(auto & name : ptr->names)
                    filler(buf, name.c_str(), nullptr, 0, fuse_fill_dir_flags(0));

                return 0;
            }
        }

        return -EFAULT;
    }

    int ll_open(const char* path, struct fuse_file_info* fi)
    {
        if(session && fi)
        {
            auto cookie = session->getCookie();
            session->emitRequestOpen(path, cookie, fi->flags);

            std::unique_ptr<ReplyBase> reply;

            // wait reply
            while(session)
            {
                if(reply = session->getReply(cookie); reply)
                    break;

                std::this_thread::sleep_for(100ms);
            }

            if(reply)
            {
                return reply->error ? -reply->errno2 : 0;
            }
        }

        return -EFAULT;
    }

    int ll_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
    {
        if(session)
        {
            auto cookie = session->getCookie();
            session->emitRequestRead(path, cookie, std::min(size, blocksz), offset);

            std::unique_ptr<ReplyBase> reply;

            // wait reply
            while(session)
            {
                if(reply = session->getReply(cookie); reply)
                    break;

                std::this_thread::sleep_for(100ms);
            }

            if(reply)
            {
                if(reply->error)
                    return -reply->errno2;

                auto ptr = static_cast<ReplyRead*>(reply.get());
                auto raw = Tools::convertJsonString2Binary(ptr->data);

                if(size < raw.size())
                {
                    Application::error("%s: out of range, raw size: %d, fuse size: %d", __FUNCTION__, raw.size(), size);
                    throw fuse_error(NS_FuncName);
                }

                std::copy_n(raw.data(), raw.size(), buf);
                return raw.size();
            }
        }

        return -EFAULT;
    }

    FuseApiWrapper::FuseApiWrapper(const std::string & folder)
    {
        oper.init    = ll_init;
        oper.getattr = ll_getattr;
        oper.readdir = ll_readdir;
        oper.open    = ll_open;
        oper.read    = ll_read;

        ptr = fuse_new(& args, & oper, sizeof(oper), this);
        if(! ptr)
        {
            Application::error("%s: %s: failed", __FUNCTION__, "fuse_new");
            throw fuse_error(NS_FuncName);
        }

        struct fuse_session *se = fuse_get_session(ptr);

        if(se &&
            0 == fuse_mount(ptr, folder.c_str()) && 
            0 == fuse_set_signal_handlers(se))
        {
            std::thread([fuse = ptr]
            {
                fuse_loop(fuse);
            }).detach();
        }
        else
        {
            Application::error("%s: %s: failed, path: `%s'", __FUNCTION__, "fuse_mount", folder.c_str());
            throw fuse_error(NS_FuncName);
        }
    }

    FuseApiWrapper::~FuseApiWrapper()
    {
        if(ptr)
        {
            if(auto se = fuse_get_session(ptr))
            {
                fuse_session_exit(se);
                fuse_remove_signal_handlers(se);
            }

            fuse_unmount(ptr);
            fuse_destroy(ptr);
        }
    }

    FuseSessionBus::FuseSessionBus(sdbus::IConnection & conn)
        : AdaptorInterfaces(conn, LTSM::dbus_session_fuse_path)
    {
        registerAdaptor();
    }

    FuseSessionBus::~FuseSessionBus()
    {
        unregisterAdaptor();
    }

    std::unique_ptr<ReplyBase>
        FuseSessionBus::getReply(uint32_t cookie)
    {
        std::scoped_lock{ lockrp };
        auto it = std::find_if(replies.begin(), replies.end(),
                [=](auto & ptr){ return ptr->cookie == cookie; });

        if(it != replies.end())
        {
            auto res = std::move(*it);
            replies.erase(it);
            return res;
        }

        return nullptr;
    }

    int32_t FuseSessionBus::getVersion(void)
    {
        LTSM::Application::debug("%s", __FUNCTION__);

        return LTSM_FUSE2SESSION_VERSION;
    }

    void FuseSessionBus::shutdown(void)
    {
        LTSM::Application::debug("%s", __FUNCTION__);

        api.reset();

        if(conn)
            conn->leaveEventLoop();
    }

    bool FuseSessionBus::mount(const std::string& point)
    {
        LTSM::Application::info("%s: point: `%s'", __FUNCTION__, point.c_str());

        try
        {
            api = std::make_unique<FuseApiWrapper>(point);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        return !! api;
    }

    void FuseSessionBus::umount(void)
    {
        LTSM::Application::info("%s", __FUNCTION__);

        api.reset();
    }

    void FuseSessionBus::replyGetAttr(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie, const std::map<std::string, int32_t>& stat)
    {
        LTSM::Application::debug("%s: path: `%s', cookie: %d, errno: %d", __FUNCTION__, path.c_str(), cookie, errno2);

        std::scoped_lock{ lockrp };
        replies.emplace_back(std::make_unique<ReplyGetAttr>(error, errno2, path, cookie, stat));
    }

    void FuseSessionBus::replyReadDir(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie, const std::vector<std::string>& names)
    {
        LTSM::Application::debug("%s: path: `%s', cookie: %d, errno: %d", __FUNCTION__, path.c_str(), cookie, errno2);

        std::scoped_lock{ lockrp };
        replies.emplace_back(std::make_unique<ReplyReadDir>(error, errno2, path, cookie, names));
    }

    void FuseSessionBus::replyOpen(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie)
    {
        LTSM::Application::debug("%s: path: `%s', cookie: %d, errno: %d", __FUNCTION__, path.c_str(), cookie, errno2);

        std::scoped_lock{ lockrp };
        replies.emplace_back(std::make_unique<ReplyOpen>(error, errno2, path, cookie));
    }

    void FuseSessionBus::replyRead(const bool& error, const int32_t& errno2, const std::string& path, const uint32_t& cookie, const std::string& data)
    {
        LTSM::Application::debug("%s: path: `%s', cookie: %d, errno: %d", __FUNCTION__, path.c_str(), cookie, errno2);

        std::scoped_lock{ lockrp };
        replies.emplace_back(std::make_unique<ReplyRead>(error, errno2, path, cookie, data));
    }
}

int main(int argc, char** argv)
{
    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
        {
            std::cout << "usage: " << argv[0] << std::endl;
            return EXIT_SUCCESS;
        }
        else
        if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v"))
        {
            std::cout << "version: " << LTSM_FUSE2SESSION_VERSION << std::endl;
            return EXIT_SUCCESS;
        }
    }

    if(0 == getuid())
    {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    LTSM::Application::setDebugLevel(LTSM::DebugLevel::SyslogInfo);

    try
    {
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_fuse_name);
        if(! LTSM::conn)
        {
            LTSM::Application::error("dbus connection failed, uid: %d", getuid());
            return EXIT_FAILURE;
        }

        LTSM::session = std::make_unique<LTSM::FuseSessionBus>(*LTSM::conn);
        LTSM::Application::info("started, uid: %d, pid: %d, version: %d", getuid(), getpid(), LTSM_FUSE2SESSION_VERSION);

        LTSM::conn->enterEventLoop();
        LTSM::session.reset();
        LTSM::conn.reset();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
    }

    return EXIT_SUCCESS;
}
