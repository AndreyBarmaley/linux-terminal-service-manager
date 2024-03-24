/***************************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 **************************************************************************/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <string>
#include <memory>
#include <filesystem>

#include "ltsm_fuse.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_channels.h"
#include "ltsm_application.h"

namespace LTSM
{
    namespace Channel
    {
        namespace Connector
        {
            // channel_system.cpp
            void loopWriter(ConnectorBase*, Remote2Local*);
            void loopReader(ConnectorBase*, Local2Remote*);
        }
    }

    void replyWriteStatStruct(StreamBuf & reply, const struct stat & st)
    {
        reply.writeIntLE64(st.st_dev);
        reply.writeIntLE64(st.st_ino);
        reply.writeIntLE32(st.st_mode);
        reply.writeIntLE64(st.st_nlink);
        reply.writeIntLE32(st.st_uid);
        reply.writeIntLE32(st.st_gid);
        reply.writeIntLE64(st.st_rdev);
        reply.writeIntLE64(st.st_size);
        reply.writeIntLE64(st.st_blksize);
        reply.writeIntLE64(st.st_blocks);
        reply.writeIntLE64(st.st_atime);
        reply.writeIntLE64(st.st_mtime);
        reply.writeIntLE64(st.st_ctime);
    }

    void replyWriteShareRootInfo(StreamBuf & reply, const std::string & dir)
    {
        std::unordered_map<ino_t, std::pair<std::string, struct stat>> inodes;
        auto items = Tools::readDir(dir, true);

        for(auto & path: items)
        {
            struct stat st = {};

            if(0 > ::stat(path.c_str(), & st))
            {
                Application::error("%s: %s failed, error: %s, code: %d, path: `%s'",
                        __FUNCTION__, "stat", strerror(errno), errno, path.c_str());
                continue;
            }

            if(0 == (st.st_mode & (S_IFREG | S_IFDIR)))
            {
                Application::warning("%s: %s, mode: 0x%" PRIx64 ", path: `%s'",
                        __FUNCTION__, "special skipped", st.st_mode, path.c_str());
                continue;
            }

            inodes.emplace(st.st_ino, std::make_pair(path, std::move(st)));
        }

        reply.writeIntLE32(inodes.size());
        for(auto & st: inodes)
        {
            reply.writeIntLE16(std::min(size_t(UINT16_MAX), st.second.first.size()));
            reply.write(st.second.first);

            replyWriteStatStruct(reply, st.second.second);
        }
    }
}

