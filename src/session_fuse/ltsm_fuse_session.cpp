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

#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <signal.h>

#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <iostream>
#include <exception>
#include <unordered_map>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_application.h"
#include "ltsm_fuse_session.h"

#define FUSE_USE_VERSION 34
#include <fuse.h>
#include <fuse_lowlevel.h>

using namespace std::chrono_literals;

namespace LTSM
{
    const char* argv2[] = { "ltsm_fuse", nullptr };
    fuse_args args = { .argc = 1, .argv = const_cast<char**>(argv2), .allocated = 0 };

    std::unique_ptr<sdbus::IConnection> conn;

    void signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            conn->leaveEventLoop();
        }
    }

    struct DirBuf : std::vector<char>
    {
        std::string root;

        DirBuf()
        {
            reserve(4096);
        }

        DirBuf(const std::string & path) : root(path)
        {
            reserve(4096);
        }

        void addEntry(fuse_req_t req, const std::string & name, const struct stat & st)
        {
            auto need = fuse_add_direntry(req, nullptr, 0, name.c_str(), nullptr, 0);
            auto used = size();
            resize(size() + need);
            auto ptr = std::next(begin(), used);
            fuse_add_direntry(req, std::addressof(*ptr), need, name.c_str(), & st, size());
        }
    };

    class PathStat : protected std::pair<std::string, struct stat>
        {
            public:
            PathStat() = default;
            PathStat(const std::string & str, struct stat && st) : std::pair<std::string, struct stat>(str, st) {}

            virtual ~PathStat() = default;

            std::string localPath(const FuseSession*) const;
            std::string remotePath(const FuseSession*) const;
            std::string joinPath(std::string_view) const;

            const std::string & relativePath(void) const
            {
                return first;
            }

            const struct stat & statRef(void) const
            {
                return second;
            }

            const struct stat* statPtr(void) const
            {
                return std::addressof(second);
            }
        };

    typedef std::pair<ino_t, ino_t> LinkInfo;

    struct FuseSession
    {
        fuse_args args = { .argc = 1, .argv = const_cast<char**>(argv2), .allocated = 0 };
        fuse_lowlevel_ops oper = {};

        std::unordered_map<ino_t, PathStat> inodes;
        std::map<std::string, const struct stat*> pathes;
        std::forward_list<LinkInfo> symlinks;

        std::unique_ptr<SocketStream> sock;
        std::unique_ptr<fuse_session, void(*)(fuse_session*)> ses{ nullptr, fuse_session_destroy };

        DirBuf dirBuf;

        std::thread thloop;
        std::atomic<bool> shutdown{false};

        const std::string localPoint;
        const std::string remotePoint;
        const std::string socketPath;

        uid_t remoteUid = 0;
        gid_t remoteGid = 0;

        FuseSession(const std::string & local, const std::string & remote, const std::string & socket);
        ~FuseSession();

        void exitSession(void);
        bool waitReplyError(void) const;
        void recvStatStruct(struct stat* st);
        void recvShareRootInfo(void);

	bool accessR(const struct stat &) const;
	bool accessW(const struct stat &) const;
	bool accessX(const struct stat &) const;

        const LinkInfo* findLink(fuse_ino_t inode) const;
        const PathStat* findInode(fuse_ino_t inode) const;
        const struct stat* findChildStat(fuse_ino_t parent, const char* child) const;
        DirBuf createDirBuf(fuse_req_t req, const std::string & dir, const struct stat & st) const;
    };

    std::string PathStat::joinPath(std::string_view str) const
    {
        if(str.empty())
        {
            return first;
        }

        if(first.empty())
        {
            return std::string(str.begin(), str.end());
        }

        bool appendSlash = first.back() != '/' && str.front() != '/';

        if(appendSlash)
        {
            return std::string(first).append("/").append(str);
        }

        return std::string(first).append(str);
    }

    std::string PathStat::localPath(const FuseSession* fuse) const
    {
        return fuse ? std::string(fuse->localPoint).append(first) : first;
    }

    std::string PathStat::remotePath(const FuseSession* fuse) const
    {
        return fuse ? std::string(fuse->remotePoint).append(first) : first;
    }

    void ll_init(void* userdata, struct fuse_conn_info* conn)
    {
        if(auto fuse = (FuseSession*) userdata)
        {
            std::error_code fserr;

            while(! fuse->shutdown)
            {
                // wait socket
                if(std::filesystem::is_socket(fuse->socketPath, fserr))
                {
                    int fd = UnixSocket::connect(fuse->socketPath);

                    if(0 < fd)
                    {
                        fuse->sock = std::make_unique<SocketStream>(fd);
                        break;
                    }
                }

                std::this_thread::sleep_for(100ms);
            }

            if(fuse->shutdown)
            {
                return;
            }

            if(! fuse->sock)
            {
                Application::error("%s: %s failed", __FUNCTION__, "socket");
                fuse->exitSession();
                return;
            }

            // send inititialize packet
            fuse->sock->sendIntLE16(FuseOp::Init);
            // <VER16> - proto version
            // <LEN16><MOUNTPOINT> - mount point
            fuse->sock->sendIntLE16(1);
            fuse->sock->sendIntLE16(fuse->remotePoint.size());
            fuse->sock->sendString(fuse->remotePoint);
            fuse->sock->sendFlush();

            if(fuse->waitReplyError())
            {
                Application::error("%s: %s failed", __FUNCTION__, "wait");
                fuse->exitSession();
                return;
            }

            // reply format:
            // <CMD16> - fuse cmd
            // <ERR32> - errno
            auto cmd = fuse->sock->recvIntLE16();
            auto err = fuse->sock->recvIntLE32();

            if(cmd != FuseOp::Init)
            {
                Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
                fuse->exitSession();
                return;
            }

            if(err)
            {
                Application::error("%s: recv error: %" PRId32, __FUNCTION__, err);
                fuse->exitSession();
                return;
            }

            // <UID16> - proto
            auto protoVer = fuse->sock->recvIntLE16();
            // <UID32> - remote uid
            fuse->remoteUid = fuse->sock->recvIntLE32();
            // <GID32> - remote gid
            fuse->remoteGid = fuse->sock->recvIntLE32();
            fuse->recvShareRootInfo();
        }
    }

    void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char* path)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64 ", path: `%s'", __FUNCTION__, parent, path);
        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto st = fuse->findChildStat(parent, path);

        if(! st)
        {
            fuse_reply_err(req, ENOENT);
            return;
        }

        // <STAT> - stat struct
        fuse_entry_param entry = {};
        entry.attr = *st;
        entry.attr_timeout = 1.0;
        entry.entry_timeout = 1.0;
        entry.ino = st->st_ino;
        fuse_reply_entry(req, & entry);
    }

    void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64, __FUNCTION__, ino);

        if(fi)
        {
            Application::debug(DebugType::Fuse, "%s: file info - flags: 0x%" PRIx32 ", fh: %" PRIu64, __FUNCTION__, fi->flags, fi->fh);
        }

        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            Application::error("%s: %s filed", __FUNCTION__, "fuse");
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            Application::error("%s: %s filed", __FUNCTION__, "sock");
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            Application::error("%s: %s filed", __FUNCTION__, "inode");
            fuse_reply_err(req, ENOENT);
            return;
        }

        fuse_reply_attr(req, pathStat->statPtr(), 1.0);
    }

    void ll_readlink(fuse_req_t req, fuse_ino_t ino)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64, __FUNCTION__, ino);
        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            Application::error("%s: %s filed", __FUNCTION__, "fuse");
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            Application::error("%s: %s filed", __FUNCTION__, "sock");
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            Application::error("%s: %s filed", __FUNCTION__, "inode");
            fuse_reply_err(req, ENOENT);
            return;
        }

        if(S_ISLNK(pathStat->statRef().st_mode))
        {
            if(auto pair = fuse->findLink(ino))
            {
                if(auto pathStat2 = fuse->findInode(pair->second))
                {
                    auto path = pathStat2->localPath(fuse);
                    fuse_reply_readlink(req, path.c_str());
                    return;
                }
            }

            fuse_reply_err(req, ENOENT);
            return;
        }

        fuse_reply_err(req, EINVAL);
    }

    void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t off, struct fuse_file_info* fi)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64 ", max size: %" PRIu64 ", offset: %" PRIu64, __FUNCTION__, ino, maxsize, off);

        if(fi)
        {
            Application::debug(DebugType::Fuse, "%s: file info - flags: 0x%" PRIx32 ", fh: %" PRIu64, __FUNCTION__, fi->flags, fi->fh);
        }

        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            fuse_reply_err(req, ENOENT);
            return;
        }

        if(! S_ISDIR(pathStat->statRef().st_mode))
        {
            fuse_reply_err(req, ENOTDIR);
            return;
        }

        // relative path
        auto & path = pathStat->relativePath();

        if(fuse->dirBuf.root != path)
        {
            fuse->dirBuf = fuse->createDirBuf(req, path, pathStat->statRef());
        }

        if((size_t) off < fuse->dirBuf.size())
        {
            fuse_reply_buf(req, fuse->dirBuf.data() + off, std::min(fuse->dirBuf.size() - off, maxsize));
        }
        else
        {
            fuse_reply_buf(req, nullptr, 0);
        }
    }

    void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64, __FUNCTION__, ino);

        if(fi)
        {
            Application::debug(DebugType::Fuse, "%s: file info - flags: 0x%" PRIx32 ", fh: %" PRIu64, __FUNCTION__, fi->flags, fi->fh);
        }
        else
        {
            Application::error("%s: %s failed", __FUNCTION__, "fuse_file_info");
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            fuse_reply_err(req, ENOENT);
            return;
        }

        if(S_ISDIR(pathStat->statRef().st_mode))
        {
            fuse_reply_err(req, EISDIR);
            return;
        }

        //
        fuse->sock->sendIntLE16(FuseOp::Open);
        auto & path = pathStat->relativePath();
        // <FLAG32> - open flags
        // <LEN16><PATH> - fuse path
        fuse->sock->sendIntLE32(fi->flags);
        fuse->sock->sendIntLE16(std::min(size_t(UINT16_MAX), path.size()));
        fuse->sock->sendString(path);
        fuse->sock->sendFlush();

        // wait reply
        if(fuse->waitReplyError())
        {
            Application::error("%s: %s failed", __FUNCTION__, "wait");
            fuse_reply_err(req, EFAULT);
            return;
        }

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno
        auto cmd = fuse->sock->recvIntLE16();
        auto err = fuse->sock->recvIntLE32();

        if(cmd != FuseOp::Open)
        {
            Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(err)
        {
            Application::error("%s: recv error: %" PRId32, __FUNCTION__, err);
            fuse_reply_err(req, err);
            return;
        }

        // <FDH32> - fd handle
        fi->fh = fuse->sock->recvIntLE32();
        fuse_reply_open(req, fi);
    }

    void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64, __FUNCTION__, ino);

        if(fi)
        {
            Application::debug(DebugType::Fuse, "%s: file info - flags: 0x%" PRIx32 ", fh: %" PRIu64, __FUNCTION__, fi->flags, fi->fh);
        }
        else
        {
            Application::error("%s: %s failed", __FUNCTION__, "fuse_file_info");
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            fuse_reply_err(req, EBADF);
            return;
        }

        if(S_ISDIR(pathStat->statRef().st_mode))
        {
            fuse_reply_err(req, EBADF);
            return;
        }

        //
        fuse->sock->sendIntLE16(FuseOp::Release);
        // <FDH32> - fd handle
        fuse->sock->sendIntLE32(fi->fh);
        fuse->sock->sendFlush();

        // wait reply
        if(fuse->waitReplyError())
        {
            Application::error("%s: %s failed", __FUNCTION__, "wait");
            fuse_reply_err(req, EFAULT);
            return;
        }

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno
        auto cmd = fuse->sock->recvIntLE16();
        auto err = fuse->sock->recvIntLE32();

        if(cmd != FuseOp::Release)
        {
            Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(err)
        {
            Application::error("%s: recv error: %" PRId32, __FUNCTION__, err);
            fuse_reply_err(req, err);
            return;
        }

        fuse_reply_err(req, 0);
    }

    void ll_read(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t offset, struct fuse_file_info* fi)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64, __FUNCTION__, ino);

        if(fi)
        {
            Application::debug(DebugType::Fuse, "%s: file info - flags: 0x%" PRIx32 ", fh: %" PRIu64, __FUNCTION__, fi->flags, fi->fh);
        }
        else
        {
            Application::error("%s: %s failed", __FUNCTION__, "fuse_file_info");
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            fuse_reply_err(req, EBADF);
            return;
        }

        if(S_ISDIR(pathStat->statRef().st_mode))
        {
            fuse_reply_err(req, EISDIR);
            return;
        }

        //
        fuse->sock->sendIntLE16(FuseOp::Read);
        const size_t blockmax = 48 * 1024;
        // <FDH32> - fd handle
        // <SIZE16> - blocksz
        // <OFF64> - offset
        fuse->sock->sendIntLE32(fi->fh);
        fuse->sock->sendIntLE16(std::min(maxsize, blockmax));
        fuse->sock->sendIntLE64(offset);
        fuse->sock->sendFlush();

        // wait reply
        if(fuse->waitReplyError())
        {
            Application::error("%s: %s failed", __FUNCTION__, "wait");
            fuse_reply_err(req, EFAULT);
            return;
        }

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno ot read sz
        auto cmd = fuse->sock->recvIntLE16();
        int err = fuse->sock->recvIntLE32();

        if(cmd != FuseOp::Read)
        {
            Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(err)
        {
            Application::error("%s: recv error: %" PRId32, __FUNCTION__, err);
            fuse_reply_err(req, err);
            return;
        }

        // <LEN16><DATA> - raw data
        const size_t len = fuse->sock->recvIntLE16();

        if(len == 0)
        {
            fuse_reply_buf(req, nullptr, 0);
            return;
        }

        auto buf = fuse->sock->recvData(len);
        fuse_reply_buf(req, (const char*) buf.data(), buf.size());
    }

    void ll_access(fuse_req_t req, fuse_ino_t ino, int mask)
    {
        Application::debug(DebugType::Fuse, "%s: ino: %" PRIu64 ", mask: 0x%" PRIx32, __FUNCTION__, ino, mask);
        auto fuse = (FuseSession*) fuse_req_userdata(req);

        if(! fuse)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(! fuse->sock)
        {
            fuse_reply_err(req, EFAULT);
            return;
        }

        if(ino == 1)
        {
            fuse_reply_err(req, 0);
            return;
        }

        auto pathStat = fuse->findInode(ino);

        if(! pathStat)
        {
            fuse_reply_err(req, ENOENT);
            return;
        }

        if(mask == F_OK)
        {
            fuse_reply_err(req, 0);
            return;
        }

        if(mask & (R_OK | W_OK | X_OK))
        {
            auto & st = pathStat->statRef();

            if((mask & R_OK) && ! fuse->accessR(st))
            {
                fuse_reply_err(req, EACCES);
                return;
            }

            if((mask & W_OK) && ! fuse->accessW(st))
            {
                fuse_reply_err(req, EACCES);
                return;
            }

            if((mask & X_OK) && ! fuse->accessX(st))
            {
                fuse_reply_err(req, EACCES);
                return;
            }

            fuse_reply_err(req, 0);
            return;
        }

        fuse_reply_err(req, EINVAL);
    }

    // FuseSession
    FuseSession::FuseSession(const std::string & local, const std::string & remote,
                             const std::string & socket) : localPoint(local), remotePoint(remote), socketPath(socket)
    {
        // added parent inode: 1
        struct stat st = {};

        if(0 > ::stat(local.c_str(), & st))
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "stat", strerror(errno), errno,
                               local.c_str());
            throw fuse_error(NS_FuncName);
        }

        Application::debug(DebugType::Fuse, "%s: added ino: %" PRIu64 ", path: `%s'", __FUNCTION__, 1, local.c_str());
        auto pair = inodes.emplace(1, PathStat{"/", std::move(st)});
        pathes.emplace("/", pair.first->second.statPtr());
        // fuse init
        oper.init = ll_init;
        oper.lookup = ll_lookup;
        oper.getattr = ll_getattr;
        oper.readdir = ll_readdir;
        oper.open = ll_open;
        oper.release = ll_release;
        oper.read = ll_read;
        oper.access = ll_access;
        oper.readlink = ll_readlink;
        ses.reset(fuse_session_new(& args, & oper, sizeof(oper), this));

        if(! ses)
        {
            Application::error("%s: %s failed", __FUNCTION__, "fuse_session_new");
            throw fuse_error(NS_FuncName);
        }

        fuse_set_signal_handlers(ses.get());

        if(0 > fuse_session_mount(ses.get(), local.c_str()))
        {
            Application::error("%s: %s failed, local point: `%s'", __FUNCTION__, "fuse_session_mount", local.c_str());
            throw fuse_error(NS_FuncName);
        }

        thloop = std::thread([ses = ses.get()]()
        {
            fuse_session_loop(ses);
        });
    }

    FuseSession::~FuseSession()
    {
        shutdown = true;

        if(ses)
        {
            if(! fuse_session_exited(ses.get()))
            {
                fuse_session_unmount(ses.get());
                fuse_remove_signal_handlers(ses.get());
                fuse_session_exit(ses.get());
            }

            ses.reset();
        }

        if(thloop.joinable())
        {
            thloop.join();
        }

        fuse_opt_free_args(& args);
    }

    bool FuseSession::accessR(const struct stat & st) const
    {
    	return (S_IROTH & st.st_mode) ||
            ((S_IRGRP & st.st_mode) && st.st_gid == remoteGid) ||
            ((S_IRUSR & st.st_mode) && st.st_uid == remoteUid);
    }

    bool FuseSession::accessW(const struct stat & st) const
    {
	return (S_IWOTH & st.st_mode) ||
            ((S_IWGRP & st.st_mode) && st.st_gid == remoteGid) ||
            ((S_IWUSR & st.st_mode) && st.st_uid == remoteUid);
    }

    bool FuseSession::accessX(const struct stat & st) const
    {
        return (S_IXOTH & st.st_mode) ||
            ((S_IXGRP & st.st_mode) && st.st_gid == remoteGid) ||
            ((S_IXUSR & st.st_mode) && st.st_uid == remoteUid);
    }

    void FuseSession::exitSession(void)
    {
        if(! fuse_session_exited(ses.get()))
        {
            fuse_session_unmount(ses.get());
            fuse_session_exit(ses.get());
        }
    }

    bool FuseSession::waitReplyError(void) const
    {
        // wait reply
        while(! shutdown)
        {
            if(! sock)
            {
                break;
            }

            if(sock->hasInput() && 6 <= sock->hasData())
            {
                break;
            }

            std::this_thread::sleep_for(5ms);
        }

        return shutdown;
    }

    void FuseSession::recvStatStruct(struct stat* st)
    {
        // <STAT> - stat struct
        st->st_dev = sock->recvIntLE64();
        st->st_ino = sock->recvIntLE64();
        st->st_mode = sock->recvIntLE32();
        st->st_nlink = sock->recvIntLE64();
        st->st_uid = sock->recvIntLE32();
        st->st_gid = sock->recvIntLE32();
        st->st_rdev = sock->recvIntLE64();
        st->st_size = sock->recvIntLE64();
        st->st_blksize = sock->recvIntLE64();
        st->st_blocks = sock->recvIntLE64();
        st->st_atime = sock->recvIntLE64();
        st->st_mtime = sock->recvIntLE64();
        st->st_ctime = sock->recvIntLE64();
        // force uid/gid
        st->st_uid = remoteUid != st->st_uid ? 0 : getuid();
        st->st_gid = remoteGid != st->st_gid ? 0 : getgid();
    }

    void FuseSession::recvShareRootInfo(void)
    {
        // inodes
        uint32_t count = sock->recvIntLE32();
        inodes.reserve(count);

        while(count--)
        {
            auto len = sock->recvIntLE16();
            // remote path
            auto path = sock->recvString(len);
            struct stat st;
            recvStatStruct(& st);
            auto ino = st.st_ino;

            if(ino != 1 &&
                    path.substr(0, remotePoint.size()) == remotePoint)
            {
                path = path.substr(remotePoint.size());
                Application::debug(DebugType::Fuse, "%s: added ino: %" PRIu64 ", path: `%s'", __FUNCTION__, ino, path.c_str());
                // added relative path
                auto pair = inodes.emplace(ino, PathStat{path, std::move(st)});
                pathes.emplace(std::move(path), pair.first->second.statPtr());
            }
        }

        // symlinks
        count = sock->recvIntLE32();

        while(count--)
        {
            auto ino1 = sock->recvIntLE64();
            auto ino2 = sock->recvIntLE64();
            symlinks.emplace_front(std::make_pair(ino1, ino2));
        }
    }

    const LinkInfo* FuseSession::findLink(fuse_ino_t inode) const
    {
        auto it = std::find_if(symlinks.begin(), symlinks.end(), [&](auto & st)
        {
            return st.first == inode;
        });

        return it != symlinks.end() ? std::addressof(*it) : nullptr;
    }

    const PathStat* FuseSession::findInode(fuse_ino_t inode) const
    {
        auto iti = inodes.find(inode);

        if(iti == inodes.end())
        {
            return nullptr;
        }

        return std::addressof(iti->second);
    }

    const struct stat* FuseSession::findChildStat(fuse_ino_t parent, const char* child) const
    {
        auto iti = inodes.find(parent);

        if(iti == inodes.end())
        {
            return nullptr;
        }

        auto path = iti->second.joinPath(child);

        if(auto itp = pathes.find(path); itp != pathes.end())
        {
            return itp->second;
        }

        Application::warning("%s: not found, ino: %" PRIu64 ", path: `%s'", __FUNCTION__, parent, path.c_str());
        return nullptr;
    }

    DirBuf FuseSession::createDirBuf(fuse_req_t req, const std::string & dir, const struct stat & st) const
    {
        DirBuf dirBuf(dir);
        dirBuf.addEntry(req, ".", st);
        dirBuf.addEntry(req, "..", st);
        auto it = pathes.upper_bound(dir);

        while(it != pathes.end())
        {
            auto path = std::filesystem::path(it->first);

            if(path.parent_path() == dir)
            {
                dirBuf.addEntry(req, path.filename(), *it->second);
            }

            it = std::next(it);
        }

        return dirBuf;
    }

    /// FuseSessionBus
    FuseSessionBus::FuseSessionBus(sdbus::IConnection & conn, bool debug)
