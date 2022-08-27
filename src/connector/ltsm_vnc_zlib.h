/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_VNC_ZLIB_
#define _LTSM_VNC_ZLIB_

#include <memory>
#include <vector>

#include <zlib.h>

#include "ltsm_sockets.h"

namespace LTSM
{
    namespace ZLib
    {
        struct Context : z_stream
        {
            std::vector<uint8_t> outbuf;
            
            Context();
            ~Context();
            
            std::vector<uint8_t> syncFlush(bool finish = false);
        };
        
        /// @brief: zlib compress output stream only (VNC version)
        class DeflateStream : public NetworkStream
        {
        protected:
            std::unique_ptr<Context> zlib;
            
        public:
            DeflateStream();
    
            std::vector<uint8_t> syncFlush(void) const;
            void                prepareSize(size_t) const;
            void                setLevel(size_t level) const;

            bool                hasInput(void) const override;
            size_t              hasData(void) const override;
            void                sendRaw(const void*, size_t) override;
            
        private:
            void                recvRaw(void*, size_t) const override;
            uint8_t             peekInt8(void) const override;
        };
    }
}

#endif // _LTSM__VNC_ZLIB_
