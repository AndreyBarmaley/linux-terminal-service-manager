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

#include <signal.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <condition_variable>

#include "pcsclite.h"

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_pcsc_session.h"

using namespace std::chrono_literals;

namespace PcscLite
{
    enum ScardState
    {
        StateUnknown = 0x0001,
        StateAbsent = 0x0002,
        StatePresent = 0x0004,
        StateSwallowed = 0x0008,
        StatePowered = 0x0010,
        StateNegotiable = 0x0020,
        StateSpecific = 0x0040
    };

    void ReaderState::reset(void)
    {
        event = 0;
        state = 0;
        share = 0;
        atrLen = MAX_ATR_SIZE;
        protocol = 0;
        std::fill(std::begin(name), std::end(name), 0);
        std::fill(std::begin(atr), std::end(atr), 0);
    }
    
    inline const char* err2str(uint32_t err)
    {
        return pcsc_stringify_error(err);
    }
}

namespace LTSM
{
    struct WaitTransaction
    {
        std::mutex lock;
        std::condition_variable cv;
        std::forward_list<const PcscLite::ReaderState*> listLocked;
        bool shutdown = false;

        void shutdownClient(void)
        {
            const std::lock_guard<std::mutex> lk{ lock };
            cv.notify_all();
        }

        void shutdownNotify(void)
        {
            const std::lock_guard<std::mutex> lk{ lock };
            shutdown = true;
            cv.notify_all();
        }

        void readerUnlock(const PcscLite::ReaderState* st)
        {
            const std::lock_guard<std::mutex> lk{ lock };
            listLocked.remove(st);
            cv.notify_all();
        }

        bool readerLock(const PcscLite::ReaderState* st)
        {
            Application::trace(DebugType::Pcsc, "%s: reader: %p wait", __FUNCTION__, st);
            std::unique_lock<std::mutex> lk{ lock };
            cv.wait(lk, [&]
            {
                return shutdown ||
                std::none_of(listLocked.begin(), listLocked.end(), [&](auto & ptr) { return st == ptr; });
            });
            listLocked.push_front(st);
            Application::trace(DebugType::Pcsc, "%s: reader: %p success", __FUNCTION__, st);
            return ! shutdown;
        }
    };

    std::unique_ptr<sdbus::IConnection> conn;
    std::atomic<bool> pcscShutdown{false};
    WaitTransaction waitTransaction;

    void signalHandler(int sig)
    {
        if(sig == SIGTERM || sig == SIGINT)
        {
            pcscShutdown = true;

            if(conn)
            {
                conn->leaveEventLoop();
            }
        }
    }

    /// PcscClient
    PcscClient::PcscClient(int fd, PcscSessionBus* sessionBus)
    {
        sock.setSocket(fd);
        thread = std::thread([this, sessionBus]()
        {
            std::this_thread::sleep_for(10ms);

            while(! this->shutdown)
            {
                if(! this->sock.hasInput())
                {
                    std::this_thread::sleep_for(10ms);
                    continue;
                }

                try
                {
                    if(sessionBus->pcscClientAction(*this))
                    {
                        continue;
                    }
                }
                catch(const std::exception & ex)
                {
                    Application::warning("%s: clientId: %" PRId32 ", remoteContext: 0x%016" PRIx64 ", localContext: 0x%08" PRIx32,
                        "ClientContextThread", this->sock.fd(), this->remoteContext, this->context);
                    Application::error("%s: exception: %s", "PcscClientThread", ex.what());
                }

                this->shutdown = true;
            }

            waitTransaction.shutdownClient();
        });
    }

    PcscClient::~PcscClient()
    {
        waitStatusChanged.stop();
        shutdown = true;
        waitTransaction.shutdownClient();

        if(thread.joinable())
        {
            thread.join();
        }
    }

    /// PcscSessionBus
    PcscSessionBus::PcscSessionBus(sdbus::IConnection & conn, bool debug)
#ifdef SDBUS_2_0_API
        : AdaptorInterfaces(conn, sdbus::ObjectPath{dbus_session_pcsc_path}),
#else
        : AdaptorInterfaces(conn, dbus_session_pcsc_path),
#endif
         Application("ltsm_pcsc2session")
    {
        Application::setDebug(DebugTarget::Syslog, debug ? DebugLevel::Debug : DebugLevel::Info);
        registerAdaptor();
    }

    PcscSessionBus::~PcscSessionBus()
    {
        unregisterAdaptor();
        close(socketFd);

        if(std::filesystem::is_socket(pcscSocketPath))
        {
            std::filesystem::remove(pcscSocketPath);
        }
    }

    int PcscSessionBus::start(void)
    {
        Application::info("%s: uid: %d, pid: %d, version: %d", __FUNCTION__, getuid(), getpid(), LTSM_PCSC2SESSION_VERSION);

	if(auto envSockName = getenv("PCSCLITE_CSOCK_NAME"))
	{
	    pcscSocketPath = envSockName;
	}

	if(pcscSocketPath.empty())
	{
    	    Application::error("%s: environment not found: %s", __FUNCTION__, "PCSCLITE_CSOCK_NAME");
    	    return EXIT_FAILURE;
	}

        Application::info("%s: socket path: `%s'", __FUNCTION__, pcscSocketPath.c_str());

	if(std::filesystem::is_socket(pcscSocketPath))
	{
    	    std::filesystem::remove(pcscSocketPath);
	    Application::warning("%s: socket found: %s", __FUNCTION__, pcscSocketPath.c_str());
	}

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);
        socketFd = UnixSocket::listen(pcscSocketPath, 50);