#ifdef SDBUS_2_0_API
        : AdaptorInterfaces(conn, sdbus::ObjectPath{dbus_session_fuse_path}),
#else
        : AdaptorInterfaces(conn, dbus_session_fuse_path),
#endif
         Application("ltsm_fuse2session")
    {
        Application::setDebug(DebugTarget::Syslog, debug ? DebugLevel::Debug : DebugLevel::Info);
        registerAdaptor();
    }

    FuseSessionBus::~FuseSessionBus()
    {
        unregisterAdaptor();
    }

    int FuseSessionBus::start(void)
    {
        Application::info("started, uid: %d, pid: %d, version: %d", getuid(), getpid(), LTSM_FUSE2SESSION_VERSION);

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);

        conn->enterEventLoop();

        for(auto & st : childs)
        {
            st->shutdown = true;
        }

        return EXIT_SUCCESS;
    }

    int32_t FuseSessionBus::getVersion(void)
    {
        Application::debug(DebugType::Fuse, "%s", __FUNCTION__);
        return LTSM_FUSE2SESSION_VERSION;
    }

    void FuseSessionBus::serviceShutdown(void)
    {
        Application::debug(DebugType::Fuse, "%s, pid: %d", __FUNCTION__, getpid());
        conn->leaveEventLoop();
    }

    bool FuseSessionBus::mountPoint(const std::string & localPoint, const std::string & remotePoint,
                                    const std::string & fuseSocket)
    {
        Application::info("%s: local point: `%s', remote point: `%s', fuse socket: `%s'", __FUNCTION__, localPoint.c_str(),
                          remotePoint.c_str(), fuseSocket.c_str());

        if(std::any_of(childs.begin(), childs.end(), [&](auto & ptr) { return ptr->localPoint == localPoint; }))
        {
            Application::error("%s: point busy, point: `%s'", __FUNCTION__, localPoint.c_str());
            return false;
        }

        std::unique_ptr<FuseSession> ptr;

        try
        {
            ptr = std::make_unique<FuseSession>(localPoint, remotePoint, fuseSocket);
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            return false;
        }

        childs.emplace_front(std::move(ptr));
        return true;
    }

    void FuseSessionBus::setDebug(const std::string & level)
    {
        setDebugLevel(level);
    }

    void FuseSessionBus::umountPoint(const std::string & localPoint)
    {
        LTSM::Application::info("%s: local point: `%s'", __FUNCTION__, localPoint.c_str());
        childs.remove_if([&](auto & ptr)
        {
            return ptr->localPoint == localPoint;
        });
    }
}

int main(int argc, char** argv)
{
    bool debug = true;

    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
        {
            std::cout << "usage: " << argv[0] << std::endl;
            return EXIT_SUCCESS;
        }
        else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v"))
        {
            std::cout << "version: " << LTSM_FUSE2SESSION_VERSION << std::endl;
            return EXIT_SUCCESS;
        }
        else if(0 == std::strcmp(argv[it], "--debug") || 0 == std::strcmp(argv[it], "-d"))
        {
            debug = true;
        }
    }

    if(0 == getuid())
    {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    try
    {
#ifdef SDBUS_2_0_API
        LTSM::conn = sdbus::createSessionBusConnection(sdbus::ServiceName{LTSM::dbus_session_fuse_name});
#else
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_fuse_name);
#endif

        if(! LTSM::conn)
        {
            LTSM::Application::error("dbus connection failed, uid: %d", getuid());
            return EXIT_FAILURE;
        }

        LTSM::FuseSessionBus fuseSession(*LTSM::conn, debug);
        return fuseSession.start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    }
    catch(const std::exception & err)
    {
        LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
    }

    return EXIT_FAILURE;
}