// createClientFuseConnector
std::unique_ptr<LTSM::Channel::ConnectorBase>
    LTSM::Channel::createClientFuseConnector(uint8_t channel, const std::string & url, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(), Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::Unknown)
    {
        Application::error("%s: %s, mode: %s", __FUNCTION__, "fuse mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<ConnectorClientFuse>(channel, url, mode, chOpts, sender);
}

/// ConnectorClientFuse
LTSM::Channel::ConnectorClientFuse::ConnectorClientFuse(uint8_t ch, const std::string & url, const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, mod, chOpts, srv), reply(4096), cid(ch)
{
    Application::info("%s: channelId: %" PRIu8, __FUNCTION__, cid);

    // start threads
    setRunning(true);
}

LTSM::Channel::ConnectorClientFuse::~ConnectorClientFuse()
{
    setRunning(false);

    for(auto & fd: opens)
        ::close(fd);
}

int LTSM::Channel::ConnectorClientFuse::error(void) const
{
    return 0;
}

uint8_t LTSM::Channel::ConnectorClientFuse::channel(void) const
{
    return cid;
}

void LTSM::Channel::ConnectorClientFuse::setSpeed(const Channel::Speed & speed)
{
}

void LTSM::Channel::ConnectorClientFuse::pushData(std::vector<uint8_t> && recv)
{
    StreamBufRef sb;

    if(last.empty())
    {
        sb.reset(recv.data(), recv.size());
    }
    else
    {
        last.insert(last.end(), recv.begin(), recv.end());
        recv.swap(last);
        sb.reset(recv.data(), recv.size());
        last.clear();
    }

    const uint8_t* beginPacket = nullptr;
    const uint8_t* endPacket = nullptr;
        
    try
    {
        while(2 < sb.last())
        {
            // fuse stream format:
            // <CMD16> - audio cmd
            // <DATA> - audio data
            beginPacket = sb.data();
            endPacket = beginPacket + sb.last();

            auto fuseCmd = sb.readIntLE16();
            Application::debug("%s: cmd: 0x%" PRIx16, __FUNCTION__, fuseCmd);

            if(! fuseInit && fuseCmd != FuseOp::Init)
            {
                Application::error("%s: %s failed, cmd: 0x%" PRIx16, __FUNCTION__, "initialize", fuseCmd);
                throw channel_error(NS_FuncName);
            }

            switch(fuseCmd)
            {
                case FuseOp::Init:      fuseOpInit(sb); break;
                // disabled, only for session part
                //case FuseOp::Quit:    fuseOpQuit(sb); break;
                //case FuseOp::GetAttr: fuseOpGetAttr(sb); break;
                //case FuseOp::ReadDir: fuseOpReadDir(sb); break;
                //case FuseOp::Lookup:  fuseOpLookup(sb); break;
                //
                case FuseOp::Open:      fuseOpOpen(sb); break;
                case FuseOp::Read:      fuseOpRead(sb); break;
                case FuseOp::Release:   fuseOpRelease(sb); break;

                default:
                    Application::error("%s: %s failed, cmd: 0x%" PRIx16 ", recv size: %u", __FUNCTION__, "fuse", fuseCmd, recv.size());
                    throw channel_error(NS_FuncName);
            }
        }

        if(sb.last())
            throw std::underflow_error(NS_FuncName);
    }
    catch(const std::underflow_error &)
    {
        Application::warning("%s: underflow data: %u", __FUNCTION__, sb.last());

        if(beginPacket)
            last.assign(beginPacket, endPacket);
        else
            last.swap(recv);
    }
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpInit(const StreamBufRef & sb)
{
    // cmd format:
    // <VER16> - proto version
    // <LEN16><MOUNTPOINT> - mount point
    if(sb.last() < 4)
        throw std::underflow_error(NS_FuncName);

    fuseVer = sb.readIntLE16();
    auto len = sb.readIntLE16();

    if(sb.last() < len)
        throw std::underflow_error(NS_FuncName);

    auto mountPoint = sb.readString(len);

    if(! owner->createChannelAllow(Channel::ConnectorType::Fuse, mountPoint, Channel::ConnectorMode::Unknown))
    {
        Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "mount point", mountPoint.c_str());
        fuseInit = false;
    }
    else
    {
        Application::info("%s: version: 0x%" PRIx16 ", mount point: `%s'", __FUNCTION__, fuseVer, mountPoint.c_str());
        shareRoot.assign(mountPoint);
        fuseInit = true;
    }

    // reply format:
    reply.reset();

    // <CMD16> - return code
    // <ERR32> - errno
    reply.writeIntLE16(FuseOp::Init);
    reply.writeIntLE32(fuseInit ? 0 : 1);

    if(fuseInit)
    {
        // proto ver
        reply.writeIntLE16(1);
        // <UID32> - local uid
        reply.writeIntLE32(getuid());
        // <GID32> - local gid
        reply.writeIntLE32(getgid());

        replyWriteShareRootInfo(reply, shareRoot);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());

    return true;
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpQuit(const StreamBufRef & sb)
{
    Application::error("%s: not implemented", __FUNCTION__);
    return false;
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpLookup(const StreamBufRef & sb)
{
    Application::error("%s: not implemented", __FUNCTION__);
    return false;
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpGetAttr(const StreamBufRef & sb)
{
    Application::error("%s: not implemented", __FUNCTION__);
    return false;
}

/*
bool LTSM::Channel::ConnectorClientFuse::sendStatFd(int fdh)
{
    struct stat st = {0};
    int ret = ::fstat(fdh, & st);
    int error = 0 > ret ? errno : 0;

    // reply format:
    reply.reset();

    // <CMD16> - return code
    // <ERR32> - errno
    reply.writeIntLE16(FuseOp::GetAttr);
    reply.writeIntLE32(error);

    if(0 > ret)
    {
        Application::error("%s: %s failed, error: %s, code: %d, fd: %d",
                    __FUNCTION__, "fstat", strerror(error), error, fdh);
    }
    else
    {
        Application::debug("%s: fd: %d", __FUNCTION__, fdh);

        // <STAT>
        replyWriteStatStruct(reply, st);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientFuse::sendStatPath(const char* path)
{
    struct stat st = {0};
    int ret = ::stat(path, & st);
    int error = 0 > ret ? errno : 0;

    // reply format:
    reply.reset();

    // <CMD16> - return code
    // <ERR32> - errno
    reply.writeIntLE16(FuseOp::GetAttr);
    reply.writeIntLE32(error);

    if(0 > ret)
    {
        Application::error("%s: %s failed, error: %s, code: %d, path: `%s'",
                    __FUNCTION__, "stat", strerror(error), error, path);
    }
    else
    {
        Application::debug("%s: path: `%s'", __FUNCTION__, path);

        // <STAT>
        replyWriteStatStruct(reply, st);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}
*/

bool LTSM::Channel::ConnectorClientFuse::fuseOpReadDir(const StreamBufRef & sb)
{
    Application::error("%s: not implemented", __FUNCTION__);
    return false;
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpOpen(const StreamBufRef & sb)
{
    // cmd format:
    // <FLAG32> - open flags
    // <LEN16><PATH> - fuse path
    if(sb.last() < 6)
        throw std::underflow_error(NS_FuncName);

    auto flags = sb.readIntLE32();
    auto len = sb.readIntLE16();

    if(sb.last() < len)
        throw std::underflow_error(NS_FuncName);

    auto path = shareRoot + sb.readString(len);

    int ret = ::open(path.c_str(), flags);
    int error = 0 > ret ? errno : 0;

    // reply format:
    reply.reset();

    // <CMD16> - return code
    // <ERR32> - errno
    reply.writeIntLE16(FuseOp::Open);
    reply.writeIntLE32(error);

    if(0 > ret)
    {
        Application::error("%s: %s failed, error: %s, code: %d, path: `%s', flags: 0x%" PRIx32,
                    __FUNCTION__, "open", strerror(error), error, path.c_str(), flags);
    }
    else
    {
        Application::debug("%s: path: `%s', flags: 0x%" PRIx32 ", fdh: %" PRId32, __FUNCTION__, path.c_str(), flags, ret);

        opens.push_front(ret);

        // <FDH32> - fd handle
        reply.writeIntLE32(ret);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpRelease(const StreamBufRef & sb)
{
    // cmd format:
    // <FDH32> - fd handle
    if(sb.last() < 4)
        throw std::underflow_error(NS_FuncName);

    auto fdh = sb.readIntLE32();

    int ret = ::close(fdh);
    int error = 0 > ret ? errno : 0;

    // reply format:
    reply.reset();

    // <CMD16> - return code
    // <ERR32> - errno
    reply.writeIntLE16(FuseOp::Release);
    reply.writeIntLE32(error);

    if(0 > ret)
    {
        Application::error("%s: %s failed, error: %s, code: %d, fd: %" PRId32,
                    __FUNCTION__, "close", strerror(error), error, fdh);
    }
    else
    {
        Application::debug("%s: fd: %" PRId32, __FUNCTION__, fdh);

        opens.remove(fdh);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientFuse::fuseOpRead(const StreamBufRef & sb)
{
    // cmd format:
    // <FDH32> - fd handle
    // <SIZE64> - blocksz
    // <OFF64> - offset

    if(sb.last() < 20)
        throw std::underflow_error(NS_FuncName);

    int fdh = sb.readIntLE32();
    size_t blocksz = sb.readIntLE64();
    size_t offset = sb.readIntLE64();

    int ret = lseek(fdh, offset, SEEK_SET);
    int error = 0 > ret ? errno : 0;

    // reply format:
    reply.reset();

    if(0 > ret)
    {
        // <CMD16> - return code
        // <ERR32> - errno
        reply.writeIntLE16(FuseOp::Read);
        reply.writeIntLE32(error);

        Application::error("%s: %s failed, error: %s, code: %d, offset: %u",
                    __FUNCTION__, "lseek", strerror(error), error, offset);
        owner->sendLtsmEvent(cid, reply.rawbuf());
        return true;
    }

    const size_t blockmax = 48 * 1024;
    std::vector<uint8_t> buf(std::min(blocksz, blockmax));

    ssize_t rsz = ::read(fdh, buf.data(), buf.size());
    error = 0 > rsz ? errno : 0;

    // <CMD16> - return code
    // <ERR32> - errno
    reply.writeIntLE16(FuseOp::Read);
    reply.writeIntLE32(error);

    if(0 > rsz)
    {
        Application::error("%s: %s failed, error: %s, code: %d, fd: %d",
                    __FUNCTION__, "read", strerror(error), error, fdh);
    }
    else
    {
        Application::debug("%s: request block size: %u, send block size: %u, offset: %u", __FUNCTION__, blocksz, rsz, offset);

        if(rsz < buf.size())
            buf.resize(rsz);

        // <LEN16>
        reply.writeIntLE16(buf.size());
        // <DATA>
        reply.write(buf);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}
