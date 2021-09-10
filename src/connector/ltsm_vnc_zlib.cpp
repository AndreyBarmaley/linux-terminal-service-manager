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

#include "ltsm_application.h"
#include "ltsm_vnc_zlib.h"

namespace LTSM
{
    namespace ZLib
    {
        Context::Context()
        {
            zalloc = 0;
            zfree = 0;
            opaque = 0;
            total_in = 0;
            total_out = 0;
            avail_in = 0;
            next_in = 0;
            avail_out = 0;
            next_out = 0;
            data_type = Z_BINARY;

            outbuf.reserve(4 * 1024);
        }

        Context::~Context()
        {
            deflateEnd(this);
        }

        std::vector<uint8_t> Context::syncFlush(bool finish)
        {
            next_in = outbuf.data();
            avail_in = outbuf.size();

            std::vector<uint8_t> zip(deflateBound(this, outbuf.size()));
            next_out = zip.data();
            avail_out = zip.size();
    
            int prev = total_out;
            int ret = deflate(this, finish ? Z_FINISH : Z_SYNC_FLUSH);
            if(ret < Z_OK)
                Application::error("zlib: deflate error: %d", ret);
        
            size_t zipsz = total_out - prev;
            zip.resize(zipsz);
        
            outbuf.clear();
            next_out = nullptr;
            avail_out = 0;
    
            return zip;
        }

        /* Zlib::DeflateStream */
        DeflateStream::DeflateStream()
        {
            auto ptr = new Context();
            zlib.reset(ptr);

            int ret = deflateInit2(ptr, Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
            if(ret < Z_OK)
            {
                Application::error("zlib: deflateInit error: %d", ret);
                throw std::invalid_argument("zlib stream init failed");
            }
        }

	void DeflateStream::prepareSize(size_t len)
	{
	    if(len < zlib->outbuf.capacity()) zlib->outbuf.reserve(len);
	}

        std::vector<uint8_t> DeflateStream::syncFlush(void) const
        {
            return zlib->syncFlush();
        }

        void DeflateStream::sendRaw(const void* ptr, size_t len)
        {
            auto buf = reinterpret_cast<const uint8_t*>(ptr);
            zlib->outbuf.insert(zlib->outbuf.end(), buf, buf + len);
        }

        void DeflateStream::recvRaw(void* ptr, size_t len) const
        {
            Application::error("zlib: %s", "recv disabled");
	    throw std::runtime_error("zlib deflate: disable recv");
        }

        bool DeflateStream::hasInput(void) const
        {
            Application::error("zlib: %s", "has input disabled");
	    throw std::runtime_error("zlib deflate: check");
        }

        uint8_t DeflateStream::peekInt8(void) const
        {
            Application::error("zlib: %s", "peek disabled");
	    throw std::runtime_error("zlib deflate: peek");
        }
    }
} // LTSM
