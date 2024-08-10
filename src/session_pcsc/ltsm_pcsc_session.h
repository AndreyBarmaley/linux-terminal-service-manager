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

#include <thread>
#include <atomic>
#include <memory>
#include <future>
#include <string>
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
        uint32_t event = 0;       ///< number of card events
        uint32_t state = 0;       ///< SCARD_* bit field
        int32_t share = 0;        ///< PCSCLITE_SHARING_* sharing status
 
        uint8_t atr[MAX_ATR_SIZE];///< ATR
        uint32_t atrLen = 0;      ///< ATR length
        uint32_t protocol = 0;    ///< SCARD_PROTOCOL_* value

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
                job.get();
        }

        void cancel(void)
        {
            canceled = true;
            if(job.valid())
                job.get();
        }
    };

    struct PcscClient
    {
        SocketStream sock;
        PcscLite::ReaderState* reader = nullptr;

        std::thread thread;
        std::atomic<bool> shutdown{false};

        WaitStatus waitStatusChanged;

        uint64_t remoteContext = 0;
        uint64_t remoteHandle = 0;

        uint32_t versionMajor = 0;
        uint32_t versionMinor = 0;

        uint32_t context = 0;
        uint32_t handle = 0;

        PcscClient(int fd, PcscSessionBus* sessionBus);
        ~PcscClient();

        int id(void) const { return sock.fd(); }
    };

    class PcscSessionBus : public sdbus::AdaptorInterfaces<Session::PCSC_adaptor>, public Application
    {
        std::forward_list<PcscClient> clients;
        std::mutex clientsLock;

        std::array<PcscLite::ReaderState, PCSCLITE_MAX_READERS_CONTEXTS> readers;
        std::mutex readersLock;

        std::unique_ptr<SocketStream> ltsm;
        std::recursive_mutex ltsmLock;

        std::filesystem::path socketPath;
        int socketFd = -1;

    protected:
        bool pcscEstablishContext(PcscClient &, uint32_t len);
        bool pcscReleaseContext(PcscClient &, uint32_t len);
        bool pcscConnect(PcscClient &, uint32_t len);
        bool pcscReconnect(PcscClient &, uint32_t len);
        bool pcscDisconnect(PcscClient &, uint32_t len);
        bool pcscBeginTransaction(PcscClient &, uint32_t len);
        bool pcscEndTransaction(PcscClient &, uint32_t len);
        bool pcscTransmit(PcscClient &, uint32_t len);
        bool pcscStatus(PcscClient &, uint32_t len);
        bool pcscControl(PcscClient &, uint32_t len);
        bool pcscCancel(PcscClient &, uint32_t len);
        bool pcscGetAttrib(PcscClient &, uint32_t len);
        bool pcscSetAttrib(PcscClient &, uint32_t len);
        bool pcscGetVersion(PcscClient &, uint32_t len);
        bool pcscGetReaderState(PcscClient &, uint32_t len);
        bool pcscReaderStateChangeStart(PcscClient &, uint32_t len);
        bool pcscReaderStateChangeStop(PcscClient &, uint32_t len);

        int syncReaderStatusChange(PcscClient &, const std::string &, PcscLite::ReaderState &, bool* changed = nullptr);
        int64_t pcscGetStatusChange(PcscClient &, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount);

        std::list<std::string> pcscListReaders(PcscClient &);
        PcscLite::ReaderState* findReaderState(const std::string & name);

    public:
        PcscSessionBus(sdbus::IConnection &, std::string_view pcscSockName);
        virtual ~PcscSessionBus();

        int start(void) override;

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        bool connectChannel(const std::string & clientSocket) override;
        void disconnectChannel(const std::string & clientSocket) override;

        bool pcscClientAction(PcscClient &);

        int64_t syncReaders(PcscClient &, bool* changed = nullptr);
    };
}

#endif // _LTSM_PCSC_SESSION_
