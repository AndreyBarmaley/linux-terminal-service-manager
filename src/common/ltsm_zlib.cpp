/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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
 ***************************************************************************/

#include "ltsm_zlib.h"
#include "ltsm_tools.h"
#include "ltsm_application.h"

namespace LTSM::Tools {
    std::vector<uint8_t> zlibCompress(std::span<const uint8_t> cont) {
        const auto & data = reinterpret_cast<const Bytef*>(cont.data());
        const auto & size = cont.size();

        if(data && size) {
            uLong dstsz = ::compressBound(size);
            std::vector<uint8_t> res(dstsz);
            int ret = ::compress(reinterpret_cast<Bytef*>(res.data()), & dstsz, data, size);

            if(ret == Z_OK) {
                res.resize(dstsz);
                return res;
            } else {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "compress", ret);
                throw zlib_error(NS_FuncNameS);
            }
        }

        return {};
    }

    std::vector<uint8_t> zlibUncompress(std::span<const uint8_t> cont, size_t real) {
        auto data = std::data(cont);
        auto size = std::size(cont);

        if(data && size) {
            uLong dstsz = real ? real : size * 7;
            std::vector<uint8_t> res(dstsz);
            int ret = Z_BUF_ERROR;

            while(Z_BUF_ERROR ==
                  (ret = ::uncompress(reinterpret_cast<Bytef*>(res.data()), &dstsz,
                                      reinterpret_cast<const Bytef*>(data), size))) {
                dstsz = res.size() * 2;
                res.resize(dstsz);
            }

            if(ret == Z_OK) {
                res.resize(dstsz);
                return res;
            } else {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "uncompress", ret);
                throw zlib_error(NS_FuncNameS);
            }
        }

        return {};
    }
}

namespace LTSM::ZLib {
    InflateBase::InflateBase() {
        zs.data_type = Z_BINARY;

        if(int ret = inflateInit2(& zs, MAX_WBITS); ret != Z_OK) {
            Application::error("{}: {} failed, error code: {}", NS_FuncNameV, "inflateInit2", ret);
            throw zlib_error(NS_FuncNameS);
        }
    }

    InflateBase::~InflateBase() {
        inflateEnd(& zs);
    }

    std::vector<uint8_t> InflateBase::inflateData(const std::vector<uint8_t> & buf) {
        return inflateData(buf.data(), buf.size(), Z_SYNC_FLUSH);
    }

    void InflateBase::reset(void) {
        if(int ret = inflateReset(&zs); ret != Z_OK) {
            Application::error("{}: {} failed, error code: {}", NS_FuncNameV, "inflateReset", ret);
            throw zlib_error(NS_FuncNameS);
        }
    }

    /// flushPolicy: Z_NO_FLUSH, Z_SYNC_FLUSH, Z_FINISH, Z_BLOCK or Z_TREES
    std::vector<uint8_t> InflateBase::inflateData(const void* buf, size_t len, int flushPolicy) {

        if(len == 0) {
            Application::warning("{}: empty data", NS_FuncNameV);
            return {};
        }

        zs.next_in = (Bytef*) buf;
        zs.avail_in = static_cast<uInt>(len);

        std::vector<uint8_t> res;
        res.resize(len * 4);
        size_t write_pos = 0;

        while(0 < zs.avail_in) {
            if(write_pos >= res.size()) {
                res.resize(res.size() * 2);
            }

            zs.next_out = res.data() + write_pos;
            zs.avail_out = static_cast<uInt>(res.size() - write_pos);

            int ret = inflate(& zs, flushPolicy);

            const size_t bytes_written = (res.size() - write_pos) - zs.avail_out;
            write_pos += bytes_written;

            if(ret == Z_STREAM_END) {
                break;
            }

            if(ret == Z_BUF_ERROR) {
                continue;
            }

            if(ret != Z_OK) {
                Application::error("{}: {} failed, error code: {}", NS_FuncNameV, "inflate", ret);
                throw zlib_error(NS_FuncNameS);
            }
        }

        res.resize(write_pos);
        return res;
    }
}