        if(0 > socketFd)
        {
    	    Application::error("%s: socket failed", __FUNCTION__);
            return EXIT_FAILURE;
        }
        
        Tools::Timeout timeout(1s);
	
        while(! pcscShutdown)
        {
            // accept new client
            if(NetworkStream::hasInput(socketFd, 1))
            {
                if(auto sock = UnixSocket::accept(socketFd); 0 < sock)
                {
                    Application::debug(DebugType::Pcsc, "%s: add clientId: %" PRId32, __FUNCTION__, sock);
                    const std::scoped_lock guard{ clientsLock };
                    clients.emplace_front(sock, this);
                }
            }

	    if(timeout.check())
	    {
                const std::scoped_lock guard{ clientsLock };
                clients.remove_if([](auto & st)
                {
                    return !!st.shutdown;
                });
	    }

            conn->enterEventLoopAsync();
            std::this_thread::sleep_for(5ms);
        }

        pcscShutdown = true;
        waitTransaction.shutdownNotify();
        return EXIT_SUCCESS;
    }

    int32_t PcscSessionBus::getVersion(void)
    {
        Application::debug(DebugType::Pcsc, "%s", __FUNCTION__);
        return LTSM_PCSC2SESSION_VERSION;
    }

    void PcscSessionBus::serviceShutdown(void)
    {
        Application::info("%s", __FUNCTION__);
        pcscShutdown = true;
    }

    void PcscSessionBus::setDebug(const std::string & level)
    {
        setDebugLevel(level);
    }

    bool PcscSessionBus::connectChannel(const std::string & clientPath)
    {
        bool waitSocket = Tools::waitCallable<std::chrono::milliseconds>(5000, 100, [&]()
        {
            return Tools::checkUnixSocket(clientPath);
        });

        if(! waitSocket)
        {
            Application::error("%s: checkUnixSocket failed, `%s'", __FUNCTION__, clientPath.c_str());
            return false;
        }

        Application::info("%s: client socket path: `%s'", __FUNCTION__, clientPath.c_str());
        int sockfd = UnixSocket::connect(clientPath);

        if(0 > sockfd)
        {
            return false;
        }

        ltsm = std::make_unique<SocketStream>(sockfd);
        return true;
    }

    void PcscSessionBus::disconnectChannel(const std::string & clientPath)
    {
        Application::info("%s: client socket path: `%s'", __FUNCTION__, clientPath.c_str());
    }

    bool PcscSessionBus::pcscClientAction(PcscClient & st)
    {
        uint32_t len = st.sock.recvInt32();
        uint32_t cmd = st.sock.recvInt32();
        Application::trace(DebugType::Pcsc, "%s: cmd: 0x%08" PRIx32 ", len: %" PRIu32, __FUNCTION__, cmd, len);

        switch(cmd)
        {
            case PcscLite::EstablishContext:
                return pcscEstablishContext(st, len);

            case PcscLite::ReleaseContext:
                return pcscReleaseContext(st, len);

            case PcscLite::Connect:
                return pcscConnect(st, len);

            case PcscLite::Reconnect:
                return pcscReconnect(st, len);

            case PcscLite::Disconnect:
                return pcscDisconnect(st, len);

            case PcscLite::BeginTransaction:
                return pcscBeginTransaction(st, len);

            case PcscLite::EndTransaction:
                return pcscEndTransaction(st, len);

            case PcscLite::Transmit:
                return pcscTransmit(st, len);

            case PcscLite::Status:
                return pcscStatus(st, len);

            case PcscLite::Control:
                return pcscControl(st, len);

            case PcscLite::Cancel:
                return pcscCancel(st, len);

            // not used
            case PcscLite::ListReaders:
            case PcscLite::GetStatusChange:
            case PcscLite::CancelTransaction:
                Application::error("%s: not used cmd: 0x%08" PRIx32 ", len: %" PRIu32, __FUNCTION__, cmd, len);
                break;

            case PcscLite::GetAttrib:
                return pcscGetAttrib(st, len);

            case PcscLite::SetAttrib:
                return pcscSetAttrib(st, len);

            case PcscLite::GetVersion:
                return pcscGetVersion(st, len);

            case PcscLite::GetReaderState:
                return pcscGetReaderState(st, len);

            case PcscLite::WaitReaderStateChangeStart:
                return pcscReaderStateChangeStart(st, len);

            case PcscLite::WaitReaderStateChangeStop:
                return pcscReaderStateChangeStop(st, len);

            default:
                Application::error("%s: unknown cmd: 0x%08" PRIx32 ", len: %" PRIu32, __FUNCTION__, cmd, len);
                break;
        }

        return false;
    }

    bool PcscSessionBus::pcscEstablishContext(PcscClient & st, uint32_t len)
    {
        if(len != 12)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t scope = st.sock.recvInt32();
        uint32_t context = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(st.context)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid context", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_PARAMETER).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << scope: %" PRIu32, __FUNCTION__, st.id(), scope);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::EstablishContext);
        ltsm->sendIntLE32(scope);
        ltsm->sendFlush();

        // wait
        auto remoteContext = ltsm->recvIntLE64();
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            // make localContext 32bit
            context = Tools::crc32b((const uint8_t*) & remoteContext, sizeof(remoteContext));
            context &= 0x7FFFFFFF;

            st.remoteContext = remoteContext;
            st.context = context;

            // init readers status
            syncReaders(st);

            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> remoteContext: 0x%016" PRIx64 ", localContext: 0x%08" PRIx32,
                               __FUNCTION__, st.id(), remoteContext, context);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", error: 0x%08" PRIx32 " (%s)", __FUNCTION__, st.id(), ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(scope).
        sendInt32(context).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscReleaseContext(PcscClient & st, uint32_t len)
    {
        if(len != 8)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t context = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteContext)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteContext", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(! context || context != st.context)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localContext: 0x%08" PRIx32, __FUNCTION__, st.id(), context);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteContext: 0x%016" PRIx64,
            __FUNCTION__, st.id(), st.remoteContext);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::ReleaseContext);
        ltsm->sendIntLE64(st.remoteContext);
        ltsm->sendFlush();
        // wait
        ret = ltsm->recvIntLE32();

        if(ret != SCARD_S_SUCCESS)
        {
            Application::error("%s: clientId: %" PRId32 ", remoteContext: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteContext, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(context).
        sendInt32(ret).sendFlush();

        // set shutdown
        st.shutdown = true;

        return ret == SCARD_S_SUCCESS;
    }

    PcscLite::ReaderState* PcscSessionBus::findReaderState(const std::string & name)
    {
        std::scoped_lock guard{ readersLock };
        auto it = std::find_if(readers.begin(), readers.end(),
                               [&](auto & rd)
        {
            return 0 == name.compare(rd.name);
        });

        return it != readers.end() ? std::addressof(*it) : nullptr;
    }

    bool PcscSessionBus::pcscConnect(PcscClient & st, uint32_t len)
    {
        if(len != 24 + MAX_READERNAME)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t context = st.sock.recvInt32();
        auto reader = st.sock.recvData(MAX_READERNAME);
        uint32_t shareMode = st.sock.recvInt32();
        uint32_t prefferedProtocols = st.sock.recvInt32();
        uint32_t handle = st.sock.recvInt32();
        uint32_t activeProtocol = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteContext)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteContext", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(!context || context != st.context)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localContext: 0x%08" PRIx32, __FUNCTION__, st.id(), context);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        auto readerName = std::string(reader.begin(),
                                      std::find(reader.begin(), reader.end(), 0));
        auto currentReader = findReaderState(readerName);

        if(! currentReader)
        {
            Application::error("%s: failed, reader not found: `%s'", __FUNCTION__, readerName.c_str());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteContext: 0x%016" PRIx64 ", shareMode: %" PRIu32 ", prefferedProtocols: %" PRIu32 ", reader: `%s'",
            __FUNCTION__, st.id(), st.remoteContext, shareMode, prefferedProtocols, readerName.c_str());
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Connect);
        ltsm->sendIntLE64(st.remoteContext).sendIntLE32(shareMode).sendIntLE32(prefferedProtocols);
        ltsm->sendIntLE32(readerName.size()).sendString(readerName);
        ltsm->sendFlush();
        // wait
        auto remoteHandle = ltsm->recvIntLE64();
        activeProtocol = ltsm->recvIntLE32();
        ret = ltsm->recvIntLE32();
        // make localHandle
        handle = ret != SCARD_S_SUCCESS ? 0 : Tools::crc32b((const uint8_t*) & remoteHandle, sizeof(remoteHandle));
        handle &= 0x7FFFFFFF;

        if(ret == SCARD_S_SUCCESS)
        {
            st.remoteHandle = remoteHandle;
            st.handle = handle;
            // sync reader
            std::scoped_lock guard{ readersLock };
            st.reader = currentReader;
            st.reader->share = shareMode;
            st.reader->protocol = activeProtocol;

            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> remoteHandle: 0x%016" PRIx64 ", localHandle: 0x%08" PRIx32
                               ", activeProtocol: %" PRIu32, __FUNCTION__, st.id(), remoteHandle, handle, activeProtocol);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteContext: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteContext, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(context).
        sendData(reader).
        sendInt32(shareMode).
        sendInt32(prefferedProtocols).
        sendInt32(handle).
        sendInt32(activeProtocol).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscReconnect(PcscClient & st, uint32_t len)
    {
        if(len != 24)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t shareMode = st.sock.recvInt32();
        uint32_t prefferedProtocols = st.sock.recvInt32();
        uint32_t initialization = st.sock.recvInt32();
        uint32_t activeProtocol = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", shareMode: %" PRIu32
            ", prefferedProtocols: %" PRIu32 ", inititalization: %" PRIu32,
            __FUNCTION__, st.id(), st.remoteHandle, shareMode, prefferedProtocols, initialization);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Reconnect);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(shareMode).sendIntLE32(prefferedProtocols).sendIntLE32(initialization);
        ltsm->sendFlush();
        // wait
        activeProtocol = ltsm->recvIntLE32();
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            assertm(st.reader, "reader not connected");
            std::scoped_lock guard{ readersLock };
            st.reader->share = shareMode;
            st.reader->protocol = activeProtocol;
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", shareMode: %" PRIu32 ", prefferedProtocols: %" PRIu32 ", inititalization: %" PRIu32 ", activeProtocol: %" PRIu32,
                __FUNCTION__, st.id(), handle, shareMode, prefferedProtocols, initialization, activeProtocol);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(shareMode).
        sendInt32(prefferedProtocols).
        sendInt32(initialization).
        sendInt32(activeProtocol).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscDisconnect(PcscClient & st, uint32_t len)
    {
        if(len != 12)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t disposition = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", disposition: %" PRIu32,
            __FUNCTION__, st.id(), st.remoteHandle, disposition);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Disconnect);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(disposition);
        ltsm->sendFlush();
        // wait
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            // sync after
            st.handle = 0;
            st.remoteHandle = 0;
            assertm(st.reader, "reader not connected");
            std::scoped_lock guard{ readersLock };
            st.reader->share = 0;
            st.reader->protocol = 0;
            st.reader = nullptr;
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", disposition: %" PRIu32,
                __FUNCTION__, st.id(), handle, disposition);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(disposition).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscBeginTransaction(PcscClient & st, uint32_t len)
    {
        if(len != 8)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64,
            __FUNCTION__, st.id(), st.remoteHandle);
        assertm(st.reader, "reader not connected");
        waitTransaction.readerLock(st.reader);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::BeginTransaction);
        ltsm->sendIntLE64(st.remoteHandle);
        ltsm->sendFlush();
        // wait
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32,
                __FUNCTION__, st.id(), handle);
        }
        else
        {
            // transaction failed
            waitTransaction.readerUnlock(st.reader);
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscEndTransaction(PcscClient & st, uint32_t len)
    {
        if(len != 12)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t disposition = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", disposition: %" PRIu32,
            __FUNCTION__, st.id(), st.remoteHandle, disposition);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::EndTransaction);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(disposition);
        ltsm->sendFlush();
        // wait
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", disposition: %" PRIu32,
                __FUNCTION__, st.id(), handle, disposition);
            // transaction ended
            assertm(st.reader, "reader not connected");
            waitTransaction.readerUnlock(st.reader);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(disposition).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscTransmit(PcscClient & st, uint32_t len)
    {
        if(len < 32)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t ioSendPciProtocol = st.sock.recvInt32();
        uint32_t ioSendPciLength = st.sock.recvInt32();
        uint32_t sendLength = st.sock.recvInt32();
        uint32_t ioRecvPciProtocol = st.sock.recvInt32();
        uint32_t ioRecvPciLength = st.sock.recvInt32();
        uint32_t recvLength = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();
        auto data = st.sock.recvData(sendLength);

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(28).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(28).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(28).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        if(sendLength != data.size())
        {
            Application::error("%s: clientId: %" PRId32 ", invalid length, send: %" PRIu32 ", data: %lu", __FUNCTION__, st.id(),
                               sendLength, data.size());
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", pciProtocol: 0x%08"
                           PRIx32 ", pciLength: %" PRIu32 ", send size: %" PRIu32,
                           __FUNCTION__, st.id(), st.remoteHandle, ioSendPciProtocol, ioSendPciLength, sendLength);

        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto str = Tools::buffer2hexstring(data.begin(), data.end(), 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: send data: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Transmit);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(ioSendPciProtocol).sendIntLE32(ioSendPciLength).sendIntLE32(sendLength);

        if(sendLength)
        {
            ltsm->sendData(data);
        }

        ltsm->sendFlush();
        // wait
        ioRecvPciProtocol = ltsm->recvIntLE32();
        ioRecvPciLength = ltsm->recvIntLE32();
        recvLength = ltsm->recvIntLE32();
        ret = ltsm->recvIntLE32();
        data = ltsm->recvData(recvLength);

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32
                ", pciProtocol: 0x%08" PRIx32 ", pciLength: %" PRIu32 ", recv size: %" PRIu32,
                 __FUNCTION__, st.id(), handle, ioRecvPciProtocol, ioRecvPciLength, recvLength);

            if(Application::isDebugLevel(DebugLevel::Trace))
            {
                auto str = Tools::buffer2hexstring(data.begin(), data.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: recv data: [ `%s' ]", __FUNCTION__, str.c_str());
            }
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }
    
        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(ioSendPciProtocol).
        sendInt32(ioSendPciLength).
        sendInt32(sendLength).
        sendInt32(ioRecvPciProtocol).
        sendInt32(ioRecvPciLength).
        sendInt32(recvLength).
        sendInt32(ret);

        if(recvLength)
        {
            st.sock.sendData(data);
        }

        st.sock.sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    void PcscSessionBus::pcscStatusApply(PcscClient & st, const std::string & name, uint32_t state, uint32_t protocol, const std::vector<uint8_t> & atr)
    {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " reader: `%s', state: %" PRIx32 ", protocol: %" PRIu32 ", atrLen: %" PRIu32,
            __FUNCTION__, st.id(), name.c_str(), state, protocol, atr.size());

        assertm(st.reader, "reader not connected");
        assertm(atr.size() <= sizeof(st.reader->atr), "atr length invalid");
        std::scoped_lock guard{ readersLock };

        // atr changed
        if(! std::equal(atr.begin(), atr.end(), std::begin(st.reader->atr)))
        {
            std::fill(std::begin(st.reader->atr), std::end(st.reader->atr), 0);
            std::copy_n(atr.data(), atr.size(), std::begin(st.reader->atr));

            st.reader->atrLen = atr.size();

            if(Application::isDebugLevel(DebugLevel::Trace))
            {
                auto str = Tools::buffer2hexstring(atr.begin(), atr.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: atr: [ `%s' ]", __FUNCTION__, str.c_str());
            }
        }

        // protocol changed
        if(protocol != st.reader->protocol)
        {
            st.reader->protocol = protocol;
        }

        if(state != st.reader->state)
        {
            st.reader->state = state;
        }
    }

    bool PcscSessionBus::pcscStatus(PcscClient & st, uint32_t len)
    {
        if(len != 8)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(len - 4).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(len - 4).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64,
            __FUNCTION__, st.id(), st.remoteHandle);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Status);
        ltsm->sendIntLE64(st.remoteHandle);
        ltsm->sendFlush();

        // wait
        uint32_t nameLen = ltsm->recvIntLE32();
        auto name = ltsm->recvString(nameLen);
        uint32_t state = ltsm->recvIntLE32();
        uint32_t protocol = ltsm->recvIntLE32();
        uint32_t atrLen = ltsm->recvIntLE32();
        auto atr = ltsm->recvData(atrLen);
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32,
                __FUNCTION__, st.id());

            pcscStatusApply(st, name, state, protocol, atr);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscControl(PcscClient & st, uint32_t len)
    {
        if(len < 24)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t controlCode = st.sock.recvInt32();
        uint32_t sendLength = st.sock.recvInt32();
        uint32_t recvLength = st.sock.recvInt32();
        uint32_t bytesReturned = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();
        auto data = st.sock.recvData(sendLength);

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(20).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(20).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(20).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        if(sendLength != data.size())
        {
            Application::error("%s: clientId: %" PRId32 ", invalid length, send: %" PRIu32 ", data: %lu", __FUNCTION__, st.id(),
                               sendLength, data.size());
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", controlCode: 0x%08"
             PRIx32 ", send size: %" PRIu32 ", recv size: %" PRIu32,
             __FUNCTION__, st.id(), st.remoteHandle, controlCode, sendLength, recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto str = Tools::buffer2hexstring(data.begin(), data.end(), 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: send data: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Control);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(controlCode).sendIntLE32(sendLength).sendIntLE32(recvLength);

        if(sendLength)
        {
            ltsm->sendData(data);
        }

        ltsm->sendFlush();
        // wait
        bytesReturned = ltsm->recvIntLE32();
        ret = ltsm->recvIntLE32();

        if(bytesReturned)
        {
            data = ltsm->recvData(bytesReturned);
        }
        else
        {
            data.clear();
        }

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", controlCode: 0x%08" PRIx32 ", bytesReturned: %" PRIu32,
             __FUNCTION__, st.id(), handle, controlCode, bytesReturned);

            if(Application::isDebugLevel(DebugLevel::Trace))
            {
                auto str = Tools::buffer2hexstring(data.begin(), data.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: recvLength: %" PRIu32 ", recv data: [ `%s' ]", __FUNCTION__, bytesReturned, str.c_str());
            }
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(controlCode).
        sendInt32(sendLength).
        sendInt32(recvLength).
        sendInt32(bytesReturned).
        sendInt32(ret);

        if(bytesReturned)
        {
            st.sock.sendData(data);
        }

        st.sock.sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscGetAttrib(PcscClient & st, uint32_t len)
    {
        if(len != 16 + MAX_BUFFER_SIZE)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t attrId = st.sock.recvInt32();
        auto attr = st.sock.recvData(MAX_BUFFER_SIZE);
        uint32_t attrLen = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(12 + MAX_BUFFER_SIZE).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(12 + MAX_BUFFER_SIZE).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(12 + MAX_BUFFER_SIZE).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", attrId: %" PRIu32,
            __FUNCTION__, st.id(), st.remoteHandle, attrId);
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::GetAttrib);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(attrId);
        ltsm->sendFlush();
        // wait
        attrLen = ltsm->recvIntLE32();
        ret = ltsm->recvIntLE32();
        assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");

        if(attrLen)
        {
            attr = ltsm->recvData(attrLen);
        }
        else
        {
            attr.clear();
        }

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", attrId: %" PRIu32 ", attrLen: %" PRIu32,
                __FUNCTION__, st.id(), handle, attrId, attrLen);

            if(Application::isDebugLevel(DebugLevel::Trace))
            {
                auto str = Tools::buffer2hexstring(attr.begin(), attr.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: attrLength: %" PRIu32 ", attr: [ `%s' ]", __FUNCTION__, attrLen, str.c_str());
            }
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(attrId);

        if(attrLen)
        {
            st.sock.sendData(attr);
        }

        st.sock.
        sendZero(MAX_BUFFER_SIZE - attrLen).
        sendInt32(attrLen).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscSetAttrib(PcscClient & st, uint32_t len)
    {
        if(len != 16 + MAX_BUFFER_SIZE)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t handle = st.sock.recvInt32();
        uint32_t attrId = st.sock.recvInt32();
        auto attr = st.sock.recvData(MAX_BUFFER_SIZE);
        uint32_t attrLen = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        if(! st.remoteHandle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, st.id());
            st.sock.sendZero(12 + MAX_BUFFER_SIZE).sendInt32(SCARD_F_INTERNAL_ERROR).sendFlush();
            return false;
        }

        if(handle != st.handle)
        {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, st.id(), handle);
            st.sock.sendZero(12 + MAX_BUFFER_SIZE).sendInt32(SCARD_E_INVALID_HANDLE).sendFlush();
            return false;
        }

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            st.sock.sendZero(12 + MAX_BUFFER_SIZE).sendInt32(SCARD_E_NO_SERVICE).sendFlush();
            return false;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle 0x%016" PRIx64 ", attrId: %" PRIu32 ", attrLength %" PRIu32,
             __FUNCTION__, st.id(), st.remoteHandle, attrId, attrLen);

        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto str = Tools::buffer2hexstring(attr.begin(), attr.end(), 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: attr: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::SetAttrib);
        ltsm->sendIntLE64(st.remoteHandle).sendIntLE32(attrId).sendIntLE32(attrLen);

        if(attrLen)
        {
            ltsm->sendData(attr);
        }

        ltsm->sendFlush();

        // wait
        ret = ltsm->recvIntLE32();

        if(ret == SCARD_S_SUCCESS)
        {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle 0x%08" PRIx32 ", attrId: %" PRIu32 ", attrLength %" PRIu32,
                 __FUNCTION__, st.id(), handle, attrId, attrLen);
        }
        else
        {
            Application::error("%s: clientId: %" PRId32 ", remoteHandle: 0x%016" PRIx64 ", error: 0x%08" PRIx32 " (%s)",
                __FUNCTION__, st.id(), st.remoteHandle, ret, PcscLite::err2str(ret));
        }

        // reply
        st.sock.
        sendInt32(handle).
        sendInt32(attrId).
        sendData(attr).
        sendInt32(attrLen).
        sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscSessionBus::pcscGetVersion(PcscClient & st, uint32_t len)
    {
        if(len != 12)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t versionMajor = st.sock.recvInt32();
        uint32_t versionMinor = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", protocol version: %" PRIu32 ".%" PRIu32, __FUNCTION__, st.id(),
                           versionMajor, versionMinor);
        st.sock.
        sendInt32(versionMajor).
        sendInt32(versionMinor).
        sendInt32(0).sendFlush();
        st.versionMajor = versionMajor;
        st.versionMinor = versionMinor;
        return true;
    }

    bool PcscSessionBus::pcscGetReaderState(PcscClient & st, uint32_t len)
    {
        const uint32_t readersLength = readers.size() * sizeof(PcscLite::ReaderState);
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", readers length: %" PRIu32, __FUNCTION__,
                           st.id(), st.context, readersLength);
        std::scoped_lock guard{ readersLock };
        st.sock.sendRaw(readers.data(), readersLength);
        st.sock.sendFlush();
        return true;
    }

    uint32_t waitReadersStatusChanged(PcscSessionBus* owner, PcscClient* st, uint32_t timeout)
    {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", timeout: %" PRIu32, __FUNCTION__, st->id(),
                           st->context, timeout);

        if(0 == timeout)
        {
            auto ret = owner->syncReaders(*st);
            return ret == SCARD_E_NO_READERS_AVAILABLE ?
                   SCARD_S_SUCCESS : ret;
        }

        std::this_thread::sleep_for(100ms);
        auto wait_ms = std::chrono::milliseconds(timeout);
        uint32_t ret = SCARD_E_TIMEOUT;
        Tools::Timeout timeoutLimit(wait_ms);
        Tools::Timeout timeoutSyncReaders(1s);
        st->waitStatusChanged.start();

        while(ret == SCARD_E_TIMEOUT)
        {
            if(st->waitStatusChanged.canceled)
            {
                ret = SCARD_E_CANCELLED;
                break;
            }

            if(st->waitStatusChanged.stopped)
            {
                ret = SCARD_S_SUCCESS;
                break;
            }

            if(timeoutLimit.check())
            {
                break;
            }

            if(timeoutSyncReaders.check())
            {
                bool readersChanged = false;
                auto ret2 = owner->syncReaders(*st, & readersChanged);

                if(ret2 != SCARD_S_SUCCESS || readersChanged)
                {
                    ret = ret2;
                    break;
                }
            }

            std::this_thread::sleep_for(100ms);
        }

        st->sock.sendInt32(timeout).sendInt32(ret).sendFlush();
        st->waitStatusChanged.reset();
        return ret;
    }

    bool PcscSessionBus::pcscReaderStateChangeStart(PcscClient & st, uint32_t len)
    {
        if(st.versionMajor == 4 && st.versionMinor < 3)
        {
            // old protocol: 4.2
            uint32_t timeout = st.sock.recvInt32();
            uint32_t ret = st.sock.recvInt32();
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", timeout: %" PRIu32, __FUNCTION__, st.id(),
                               st.context, timeout);
            st.waitStatusChanged.stop();
            st.waitStatusChanged.job = std::async(std::launch::async, & waitReadersStatusChanged, this, & st, timeout);
        }
        else if(st.versionMajor == 4 && st.versionMinor > 2)
        {
            // new protocol 4.4: empty params
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", timeout: %" PRIu32, __FUNCTION__, st.id(),
                               st.context);
            waitReadersStatusChanged(this, & st, 0);
            // send all readers
            const uint32_t readersLength = readers.size() * sizeof(PcscLite::ReaderState);
            std::scoped_lock guard{ readersLock };
            st.sock.sendRaw(readers.data(), readersLength);
            st.sock.sendFlush();
        }

        return true;
    }

    bool PcscSessionBus::pcscReaderStateChangeStop(PcscClient & st, uint32_t len)
    {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32, __FUNCTION__, st.id(), st.context);

        if(st.versionMajor == 4 && st.versionMinor < 3)
        {
            // old protocol: 4.2
            uint32_t timeout = st.sock.recvInt32();
            uint32_t ret = st.sock.recvInt32();
        }
        else if(st.versionMajor == 4 && st.versionMinor > 2)
        {
            // new protocol: 4.4, empty params
        }

        // stop
        st.waitStatusChanged.stop();
        st.sock.
        sendInt32(0).
        sendInt32(SCARD_S_SUCCESS).sendFlush();
        return true;
    }

    bool PcscSessionBus::pcscCancel(PcscClient & st, uint32_t len)
    {
        if(len != 8)
        {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, st.id(), len);
            return false;
        }

        uint32_t context = st.sock.recvInt32();
        uint32_t ret = st.sock.recvInt32();

        Application::debug(DebugType::Pcsc, "%s: << clientId: %" PRId32 ", remoteContext: 0x%016" PRIx64 ", context: 0x%08" PRIx32,
            __FUNCTION__, st.id(), st.remoteContext, context);

        const std::scoped_lock guard{ clientsLock };
        auto it = std::find_if(clients.begin(), clients.end(), [&](auto & cl)
        {
            return cl.context == context;
        });

        if(it != clients.end())
        {
            Application::debug(DebugType::Pcsc, "%s: found context: 0x%08" PRIx32 ", client Id: %" PRId32 ", cancelled", __FUNCTION__, context, it->id());
            it->waitStatusChanged.cancel();
            ret = SCARD_S_SUCCESS;
        }
        else
        {
            Application::error("%s: clientId: 0x%08" PRIx32 ", invalid context: 0x%08" PRIx32, __FUNCTION__, st.id(), context);
            ret = SCARD_E_INVALID_HANDLE;
        }

        st.sock.
        sendInt32(context).
        sendInt32(ret).sendFlush();
        return true;
    }

    std::list<std::string> PcscSessionBus::pcscListReaders(PcscClient & st)
    {
        if(! ltsm)
            return {};

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };

        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::ListReaders);
        ltsm->sendIntLE64(st.remoteContext);
        ltsm->sendFlush();

        // wait
        uint32_t readersCount = ltsm->recvIntLE32();

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", readers count: %" PRIu32, __FUNCTION__,
                           st.id(), st.context, readersCount);

        std::list<std::string> names;

        while(readersCount--)
        {
            uint32_t len = ltsm->recvIntLE32();
            names.emplace_back(ltsm->recvString(len));

            if(names.back().size() > MAX_READERNAME - 1)
            {
                names.back().resize(MAX_READERNAME - 1);
            }
        }

        return names;
    }

    int64_t PcscSessionBus::syncReaders(PcscClient & st, bool* changed)
    {
        Application::debug(DebugType::Pcsc, "%s", __FUNCTION__);

        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            return SCARD_E_NO_SERVICE;
        }

        auto names = pcscListReaders(st);

        if(names.empty())
        {
            Application::warning("%s: no readers available", __FUNCTION__);
            std::scoped_lock guard{ readersLock };

            // reset all readers
            for(auto & rd : readers)
            {
                if(rd.name[0])
                {
                    rd.reset();

                    if(changed)
                    {
                        *changed = true;
                    }
                }
            }

            return SCARD_E_NO_READERS_AVAILABLE;
        }

        // sync readers
        std::scoped_lock guard{ readersLock };

        for(auto & rd : readers)
        {
            if(0 == rd.name[0])
            {
                continue;
            }

            auto it = std::find_if(names.begin(), names.end(),
                                   [&](auto & name)
            {
                return 0 == name.compare(rd.name);
            });

            // not found, mark absent
            if(it == names.end())
            {
                // reset reader
                rd.reset();

                if(changed)
                {
                    *changed = true;
                }
            }
        }

        for(const auto & name : names)
        {
            auto it = std::find_if(readers.begin(), readers.end(),
                                   [&](auto & rd)
            {
                return 0 == name.compare(rd.name);
            });

            // not found, add new
            if(it == readers.end())
            {
                Application::debug(DebugType::Pcsc, "%s: added reader, name: `%s'", __FUNCTION__, name.c_str());
                // find unused slot
                auto rd = std::find_if(readers.begin(), readers.end(),
                                       [&](auto & rd)
                {
                    return 0 == rd.name[0];
                });

                if(rd == readers.end())
                {
                    Application::error("%s: failed, %s", __FUNCTION__, "all slots is busy");
                    return SCARD_E_NO_MEMORY;
                }

                rd->reset();
                syncReaderStatusChange(st, name, *rd, changed);
            }
        }

        return SCARD_S_SUCCESS;
    }

    int64_t PcscSessionBus::pcscGetStatusChange(PcscClient & st, uint32_t timeout, SCARD_READERSTATE* states,
            uint32_t statesCount)
    {
        if(! ltsm)
        {
            Application::error("%s: no service", __FUNCTION__);
            return SCARD_E_NO_SERVICE;
        }

        // multiple client to one socket order
        const std::scoped_lock guard{ ltsmLock };
        // send
        ltsm->sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::GetStatusChange);
        ltsm->sendIntLE64(st.remoteContext).sendIntLE32(timeout).sendIntLE32(statesCount);

        for(uint32_t it = 0; it < statesCount; ++it)
        {
            const SCARD_READERSTATE & state = states[it];
            ltsm->sendIntLE32(strnlen(state.szReader, MAX_READERNAME));
            ltsm->sendIntLE32(state.dwCurrentState);
            ltsm->sendIntLE32(state.cbAtr);
            ltsm->sendString(state.szReader);
            ltsm->sendRaw(state.rgbAtr, state.cbAtr);
        }

        ltsm->sendFlush();
        // wait
        uint32_t counts = ltsm->recvIntLE32();
        uint32_t ret = ltsm->recvIntLE32();
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", timeout: %" PRIu32 ", states: %" PRIu32,
                           __FUNCTION__, st.id(), st.context, timeout, counts);
        assertm(counts == statesCount, "count states invalid");

        for(uint32_t it = 0; it < statesCount; ++it)
        {
            SCARD_READERSTATE & state = states[it];
            state.dwCurrentState = ltsm->recvIntLE32();
            state.dwEventState = ltsm->recvIntLE32();

            uint32_t szReader = ltsm->recvIntLE32();
            uint32_t cbAtr = ltsm->recvIntLE32();

            std::string reader = ltsm->recvString(szReader);
            if(reader != state.szReader)
                Application::warning("%s: invalid reader, `%s' != `'", __FUNCTION__, reader.c_str(), state.szReader);

            assertm(cbAtr <= sizeof(state.rgbAtr), "atr length invalid");
            state.cbAtr = cbAtr;
            ltsm->recvData(state.rgbAtr, cbAtr);
        }

        return ret;
    }

    int PcscSessionBus::syncReaderStatusChange(PcscClient & st, const std::string & readerName, PcscLite::ReaderState & rd,
            bool* changed)
    {
        const uint32_t timeout = 0;
        SCARD_READERSTATE state = {};

        state.szReader = readerName.c_str();
        state.dwCurrentState = SCARD_STATE_UNAWARE;
        state.cbAtr = MAX_ATR_SIZE;

        auto ret = pcscGetStatusChange(st, timeout, & state, 1);

        if(ret == SCARD_E_TIMEOUT)
        {
            Application::warning("%s: timeout", __FUNCTION__);
            return ret;
        }

        if(ret != SCARD_S_SUCCESS)
        {
            Application::warning("%s: error: 0x%08" PRIx32 " (%s)", __FUNCTION__, ret, PcscLite::err2str(ret));
            return ret;
        }

        Application::debug(DebugType::Pcsc, "%s: reader: `%s', currentState: 0x%08" PRIx32 ", eventState: 0x%08" PRIx32 ", atrLen: %" PRIu32,
                           __FUNCTION__, readerName.c_str(), state.dwCurrentState, state.dwEventState, state.cbAtr);

        if(Application::isDebugLevel(DebugLevel::Trace))
        {
            auto str = Tools::buffer2hexstring(state.rgbAtr, state.rgbAtr + state.cbAtr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: atr: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        if(state.dwEventState & SCARD_STATE_CHANGED)
        {
            assertm(readerName.size() < sizeof(rd.name), "reader name invalid");
            assertm(state.cbAtr <= sizeof(rd.atr), "atr length invalid");
            rd.state = state.dwEventState & SCARD_STATE_PRESENT ? (PcscLite::StatePresent | PcscLite::StatePowered |
                       PcscLite::StateNegotiable) : PcscLite::StateAbsent;
            std::copy_n(readerName.data(), readerName.size(), rd.name);
            std::copy_n(state.rgbAtr, state.cbAtr, rd.atr);
            rd.atrLen = state.cbAtr;

            if(changed)
            {
                *changed = true;
            }
        }

        return SCARD_S_SUCCESS;
    }
}

int main(int argc, char** argv)
{
    bool debug = true;

    for(int it = 1; it < argc; ++it)
    {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
        {
            std::cout << "usage: " << argv[0] << " [--version] [--debug]" << std::endl;
            return EXIT_SUCCESS;
        }
        else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v"))
        {
            std::cout << "version: " << LTSM_PCSC2SESSION_VERSION << std::endl;
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
        LTSM::conn = sdbus::createSessionBusConnection(sdbus::ServiceName{LTSM::dbus_session_pcsc_name});
#else
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_pcsc_name);
#endif

        if(! LTSM::conn)
        {
            std::cerr << "dbus connection failed, uid: " << getuid() << std::endl;
            return EXIT_FAILURE;
        }

        LTSM::PcscSessionBus pcscSession(*LTSM::conn, debug);
        return pcscSession.start();
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
