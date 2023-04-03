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

#ifndef _LIBRFB_SERVER_
#define _LIBRFB_SERVER_

#include <list>
#include <mutex>
#include <array>

#include "ltsm_librfb.h"
#include "librfb_encodings.h"

namespace LTSM
{
    namespace RFB
    {
        /// ServerEncoder
        class ServerEncoderBuf : public EncoderStream
        {
            std::vector<int>    clientEncodings;

            BinaryBuf           bufData;
            std::unique_ptr<EncoderWrapper> socket;
            std::unique_ptr<EncodingBase> encoder;

            PixelFormat         clientPf, serverPf;
            std::mutex          sendLock;

            mutable size_t      netStatRx = 0;
            mutable size_t      netStatTx = 0;

            NetworkStream*      streamIn = nullptr;
            NetworkStream*      streamOut = nullptr;

            bool                clientTrueColor = true;
            bool                clientBigEndian = false;

        protected:
            friend class EncodingBase;
            friend class EncodingRaw;
            friend class EncodingRRE;
            friend class EncodingHexTile;
            friend class EncodingTRLE;
            friend class EncodingZlib;

            // network stream interface
            void                sendFlush(void) override;
            void                sendRaw(const void* ptr, size_t len) override;
            void                recvRaw(void* ptr, size_t len) const override;
            bool                hasInput(void) const override;
            size_t              hasData(void) const override;
            uint8_t             peekInt8(void) const override;

            bool                isUpdateProcessed(void) const;
            void                waitUpdateProcess(void);

            void                recvPixelFormat(void);
            void                recvSetEncodings(void);
            void                recvKeyCode(void);
            void                recvPointer(void);
            void                recvCutText(void);
            void                recvFramebufferUpdate(void);
            void                recvSetContinuousUpdates(void);
            void                recvSetDesktopSize(void);

        public:
            ServerEncoderBuf(const PixelFormat &);

            const PixelFormat & clientFormat(void) const override { return clientPf; }
            const PixelFormat & serverFormat(void) const override { return serverPf; }
            bool                clientIsBigEndian(void) const override { return clientBigEndian; }

            void                sendFrameBufferUpdate(const FrameBuffer &);

            void                setEncodingDebug(int v);
            void                setEncodingThreads(int v);
            bool                serverSetClientEncoding(int type);

            const std::vector<uint8_t> & getBuffer(void) const;
            void                resetBuffer(void);
        };
    }
}

#endif // _LTSM_LIBRFB_
