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

#ifndef _LTSM_PCSC_SESSION_
#define _LTSM_PCSC_SESSION_

#include <tuple>
#include <thread>
#include <atomic>
#include <memory>
#include <future>
#include <string>
#include <vector>
#include <functional>
#include <forward_list>

#include "pcsclite.h"

#include "ltsm_pcsc.h"
#include "ltsm_streambuf.h"
#include "ltsm_application.h"
#include "ltsm_pcsc_adaptor.h"

namespace PcscLite
{
    // origin READER_STATE: PCSC/src/eventhandler.h
    struct ReaderState
    {
        char name[MAX_READERNAME];///< reader name
        uint32_t event = 0; ///< number of card events
        uint32_t state = 0; ///< SCARD_* bit field
        int32_t share = 0; ///< PCSCLITE_SHARING_* sharing status

        uint8_t atr[MAX_ATR_SIZE];///< ATR
        uint32_t atrLen = 0; ///< ATR length
        uint32_t protocol = 0; ///< SCARD_PROTOCOL_* value

        void reset(void);
    };
}

namespace LTSM
{
    class PcscSessionBus;

    struct WaitStatus
    {
        std::future<uint32_t> job;
        std::atomic<bool> stopped{true};
        std::atomic<bool> canceled{true};

        void reset(void)
        {
            stopped = true;
            canceled = true;
        }

        void start(void)
        {
            stopped = false;
            canceled = false;
        }

        void stop(void)
        {
            stopped = true;

            if(job.valid())
            {
                job.get();
            }
        }

        void cancel(void)
        {
            canceled = true;

            if(job.valid())
            {
                job.get();
            }
        }
    };

    using binary_buf = std::vector<uint8_t>;
 
    class PcscRemote
    {
        SocketStream sock;
        std::mutex sockLock;

    protected:

    public:
        PcscRemote(int fd) : sock(fd, false) {}

        std::tuple<uint64_t, uint32_t> sendEstablishedContext(const int32_t & id, const uint32_t & scope);
        std::tuple<uint32_t> sendReleaseContext(const int32_t & id, const uint64_t & context);
        std::tuple<uint64_t, uint32_t, uint32_t> sendConnect(const int32_t & id, const uint64_t & context, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const std::string & readerName);
        std::tuple<uint32_t, uint32_t> sendReconnect(const int32_t & id, const uint64_t & handle, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const uint32_t & initialization);
        std::tuple<uint32_t> sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition);
        std::tuple<uint32_t> sendBeginTransaction(const int32_t & id, const uint64_t & handle);
        std::tuple<uint32_t> sendEndTransaction(const int32_t & id, const uint64_t & handle, const uint32_t & disposition);
        std::tuple<uint32_t, uint32_t, uint32_t, binary_buf> sendTransmit(const int32_t & id, const uint64_t & handle, const uint32_t & ioSendPciProtocol, const uint32_t & ioSendPciLength, const uint32_t & recvLength, const binary_buf & data);
        std::tuple<std::string, uint32_t, uint32_t, uint32_t, binary_buf> sendStatus(const int32_t & id, const uint64_t & handle);
        std::tuple<uint32_t, binary_buf> sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1);
        std::tuple<uint32_t, binary_buf> sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId);
        std::tuple<uint32_t> sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr);
        std::tuple<uint32_t> sendCancel(const int32_t & id, const uint64_t & context);

        uint32_t sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount);
        std::list<std::string> sendListReaders(const int32_t & id, const uint64_t & context);
    };

    class PcscLocal
    {
        SocketStream sock;
        PcscLite::ReaderState* reader = nullptr;

        std::thread thread;
        WaitStatus waitStatusChanged;

        uint64_t remoteContext = 0;
        uint64_t remoteHandle = 0;

        uint32_t context = 0;
        uint32_t handle = 0;

        std::weak_ptr<PcscRemote> remote;
        std::function<uint64_t(uint32_t)> clientCanceledCb;

    protected:
        void replyError(uint32_t len, uint32_t err);

        bool proxyEstablishContext(void);
        bool proxyReleaseContext(void);
        bool proxyConnect(void);
        bool proxyReconnect(void);
        bool proxyDisconnect(void);
        bool proxyBeginTransaction(void);
        bool proxyEndTransaction(void);
        bool proxyTransmit(void);
        bool proxyStatus(void);
        bool proxyControl(void);
        bool proxyGetAttrib(void);
        bool proxySetAttrib(void);
        bool proxyCancel(void);
        bool proxyGetVersion(void);
        bool proxyGetReaderState(void);
        bool proxyReaderStateChangeStart(void);
        bool proxyReaderStateChangeStop(void);

        bool clientAction(void);
        void statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr);

        uint32_t syncReaderStatus(const std::string &, PcscLite::ReaderState &, bool* changed = nullptr);
        uint32_t syncReaders(bool* changed = nullptr);
        uint32_t waitReadersStatusChanged(uint32_t timeout);

    public:
        PcscLocal(int fd, const std::shared_ptr<PcscRemote> &, PcscSessionBus* sessionBus);
        ~PcscLocal();

        inline int id(void) const { return sock.fd(); }

        const uint64_t & proxyContext(void) const { return remoteContext; }
        const uint64_t & proxyHandle(void) const { return remoteHandle; }

        const uint32_t & localContext(void) const { return context; }
        const uint32_t & localHandle(void) const { return handle; }

        void canceledAction(void);
    };

    class PcscSessionBus : public ApplicationLog, public sdbus::AdaptorInterfaces<Session::Pcsc_adaptor>
    {
        std::forward_list<PcscLocal> clients;
        std::mutex clientsLock;

        std::shared_ptr<PcscRemote> remote;
        std::filesystem::path pcscSocketPath;

    public:
        PcscSessionBus(sdbus::IConnection &, bool debug = false);
        virtual ~PcscSessionBus();

        int start(void) override;

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        bool connectChannel(const std::string & sock) override;
        void disconnectChannel(const std::string & sock) override;

        void clientShutdownNotify(const PcscLocal*);
        uint64_t clientCanceledNotify(uint32_t ctx);
    };
}

#endif // _LTSM_PCSC_SESSION_
