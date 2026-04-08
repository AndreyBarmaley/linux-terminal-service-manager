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

#include <csignal>

#include <map>
#include <mutex>
#include <chrono>
#include <atomic>
#include <thread>
#include <cstring>
#include <iostream>
#include <exception>
#include <unordered_map>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_application.h"
#include "ltsm_fuse_session.h"

#define FUSE_USE_VERSION 34
#include <fuse.h>
#include <fuse_lowlevel.h>

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    const char* argv2[] = { "ltsm_fuse", nullptr };
    fuse_args args = { .argc = 1, .argv = const_cast<char**>(argv2), .allocated = 0 };

    using LinkInfo = std::pair<ino_t, ino_t>;
    using StStat = struct stat;

    struct DirBuf : std::vector<char> {
        std::string root;

        DirBuf() {
            reserve(4096);
        }

        DirBuf(const std::string & path) : root(path) {
            reserve(4096);
        }

        void addEntry(fuse_req_t req, const std::string & name, const StStat & st) {
            auto need = fuse_add_direntry(req, nullptr, 0, name.c_str(), nullptr, 0);
            auto used = size();
            resize(size() + need);
            auto ptr = std::next(begin(), used);
            fuse_add_direntry(req, std::addressof(*ptr), need, name.c_str(), & st, size());
        }
    };

    struct PathStat : protected std::pair<std::string, StStat> {
      public:
        PathStat() = default;
        PathStat(const std::string & str, StStat && st) : std::pair<std::string, StStat>(str, st) {}

        virtual ~PathStat() = default;

        //        std::string localPath(const FuseSession*) const;
        //        std::string remotePath(const FuseSession*) const;

        std::string joinPath(std::string_view str) const {
            if(str.empty()) {
                return first;
            }

            if(first.empty()) {
                return std::string(str.begin(), str.end());
            }

            bool appendSlash = first.back() != '/' && str.front() != '/';

            if(appendSlash) {
                return std::string(first).append("/").append(str);
            }

            return std::string(first).append(str);
        }

        const std::string & relativePath(void) const {
            return first;
        }

        const StStat & statRef(void) const {
            return second;
        }

        const StStat* statPtr(void) const {
            return std::addressof(second);
        }
    };

    class FuseSession : protected AsyncSocket<boost::asio::local::stream_protocol::socket> {
        boost::asio::strand<boost::asio::any_io_executor> fuse_strand_;

        fuse_args args = { .argc = 1, .argv = const_cast<char**>(argv2), .allocated = 0 };
        fuse_lowlevel_ops oper = {};

        std::unordered_map<ino_t, PathStat> inodes;
        std::map<std::string, const StStat*> pathes;
        std::forward_list<LinkInfo> symlinks;
        std::unique_ptr<fuse_session, void(*)(fuse_session*)> ses{ nullptr, fuse_session_destroy };

        DirBuf dirBuf;

        std::thread thloop;

        const std::string localPoint;
        const std::string remotePoint;

        uid_t remoteUid = 0;
        gid_t remoteGid = 0;

      protected:
        void stopSession(void) noexcept;

        bool accessR(const StStat &) const;
        bool accessW(const StStat &) const;
        bool accessX(const StStat &) const;

        const LinkInfo* findLink(fuse_ino_t inode) const;
        const PathStat* findInode(fuse_ino_t inode) const;
        const StStat* findChildStat(fuse_ino_t parent, const char* child) const;
        DirBuf createDirBuf(fuse_req_t req, const std::string & dir, const StStat & st) const;

      public:
        FuseSession(const boost::asio::any_io_executor & ex, const std::string & local, const std::string & remote)
            : AsyncSocket<boost::asio::local::stream_protocol::socket>(ex), fuse_strand_{boost::asio::make_strand(ex)}, localPoint{local}, remotePoint{remote} {
            // added parent inode: 1
            StStat st = {};

            if(0 > ::stat(local.c_str(), & st)) {
                Application::error("{}: {} failed, error: {}, code: {}, path: `{}'",
                                   NS_FuncNameV, "stat", strerror(errno), errno, local);
                throw fuse_error(NS_FuncNameS);
            }

            Application::debug(DebugType::Fuse, "{}: added ino: {}, path: `{}'", NS_FuncNameV, 1, local);
            auto pair = inodes.emplace(1, PathStat{"/", std::move(st)});
            pathes.emplace("/", pair.first->second.statPtr());
        }

        ~FuseSession() {
            stopSession();
        }

        asio::awaitable<void> retryConnect(const std::string &, int);
        asio::awaitable<void> remoteHandshake(void);
        asio::awaitable<StStat> recvStatStruct(void) const;

        bool startSession(void);

        inline bool localPath(std::string_view path) const {
            return localPoint == path;
        }

        inline bool socketPath(std::string_view path) const {
            return socket().local_endpoint().path() == path;
        }

        inline bool socketConnected(void) const {
            return socket().is_open();
        }

        boost::asio::strand<boost::asio::any_io_executor> & strand(void) {
            return fuse_strand_;
        }

        // fuse low level callbacks
        asio::awaitable<void> cbLookup(fuse_req_t req, fuse_ino_t parent, const char* path) const;
        asio::awaitable<void> cbGetAttr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*) const;
        asio::awaitable<void> cbReadLink(fuse_req_t req, fuse_ino_t ino) const;
        asio::awaitable<void> cbReadDir(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t off, struct fuse_file_info*);
        asio::awaitable<void> cbOpen(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*) const;
        asio::awaitable<void> cbRelease(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*) const;
        asio::awaitable<void> cbRead(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t offset, struct fuse_file_info*) const;
        asio::awaitable<void> cbAccess(fuse_req_t req, fuse_ino_t ino, int mask) const;
    };

    //    std::string PathStat::localPath(const FuseSession* fuse) const {
    //        return fuse ? std::string(fuse->localPoint).append(first) : first;
    //    }

    //    std::string PathStat::remotePath(const FuseSession* fuse) const {
    //        return fuse ? std::string(fuse->remotePoint).append(first) : first;
    //    }

    void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char* path) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbLookup, fuse, req, parent, path), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbGetAttr, fuse, req, ino, fi), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_readlink(fuse_req_t req, fuse_ino_t ino) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbReadLink, fuse, req, ino), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t off, struct fuse_file_info* fi) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbReadDir, fuse, req, ino, maxsize, off, fi), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbOpen, fuse, req, ino, fi), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbRelease, fuse, req, ino, fi), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_read(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t off, struct fuse_file_info* fi) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbRead, fuse, req, ino, maxsize, off, fi), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    void ll_access(fuse_req_t req, fuse_ino_t ino, int mask) {
        if(auto fuse = static_cast<FuseSession*>(fuse_req_userdata(req))) {
            asio::co_spawn(fuse->strand(), std::bind(&FuseSession::cbAccess, fuse, req, ino, mask), asio::detached);
        } else {
            fuse_reply_err(req, EFAULT);
        }
    }

    // FuseSession
    boost::asio::awaitable<void> FuseSession::retryConnect(const std::string & path, int attempts) {
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer{executor};

        for(int it = 1; it <= attempts; it++) {
            try {
                co_await socket().async_connect(path, asio::use_awaitable);
                co_return;
            } catch(const system::system_error& ec) {
                if(it == attempts) {
                    Application::warning("{}: {} failed, path: {}, attempts: {}", NS_FuncNameV, "connect", path, attempts);
                    throw;
                }
            }

            timer.expires_after(300ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
    }

    boost::asio::awaitable<void> FuseSession::remoteHandshake(void) {
        // send inititialize packet
        const uint16_t protoVer = 1;

        // <VER16> - proto version
        // <LEN16><MOUNTPOINT> - mount point
        co_await async_send_values(
            endian::native_to_little(static_cast<uint16_t>(FuseOp::Init)),
            endian::native_to_little(static_cast<uint16_t>(protoVer)),
            endian::native_to_little(static_cast<uint16_t>(remotePoint.size())),
            remotePoint);

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno
        uint16_t cmd;
        uint32_t err;
        co_await async_recv_values(cmd, err);

        endian::little_to_native_inplace(cmd);
        endian::little_to_native_inplace(err);

        if(cmd != FuseOp::Init) {
            Application::error("{}: {}: failed, cmd: {:#06x}", NS_FuncNameV, "id", cmd);
            throw fuse_error(NS_FuncNameS);
        }

        if(err) {
            Application::error("{}: recv error: {:#06x}", NS_FuncNameV, err);
            throw fuse_error(NS_FuncNameS);
        }

        // <UID16> - proto
        uint16_t ver;
        uint32_t uid, gid, count;

        co_await async_recv_values(ver, uid, gid, count);

        endian::little_to_native_inplace(ver);
        endian::little_to_native_inplace(uid);
        endian::little_to_native_inplace(gid);
        endian::little_to_native_inplace(count);

        remoteUid = uid;
        remoteGid = gid;

        if(ver != protoVer) {
            Application::error("{}: unsupported proto version: {}", NS_FuncNameV, ver);
            throw fuse_error(NS_FuncNameS);
        }

        Application::debug(DebugType::Fuse, "{}: proto version: {}", NS_FuncNameV, ver);

        // init inode root info
        inodes.reserve(count);

        while(count--) {
            // <LEN16><PATH> - remote path
            auto len = co_await async_recv_le16();
            auto path = co_await async_recv_buf<std::string>(len);

            auto st = co_await recvStatStruct();
            auto ino = st.st_ino;

            if(ino != 1 &&
               startsWith(path, remotePoint)) {
                path = path.substr(remotePoint.size());
                Application::debug(DebugType::Fuse, "{}: added ino: {}, path: `{}'", NS_FuncNameV, ino, path);
                // added relative path
                auto pair = inodes.emplace(ino, PathStat{path, std::move(st)});
                pathes.emplace(std::move(path), pair.first->second.statPtr());
            }
        }

        // symlinks
        count = co_await async_recv_le32();

        while(count--) {
            uint64_t ino1, ino2;
            co_await async_recv_values(ino1, ino2);
            endian::little_to_native_inplace(ino1);
            endian::little_to_native_inplace(ino2);
            symlinks.emplace_front(std::make_pair(ino1, ino2));
        }

        co_return;
    }

    boost::asio::awaitable<StStat> FuseSession::recvStatStruct(void) const {
        // <STAT> - stat struct
        uint64_t dev, ino, nlink, rdev, size, blksize, blocks, atime, mtime, ctime;
        uint32_t mode, uid, gid;

        co_await async_recv_values(dev, ino, mode, nlink, uid, gid, rdev, size, blksize, blocks, atime, mtime, ctime);

        StStat st;
        st.st_dev = endian::little_to_native(dev);
        st.st_ino = endian::little_to_native(ino);
        st.st_mode = endian::little_to_native(mode);
        st.st_nlink = endian::little_to_native(nlink);
        st.st_uid = endian::little_to_native(uid);
        st.st_gid = endian::little_to_native(gid);
        st.st_rdev = endian::little_to_native(rdev);
        st.st_size = endian::little_to_native(size);
        st.st_blksize = endian::little_to_native(blksize);
        st.st_blocks = endian::little_to_native(blocks);
        st.st_atime = endian::little_to_native(atime);
        st.st_mtime = endian::little_to_native(mtime);
        st.st_ctime = endian::little_to_native(ctime);
        // force uid/gid
        st.st_uid = remoteUid != st.st_uid ? 0 : getuid();
        st.st_gid = remoteGid != st.st_gid ? 0 : getgid();

        co_return st;
    }

    bool FuseSession::startSession(void) {
        // fuse init
        oper.init = nullptr;
        oper.lookup = ll_lookup;
        oper.getattr = ll_getattr;
        oper.readdir = ll_readdir;
        oper.open = ll_open;
        oper.release = ll_release;
        oper.read = ll_read;
        oper.access = ll_access;
        oper.readlink = ll_readlink;

        ses.reset(fuse_session_new(& args, & oper, sizeof(oper), this));

        if(! ses) {
            Application::error("{}: {} failed", NS_FuncNameV, "fuse_session_new");
            return false;
        }

        // FIXME remove thread
        // fuse_session_get_fd(se)
        //fuse_set_signal_handlers(ses.get());

        if(0 > fuse_session_mount(ses.get(), localPoint.c_str())) {
            Application::error("{}: {} failed, local point: `{}'", NS_FuncNameV, "fuse_session_mount", localPoint);
            return false;
        }

        thloop = std::thread([ses = ses.get()]() {
            fuse_session_loop(ses);
        });

        return true;
    }

    void FuseSession::stopSession(void) noexcept {
        if(ses) {
            if(! fuse_session_exited(ses.get())) {
                fuse_session_unmount(ses.get());
                fuse_session_exit(ses.get());
            }

            ses.reset();
        }

        if(thloop.joinable()) {
            thloop.join();
        }

        fuse_opt_free_args(& args);
    }

    bool FuseSession::accessR(const StStat & st) const {
        return (S_IROTH & st.st_mode) ||
               ((S_IRGRP & st.st_mode) && st.st_gid == remoteGid) ||
               ((S_IRUSR & st.st_mode) && st.st_uid == remoteUid);
    }

    bool FuseSession::accessW(const StStat & st) const {
        return (S_IWOTH & st.st_mode) ||
               ((S_IWGRP & st.st_mode) && st.st_gid == remoteGid) ||
               ((S_IWUSR & st.st_mode) && st.st_uid == remoteUid);
    }

    bool FuseSession::accessX(const StStat & st) const {
        return (S_IXOTH & st.st_mode) ||
               ((S_IXGRP & st.st_mode) && st.st_gid == remoteGid) ||
               ((S_IXUSR & st.st_mode) && st.st_uid == remoteUid);
    }

    const LinkInfo* FuseSession::findLink(fuse_ino_t inode) const {
        auto it = std::ranges::find_if(symlinks, [&](auto & st) {
            return st.first == inode;
        });

        return it != symlinks.end() ? std::addressof(*it) : nullptr;
    }

    const PathStat* FuseSession::findInode(fuse_ino_t inode) const {
        auto iti = inodes.find(inode);

        if(iti == inodes.end()) {
            return nullptr;
        }

        return std::addressof(iti->second);
    }

    const StStat* FuseSession::findChildStat(fuse_ino_t parent, const char* child) const {
        auto iti = inodes.find(parent);

        if(iti == inodes.end()) {
            return nullptr;
        }

        auto path = iti->second.joinPath(child);

        if(auto itp = pathes.find(path); itp != pathes.end()) {
            return itp->second;
        }

        Application::warning("{}: not found, ino: {}, path: `{}'", NS_FuncNameV, parent, path);
        return nullptr;
    }

    DirBuf FuseSession::createDirBuf(fuse_req_t req, const std::string & dir, const StStat & st) const {
        DirBuf dirBuf(dir);
        dirBuf.addEntry(req, ".", st);
        dirBuf.addEntry(req, "..", st);
        auto it = pathes.upper_bound(dir);

        while(it != pathes.end()) {
            auto path = std::filesystem::path(it->first);

            if(path.parent_path() == dir) {
                dirBuf.addEntry(req, path.filename(), *it->second);
            }

            it = std::next(it);
        }

        return dirBuf;
    }

    asio::awaitable<void> FuseSession::cbLookup(fuse_req_t req, fuse_ino_t parent, const char* path) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}, path: `{}'", NS_FuncNameV, parent, path);

        auto st = findChildStat(parent, path);

        if(! st) {
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        // <STAT> - stat struct
        fuse_entry_param entry = {};
        entry.attr = *st;
        entry.attr_timeout = 1.0;
        entry.entry_timeout = 1.0;
        entry.ino = st->st_ino;
        fuse_reply_entry(req, & entry);

        co_return;
    }

    asio::awaitable<void> FuseSession::cbGetAttr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}", NS_FuncNameV, ino);

        if(fi) {
            Application::debug(DebugType::Fuse, "{}: file info - flags: {:#010x}, fh: {}", NS_FuncNameV, fi->flags, fi->fh);
        }

        auto pathStat = findInode(ino);

        if(! pathStat) {
            Application::error("{}: {} failed", NS_FuncNameV, "inode");
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        fuse_reply_attr(req, pathStat->statPtr(), 1.0);

        co_return;
    }

    asio::awaitable<void> FuseSession::cbReadLink(fuse_req_t req, fuse_ino_t ino) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}", NS_FuncNameV, ino);

        auto pathStat = findInode(ino);

        if(! pathStat) {
            Application::error("{}: {} failed", NS_FuncNameV, "inode");
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        if(! S_ISLNK(pathStat->statRef().st_mode)) {
            Application::error("{}: {} failed", NS_FuncNameV, "islnk");
            fuse_reply_err(req, EINVAL);
            co_return;
        }

        auto pair = findLink(ino);

        if(! pair) {
            Application::error("{}: {} failed", NS_FuncNameV, "findLink");
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        pathStat = findInode(pair->second);

        if(! pathStat) {
            Application::error("{}: {} failed", NS_FuncNameV, "findInode");
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        auto path = std::string(localPoint).append(pathStat->relativePath());
        fuse_reply_readlink(req, path.c_str());

        co_return;
    }

    asio::awaitable<void> FuseSession::cbReadDir(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t off, struct fuse_file_info* fi) {
        Application::debug(DebugType::Fuse, "{}: ino: {}, max size: {}, offset: {}", NS_FuncNameV, ino, maxsize, off);

        if(fi) {
            Application::debug(DebugType::Fuse, "{}: file info - flags: {:#010x}, fh: {}", NS_FuncNameV, fi->flags, fi->fh);
        }

        auto pathStat = findInode(ino);

        if(! pathStat) {
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        if(! S_ISDIR(pathStat->statRef().st_mode)) {
            fuse_reply_err(req, ENOTDIR);
            co_return;
        }

        // relative path
        const auto & path = pathStat->relativePath();

        if(dirBuf.root != path) {
            dirBuf = createDirBuf(req, path, pathStat->statRef());
        }

        if((size_t) off < dirBuf.size()) {
            fuse_reply_buf(req, dirBuf.data() + off, std::min(dirBuf.size() - off, maxsize));
        } else {
            fuse_reply_buf(req, nullptr, 0);
        }

        co_return;
    }

    asio::awaitable<void> FuseSession::cbOpen(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}", NS_FuncNameV, ino);

        if(fi) {
            Application::debug(DebugType::Fuse, "{}: file info - flags: {:#010x}, fh: {}", NS_FuncNameV, fi->flags, fi->fh);
        } else {
            Application::error("{}: {} failed", NS_FuncNameV, "fuse_file_info");
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        auto pathStat = findInode(ino);

        if(! pathStat) {
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        if(S_ISDIR(pathStat->statRef().st_mode)) {
            fuse_reply_err(req, EISDIR);
            co_return;
        }

        assert(patch.size() < UINT16_MAX);

        const auto & path = pathStat->relativePath();

        // <FLAG32> - open flags
        // <LEN16><PATH> - fuse path
        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(FuseOp::Open)),
                endian::native_to_little(static_cast<uint32_t>(fi->flags)),
                endian::native_to_little(static_cast<uint16_t>(path.size())),
                path);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno
        uint16_t cmd;
        uint32_t err;

        try {
            co_await async_recv_values(cmd, err);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        endian::little_to_native_inplace(cmd);
        endian::little_to_native_inplace(err);

        if(cmd != FuseOp::Open) {
            Application::error("{}: {}: failed, cmd: {:#06x}", NS_FuncNameV, "id", cmd);
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        if(err) {
            Application::error("{}: recv error: {}", NS_FuncNameV, err);
            fuse_reply_err(req, err);
            co_return;
        }

        // <FDH32> - fd handle
        try {
            fi->fh = co_await async_recv_le32();
            fuse_reply_open(req, fi);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
        }

        co_return;
    }

    asio::awaitable<void> FuseSession::cbRelease(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}", NS_FuncNameV, ino);

        if(fi) {
            Application::debug(DebugType::Fuse, "{}: file info - flags: {:#010x}, fh: {}", NS_FuncNameV, fi->flags, fi->fh);
        } else {
            Application::error("{}: {} failed", NS_FuncNameV, "fuse_file_info");
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        auto pathStat = findInode(ino);

        if(! pathStat) {
            fuse_reply_err(req, EBADF);
            co_return;
        }

        if(S_ISDIR(pathStat->statRef().st_mode)) {
            fuse_reply_err(req, EBADF);
            co_return;
        }

        // <FDH32> - fd handle
        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(FuseOp::Release)),
                endian::native_to_little(static_cast<uint32_t>(fi->fh)));
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno
        uint16_t cmd;
        uint32_t err;

        try {
            co_await async_recv_values(cmd, err);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        if(cmd != FuseOp::Release) {
            Application::error("{}: {}: failed, cmd: {:#06x}", NS_FuncNameV, "id", cmd);
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        if(err) {
            Application::error("{}: recv error: {}", NS_FuncNameV, err);
        }

        fuse_reply_err(req, err);
        co_return;
    }

    asio::awaitable<void> FuseSession::cbRead(fuse_req_t req, fuse_ino_t ino, size_t maxsize, off_t off, struct fuse_file_info* fi) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}", NS_FuncNameV, ino);

        if(fi) {
            Application::debug(DebugType::Fuse, "{}: file info - flags: {:#010x}, fh: {}", NS_FuncNameV, fi->flags, fi->fh);
        } else {
            Application::error("{}: {} failed", NS_FuncNameV, "fuse_file_info");
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        auto pathStat = findInode(ino);

        if(! pathStat) {
            fuse_reply_err(req, EBADF);
            co_return;
        }

        if(S_ISDIR(pathStat->statRef().st_mode)) {
            fuse_reply_err(req, EISDIR);
            co_return;
        }

        const size_t blockmax = 48 * 1024;
        const size_t blocksz = std::min(maxsize, blockmax);

        // <FDH32> - fd handle
        // <SIZE16> - blocksz
        // <OFF64> - offset
        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(FuseOp::Read)),
                endian::native_to_little(static_cast<uint32_t>(fi->fh)),
                endian::native_to_little(static_cast<uint16_t>(blocksz)),
                endian::native_to_little(static_cast<uint64_t>(off)));
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        // reply format:
        // <CMD16> - fuse cmd
        // <ERR32> - errno ot read sz
        uint16_t cmd;
        uint32_t err;

        try {
            co_await async_recv_values(cmd, err);
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        if(cmd != FuseOp::Read) {
            Application::error("{}: {}: failed, cmd: {:#06x}", NS_FuncNameV, "id", cmd);
            fuse_reply_err(req, EFAULT);
            co_return;
        }

        if(err) {
            Application::error("{}: recv error: {}", NS_FuncNameV, err);
            fuse_reply_err(req, err);
            co_return;
        }

        // <LEN16><DATA> - raw data
        using binary_buf = std::vector<char>;

        try {
            auto len = co_await async_recv_le16();

            if(len) {
                auto buf = co_await async_recv_buf<binary_buf>(len);
                fuse_reply_buf(req, buf.data(), buf.size());
            } else {
                fuse_reply_buf(req, nullptr, 0);
            }
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            fuse_reply_err(req, EFAULT);
        }

        co_return;
    }

    asio::awaitable<void> FuseSession::cbAccess(fuse_req_t req, fuse_ino_t ino, int mask) const {
        Application::debug(DebugType::Fuse, "{}: ino: {}, mask: {:#010x}", NS_FuncNameV, ino, mask);

        if(ino == 1) {
            fuse_reply_err(req, 0);
            co_return;
        }

        auto pathStat = findInode(ino);

        if(! pathStat) {
            fuse_reply_err(req, ENOENT);
            co_return;
        }

        if(mask == F_OK) {
            fuse_reply_err(req, 0);
            co_return;
        }

        if(mask & (R_OK | W_OK | X_OK)) {
            auto & st = pathStat->statRef();

            if((mask & R_OK) && ! accessR(st)) {
                fuse_reply_err(req, EACCES);
                co_return;
            }

            if((mask & W_OK) && ! accessW(st)) {
                fuse_reply_err(req, EACCES);
                co_return;
            }

            if((mask & X_OK) && ! accessX(st)) {
                fuse_reply_err(req, EACCES);
                co_return;
            }

            fuse_reply_err(req, 0);
            co_return;
        }

        fuse_reply_err(req, EINVAL);
        co_return;
    }

    /// FuseSessionBus
    FuseSessionBus::FuseSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_fuse"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_fuse_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_fuse_path),
#endif
        signals_ {ioc_}, dbus_conn_ {std::move(conn)} {
        registerAdaptor();

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }
    }

    FuseSessionBus::~FuseSessionBus() {
        unregisterAdaptor();
        stop();
    }

    void FuseSessionBus::stop(void) {
        childs_.clear();
        signals_.cancel();
        dbus_conn_->leaveEventLoop();
    }

    int FuseSessionBus::start(void) {
        Application::info("service started, uid: {}, pid: {}, version: {}", getuid(), getpid(), LTSM_SESSION_FUSE_VERSION);

        std::signal(SIGPIPE, SIG_IGN);

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const system::error_code & ec, int signal) {
            // skip canceled
            if(ec != asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT)) {
                this->stop();
            }
        });

        auto sdbus_job = std::thread([this]() {
            try {
                dbus_conn_->enterEventLoop();
            } catch(const std::exception & err) {
                Application::error("sdbus exception: {}", err.what());
                asio::post(ioc_, std::bind(&FuseSessionBus::stop, this));
            }
        });

        ioc_.run();

        dbus_conn_->leaveEventLoop();
        sdbus_job.join();

        Application::debug(DebugType::App, "Fuse session shutdown");
        return EXIT_SUCCESS;
    }

    int32_t FuseSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "{}", NS_FuncNameV);
        return LTSM_SESSION_FUSE_VERSION;
    }

    void FuseSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", NS_FuncNameV, getpid());
        asio::post(ioc_, std::bind(&FuseSessionBus::stop, this));
    }

    bool FuseSessionBus::mountPoint(const std::string & localPoint, const std::string & remotePoint,
                                    const std::string & socketPath) {
        Application::debug(DebugType::Dbus, "{}: local point: `{}', remote point: `{}', fuse socket: `{}'",
                           NS_FuncNameV, localPoint, remotePoint, socketPath);

        if(std::ranges::any_of(childs_, [&](auto & ptr) {
        return ptr->localPath(localPoint) && ptr->socketConnected();
        })) {
            Application::error("{}: point busy, point: `{}'", NS_FuncNameV, localPoint);
            return false;
        }

        asio::co_spawn(ioc_,
        [localPoint, remotePoint, socketPath, this]() -> asio::awaitable<void>  {
            auto executor = co_await asio::this_coro::executor;
            auto sess = std::make_unique<FuseSession>(executor, localPoint, remotePoint);

            try {
                co_await sess->retryConnect(socketPath, 5);
                co_await sess->remoteHandshake();

                if(sess->startSession()) {
                    childs_.emplace_front(std::move(sess));
                }
            } catch(const system::system_error& err) {
                auto ec = err.code();
                Application::error("{}: {} failed, code: {}, error: {}", NS_FuncNameV, "remoteHandshake", "asio", ec.value(), ec.message());
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            }

        }, asio::detached);

        return true;
    }

    void FuseSessionBus::setDebug(const std::string & level) {
        LTSM::Application::debug(DebugType::Dbus, "{}: level: {}", NS_FuncNameV, level);
        setDebugLevel(level);
    }

    void FuseSessionBus::umountPoint(const std::string & localPoint) {
        LTSM::Application::debug(DebugType::Dbus, "{}: local point: `{}'", NS_FuncNameV, localPoint);
        std::erase_if(childs_, [&](auto & ptr) {
            return ptr->localPath(localPoint);
        });
    }
}

int main(int argc, char** argv) {
    bool debug = false;

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            std::cout << "usage: " << argv[0] << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v")) {
            std::cout << "version: " << LTSM_SESSION_FUSE_VERSION << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--debug") || 0 == std::strcmp(argv[it], "-d")) {
            debug = true;
        }
    }

    if(0 == getuid()) {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    try {
#ifdef SDBUS_2_0_API
        auto conn = sdbus::createSessionBusConnection(sdbus::ServiceName {LTSM::dbus_session_fuse_name});
#else
        auto conn = sdbus::createSessionBusConnection(LTSM::dbus_session_fuse_name);
#endif
        return LTSM::FuseSessionBus(std::move(conn), debug).start();
    } catch(const sdbus::Error & err) {
        LTSM::Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return EXIT_FAILURE;
}
