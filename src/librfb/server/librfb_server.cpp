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

#include <cmath>
#include <string>
#include <chrono>
#include <cstring>
#include <fstream>
#include <algorithm>

#include <boost/endian.hpp>

#include "ltsm_zlib.h"
#include "librfb_server.h"
#include "ltsm_application.h"
#include "ltsm_boost_socket.h"

#ifdef LTSM_WITH_GSSAPI
#include "ltsm_gsslayer.h"
#endif

#ifdef LTSM_ENCODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

#include <boost/system/system_error.hpp>
#include <boost/asio/ssl/stream.hpp>

using namespace boost;
using namespace std::chrono_literals;

namespace LTSM {
    // ClintEncodings
    void ClientEncodings::setPriority(const std::vector<int> & priorities) {
        std::erase_if(encs, [&](auto & enc) {
            return std::ranges::any_of(priorities, [&](auto & val) { return val == enc; });
        });

        encs.insert(encs.begin(), priorities.begin(), priorities.end());
    }

    std::vector<int> ClientEncodings::toVector(void) const {
        return std::vector<int>{ encs.begin(), encs.end() };
    }

    bool ClientEncodings::isPresent(int type) const {
        return std::ranges::any_of(encs, [=](auto & val) { return val == type; });
    }

    int ClientEncodings::findPriorityFrom(std::initializer_list<int> priorities) const {
        for(const auto & enc : encs) {
            if(std::ranges::any_of(priorities,[&](auto & val) { return val == enc; })) {
                return enc;
            }
        }

        return RFB::ENCODING_RAW;
    }

    // ServerEncoder
    RFB::ServerEncoder::ServerEncoder(const boost::asio::any_io_executor& ctx)
        : ChannelListener(ctx), rfb_strand_{ctx}, xcb_strand_{ctx}, timer_updates_{ctx}, send_lock_{ctx} {
        stream_ = std::make_unique<AsyncTcpStream>(rfb_strand_);
    }

    void RFB::ServerEncoder::assignSocket(asio::ip::tcp::socket&& sock) {
        dynamic_cast<AsyncTcpStream&>(*stream_).socket() = std::move(sock);
    }

    void RFB::ServerEncoder::assignSocketFd(int fd) {
        dynamic_cast<AsyncTcpStream&>(*stream_).socket().assign(asio::ip::tcp::v4(), fd);
    }

    asio::awaitable<bool> RFB::ServerEncoder::authVncInitAwait(const std::string & passwdFile) {
        std::vector<uint8_t> challenge = Tools::randomBytes(16);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto tmp = Tools::hexString(challenge, 2);
            Application::debug(DebugType::Rfb, "{}: challenge: {}", NS_FuncNameV, tmp);
        }

        co_await stream_->async_send_buf(asio::buffer(challenge));
        auto response = co_await stream_->async_recv_buffer(16);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto tmp = Tools::hexString(response, 2);
            Application::debug(DebugType::Rfb, "{}: response: {}", NS_FuncNameV, tmp);
        }

        std::ifstream ifs(passwdFile, std::ifstream::in);

        while(ifs.good()) {
            std::string pass;
            std::getline(ifs, pass);
            auto crypt = OpenSSL::encryptDES(challenge, pass);

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto tmp = Tools::hexString(crypt, 2);
                Application::debug(DebugType::Rfb, "{}: encrypt: {}", NS_FuncNameV, tmp);
            }

            if(crypt == response) {
                co_return true;
            }
        }

        const std::string err("password mismatch");
        co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
        co_await stream_->async_send_be32(err.size());
        co_await stream_->async_send_buf(asio::buffer(err));
        Application::error("{}: {}, passwd file: {}", NS_FuncNameV, err, passwdFile);
        co_return false;
    }

    asio::awaitable<bool> RFB::ServerEncoder::authVenCryptInitAwait(const SecurityInfo & secInfo) {
        // VenCrypt version 0.2
        co_await stream_->async_send_byte(0);
        co_await stream_->async_send_byte(2);
        // client req
        const uint8_t majorVer = co_await stream_->async_recv_byte();
        const uint8_t minorVer = co_await stream_->async_recv_byte();
        Application::debug(DebugType::Rfb, "{}: client vencrypt version {}.{}", NS_FuncNameV, majorVer, minorVer);

        if(majorVer != 0 || (minorVer < 1 || minorVer > 2)) {
            // send unsupported
            co_await stream_->async_send_byte(255);
            Application::error("{}: unsupported vencrypt version {}.{}", NS_FuncNameV, majorVer, minorVer);
            co_return false;
        }

        // send supported
        co_await stream_->async_send_byte(0);
        bool x509Mode = false;

        if(minorVer == 1) {
            if(secInfo.tlsAnonMode) {
                co_await stream_->async_send_byte(1);
                co_await stream_->async_send_byte(RFB::SECURITY_VENCRYPT01_TLSNONE);
            } else {
                co_await stream_->async_send_byte(2);
                co_await stream_->async_send_byte(RFB::SECURITY_VENCRYPT01_TLSNONE);
                co_await stream_->async_send_byte(RFB::SECURITY_VENCRYPT01_X509NONE);
            }

            const uint8_t mode = co_await stream_->async_recv_byte();
            Application::debug(DebugType::Rfb, "{}: client choice vencrypt mode: {}", NS_FuncNameV, mode);

            switch(mode) {
                case RFB::SECURITY_VENCRYPT01_TLSNONE:
                    break;

                case RFB::SECURITY_VENCRYPT01_X509NONE:
                    if(secInfo.tlsAnonMode) {
                        Application::error("{}: unsupported vencrypt mode: {}", NS_FuncNameV, "x509");
                        co_return false;
                    }

                    x509Mode = true;
                    break;

                default:
                    Application::error("{}: unsupported vencrypt mode: {}", NS_FuncNameV, mode);
                    co_return false;
            }
        } else {
            // minorVer == 2
            if(secInfo.tlsAnonMode) {
                co_await stream_->async_send_byte(1);
                co_await stream_->async_send_be32(RFB::SECURITY_VENCRYPT02_TLSNONE);
            } else {
                co_await stream_->async_send_byte(2);
                co_await stream_->async_send_be32(RFB::SECURITY_VENCRYPT02_TLSNONE);
                co_await stream_->async_send_be32(RFB::SECURITY_VENCRYPT02_X509NONE);
            }

            const uint32_t mode = co_await stream_->async_recv_be32();
            Application::debug(DebugType::Rfb, "{}: client choice vencrypt mode: {}", NS_FuncNameV, mode);

            switch(mode) {
                case RFB::SECURITY_VENCRYPT02_TLSNONE:
                    break;

                case RFB::SECURITY_VENCRYPT02_X509NONE:
                    if(secInfo.tlsAnonMode) {
                        Application::error("{}: unsupported vencrypt mode: {}", NS_FuncNameV, "x509");
                        co_return false;
                    }

                    x509Mode = true;
                    break;

                default:
                    Application::error("{}: unsupported vencrypt mode: {}", NS_FuncNameV, mode);
                    co_return false;
            }
        }

        if(x509Mode) {
            const std::string* errFile = nullptr;
            std::error_code fserr;

            if(! std::filesystem::exists(secInfo.caFile, fserr)) {
                errFile = &secInfo.caFile;
            }

            if(! std::filesystem::exists(secInfo.certFile, fserr)) {
                errFile = &secInfo.certFile;
            }

            if(! std::filesystem::exists(secInfo.keyFile, fserr)) {
                errFile = &secInfo.keyFile;
            }

            if(errFile) {
                Application::error("{}: file not found: {}", NS_FuncNameV, errFile->c_str());
                co_await stream_->async_send_byte(0);
                co_return false;
            }
        }

        co_await stream_->async_send_byte(1);

        try {
            auto tcp_stream = std::move(static_cast<AsyncTcpStream&>(*stream_).socket());
            auto sock = std::make_unique<AsioTls::AsyncStream>(std::move(tcp_stream), asio::ssl::context::tlsv12_server);

            auto& ssl_ctx = sock->ssl_context();
            auto& ssl_stream = sock->ssl_stream();
            int verify_mode = asio::ssl::verify_none;
            // anonymous ciphers
            const char* ciphers = "aNULL:ADH:AECDH:@SECLEVEL=0";

            if(x509Mode) {
                //SSL_CTX_set_security_level(ssl_ctx.native_handle(), 1);
                ssl_ctx.set_default_verify_paths();

                if(! secInfo.caFile.empty()) {
                    ssl_ctx.load_verify_file(secInfo.caFile);
                }

                ssl_ctx.use_certificate_chain_file(secInfo.certFile);
                ssl_ctx.use_private_key_file(secInfo.keyFile, asio::ssl::context::pem);
                //verify_mode = asio::ssl::verify_peer;
                // FIXME: !!! secInfo.crlFile, secInfo.tlsPriority, true, secInfo.tlsDebug);
                ciphers = "AECDH-AES256-SHA:@SECLEVEL=1";
            }

            auto dh_buf = OpenSSL::generateDH2048();
            auto span = dh_buf.span();
            ssl_ctx.use_tmp_dh(asio::const_buffer(span.data(), span.size()));

            ssl_stream.set_verify_mode(verify_mode);

            sock->setCipherSuite(ciphers);
            sock->sslHandshake(AsioTls::HandshakeType::Server);
            Application::info("{}: {}", NS_FuncNameV, "TLS handshake success");
            stream_ = std::move(sock);
        } catch(system::system_error & err) {
            auto ec = err.code();
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            co_return false;
        }

        co_return true;
    }

    asio::awaitable<int> RFB::ServerEncoder::serverHandshakeVersionAwait(void) {
        // RFB 6.1.1 version
        int protover = RFB::VERSION_MAJOR * 10 + RFB::VERSION_MINOR;
        auto version = fmt::format("RFB {:03}.{:03}\n", RFB::VERSION_MAJOR, RFB::VERSION_MINOR);
        co_await stream_->async_send_buf(asio::buffer(version));
        std::string magick = co_await stream_->async_recv_string(12);
        Application::debug(DebugType::Rfb, "{}: handshake version {}", NS_FuncNameV, magick);

        if(magick == "RFB 003.003\n") {
            protover = 33;
        } else if(magick == "RFB 003.007\n") {
            protover = 37;
        } else if(magick != version) {
            Application::error("{}: handshake failure, unknown magic: {}", NS_FuncNameV, magick);
            co_return 0;
        }

        co_return protover;
    }

#ifdef LTSM_WITH_GSSAPI
    namespace GssWrapper {
        /// @brief: gss api server layer
        class Service : public Gss::ServiceContext {
            AsyncSocketBase & sock_;

          protected:
            // Gss::ServiceContext interface
            void error(std::string_view func, std::string_view subfunc, OM_uint32 code1, OM_uint32 code2) const override {
                auto err = Gss::error2str(code1, code2);
                Application::error("{}: {} failed, error: '{}', codes: [ {:#010x}, {:#010x}]", func, subfunc, err, code1, code2);
            }

            uint32_t recvLength(void) const {
                uint32_t val;
                sock_.sync_recv_buf(&val, sizeof(val));
                return boost::endian::big_to_native(val);
            }

            void sendLength(uint32_t val) {
                val = boost::endian::native_to_big(val);
                sock_.sync_recv_buf(&val, sizeof(val));
            }

          public:
            Service(AsyncSocketBase & sock) : sock_{sock} {
            }

            bool checkServiceCredential(std::string_view service) const {
                Gss::ErrorCodes err;
                if(auto cred = Gss::acquireServiceCredential(service, & err)) {
                    return true;
                }
                error(NS_FuncNameV, err.func, err.code1, err.code2);
                return false;
            }

            bool handshakeLayer(std::string_view service) {
                Gss::ErrorCodes err;
                if(auto cred = Gss::acquireServiceCredential(service, & err)) {
                    return acceptClient(std::move(cred));
                }
                error(NS_FuncNameV, err.func, err.code1, err.code2);
                return false;
            }

            std::vector<uint8_t> recvToken(void) const override {
                uint32_t len = recvLength();
                std::vector<uint8_t> buf(len, 0);
                sock_.sync_recv_buf(buf.data(), buf.size());
                return buf;
            }

            std::string recvTokenString(void) const {
                uint32_t len = recvLength();
                std::string buf(len, 0);
                sock_.sync_recv_buf(buf.data(), buf.size());
                return buf;
            }

            void sendToken(const void* buf, size_t len) override {
                sendLength(len);
                sock_.sync_send_buf(buf, len);
            }
        };
    }
#endif

    asio::awaitable<bool> RFB::ServerEncoder::serverSecurityInitAwait(int protover, const SecurityInfo & secInfo) {
        // RFB 6.1.2 security
        if(protover == 33) {
            uint32_t res = 0;

            if(secInfo.authVnc) {
                res |= RFB::SECURITY_TYPE_VNC;
            }

            if(secInfo.authNone) {
                res |= RFB::SECURITY_TYPE_NONE;
            }

            co_await stream_->async_send_be32(res);
        } else {
            std::vector<uint8_t> res;
#ifdef LTSM_WITH_GSSAPI
            res.push_back(SECURITY_TYPE_GSSAPI);
#endif

            if(secInfo.authVenCrypt) {
                res.push_back(RFB::SECURITY_TYPE_VENCRYPT);
            }

            if(secInfo.authVnc) {
                res.push_back(RFB::SECURITY_TYPE_VNC);
            }

            if(noVncMode()) {
                res.clear();
            }

            if(secInfo.authNone) {
                res.push_back(RFB::SECURITY_TYPE_NONE);
            }

            co_await stream_->async_send_byte(res.size());

            if(res.empty()) {
                Application::error("{}: server security invalid", NS_FuncNameV);
                co_return false;
            }

            co_await stream_->async_send_buf(asio::buffer(res));
        }

        // unsupported
        if(protover == 33) {
            co_return true;
        }

        int clientSecurity = co_await stream_->async_recv_byte();
        Application::debug(DebugType::Rfb, "{}, client security: {:#04x}", NS_FuncNameV, clientSecurity);

        if(protover == 38 || clientSecurity != RFB::SECURITY_TYPE_NONE) {
            // RFB 6.1.3 security result
            if(clientSecurity == RFB::SECURITY_TYPE_NONE && secInfo.authNone) {
                co_await stream_->async_send_be32(RFB::SECURITY_RESULT_OK);
            } else if(clientSecurity == RFB::SECURITY_TYPE_VNC && secInfo.authVnc) {
                if(secInfo.passwdFile.empty()) {
                    Application::error("{}: passwd file not defined", NS_FuncNameV);
                    co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
                    co_await stream_->async_send_be32(0);
                    co_return false;
                }

                std::error_code err;

                if(! std::filesystem::exists(secInfo.passwdFile, err)) {
                    Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                                        NS_FuncNameV, "exists", err.value(), err.message(), secInfo.passwdFile);
                    co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
                    co_await stream_->async_send_be32(0);
                    co_return false;
                }

                if(! co_await authVncInitAwait(secInfo.passwdFile)) {
                    co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
                    co_await stream_->async_send_be32(0);
                    co_return false;
                }

                co_await stream_->async_send_be32(RFB::SECURITY_RESULT_OK);
            } else if(clientSecurity == RFB::SECURITY_TYPE_VENCRYPT && secInfo.authVenCrypt) {
                if(! co_await authVenCryptInitAwait(secInfo)) {
                    co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
                    co_await stream_->async_send_be32(0);
                    co_return false;
                }

                co_await stream_->async_send_be32(RFB::SECURITY_RESULT_OK);
            }
#ifdef LTSM_WITH_GSSAPI
            else if(clientSecurity == SECURITY_TYPE_GSSAPI) {
                try {
                    auto krb = std::make_unique<GssWrapper::Service>(*stream_);
                    Application::info("{}: kerberos service: `{}'", NS_FuncNameV, secInfo.krb5Service);

                    if(krb->handshakeLayer(secInfo.krb5Service)) {
                        auto remoteName = Gss::displayName(krb->securityContext()->name);
                        std::unique_ptr<JsonObject> jo;

                        if(auto buf = krb->recvTokenString(); !buf.empty()) {
                            jo = std::make_unique<JsonObject>(JsonContentString(std::move(buf)).toObject());
                        }

                        // stop kerbero session
                        krb.reset();
                        Application::info("{}: kerberos auth: {}, remote: {}", NS_FuncNameV, "success", remoteName);

                        if(auto pos = remoteName.find("@"); pos != std::string::npos) {
                            clientAuthName_ = remoteName.substr(0, pos);
                            clientAuthDomain_ = remoteName.substr(pos + 1);
                        } else {
                            clientAuthName_ = remoteName;
                        }

                        // check json info
                        if(jo) {
                            auto tls = jo->getBoolean("continue:tls", false);

                            if(tls && ! co_await authVenCryptInitAwait(secInfo)) {
                                co_return false;
                            }
                        }

                        co_await stream_->async_send_be32(RFB::SECURITY_RESULT_OK);
                        co_return true;
                    }
                } catch(const std::exception & err) {
                    Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                }

                const std::string err("security kerberos failed");
                co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
                co_await stream_->async_send_be32(err.size());
                co_await stream_->async_send_buf(asio::buffer(err));
                Application::error("{}: error: {}", NS_FuncNameV, err);
                co_return false;
            }

#endif
            else {
                const std::string err("no matching security types");
                co_await stream_->async_send_be32(RFB::SECURITY_RESULT_ERR);
                co_await stream_->async_send_be32(err.size());
                co_await stream_->async_send_buf(asio::buffer(err));
                Application::error("{}: error: {}", NS_FuncNameV, err);
                co_return false;
            }
        }

        co_return true;
    }

    asio::awaitable<void> RFB::ServerEncoder::serverClientInitAwait(std::string_view desktopName, const XCB::Size & displaySize, int displayDepth,
            const PixelFormat & pf) {
        // RFB 6.3.1 client init
        int clientSharedFlag = co_await stream_->async_recv_byte();
        Application::debug(DebugType::Rfb, "{}: client shared: {:#04x}", NS_FuncNameV, clientSharedFlag);
        // RFB 6.3.2 server init
        Application::notice("{}: server pf - bpp: {}, depth: {}, bigendian: {}, red({:#010x}), green({:#010x}), blue({:#010x})",
                            NS_FuncNameV, pf.bitsPerPixel(), displayDepth, (int) platformBigEndian(),
                            pf.rmask(), pf.gmask(), pf.bmask());
        clientPf = serverFormat();
        StreamBuf sb(24 + desktopName.size());

        sb.writeIntBE16(displaySize.width).
            writeIntBE16(displaySize.height).
            // send pixel format
            writeInt8(pf.bitsPerPixel()).
            writeInt8(displayDepth).
            writeInt8(platformBigEndian() ? 1 : 0).
            // true color
            writeInt8(1).
            writeIntBE16(pf.rmax()).
            writeIntBE16(pf.gmax()).
            writeIntBE16(pf.bmax()).
            writeInt8(pf.rshift()).
            writeInt8(pf.gshift()).
            writeInt8(pf.bshift()).
            // send padding
            writeZero(3).
            // send name desktop
            writeIntBE32(desktopName.size()).
            write(desktopName);

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    void RFB::ServerEncoder::waitUpdateProcess(void) {
        while(fbUpdateProcessing_.load(std::memory_order_acquire)) {
            fbUpdateProcessing_.wait(true, std::memory_order_acquire); 
        }
    }

    asio::awaitable<void> RFB::ServerEncoder::waitUpdateProcessAwait(void) {
        while(fbUpdateProcessing_.load(std::memory_order_acquire)) {
            timer_updates_.expires_after(std::chrono::milliseconds(1));
            co_await timer_updates_.async_wait(asio::use_awaitable);
        }

        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendUpdateScreenAwait(const XCB::Region & area) const {
        if(! encoder_) {
            Application::warning("{}: encoder null", NS_FuncNameV);
            co_return;
        }

        try {
            fbUpdateProcessing_ = true;
            co_await asio::dispatch(xcb_strand_, asio::use_awaitable);
            auto reply = serverFrameBuffer(area);
            co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
            co_await sendFrameBufferUpdateAwait(reply.fb);

            asio::post(xcb_strand_, [this, area]() {
                serverSendFBUpdateEvent(area);
            });
        } catch(const xcb_error_busy&) {
            Application::warning("{}: update busy, area: {}", NS_FuncNameV, area);
        } catch(const std::exception & err) {
            fbUpdateProcessing_ = false;
            throw err;
        }

        fbUpdateProcessing_ = false;
        co_return;
    }

    void RFB::ServerEncoder::asioStop(void) {
        channelsShutdown();
        if(stream_) {
            stream_->closeSocket();
        }
        timer_updates_.cancel();
    }

    asio::awaitable<void> RFB::ServerEncoder::rfbMessageAwait(void) {

        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        int msgType = co_await stream_->async_recv_byte();

        switch(msgType) {
            case RFB::PROTOCOL_LTSM:
                co_await recvLtsmProtoAwait();
                break;

            case RFB::CLIENT_SET_PIXEL_FORMAT:
                co_await recvPixelFormatAwait();
                break;

            case RFB::CLIENT_SET_ENCODINGS:
                co_await recvSetEncodingsAwait();
                break;

            case RFB::CLIENT_REQUEST_FB_UPDATE:
                co_await recvFramebufferUpdateAwait();
                break;

            case RFB::CLIENT_EVENT_KEY:
                co_await recvKeyCodeAwait();
                break;

            case RFB::CLIENT_EVENT_POINTER:
                co_await recvPointerAwait();
                break;

            case RFB::CLIENT_CUT_TEXT:
                co_await recvCutTextAwait();
                break;

            case RFB::CLIENT_SET_DESKTOP_SIZE:
                co_await recvSetDesktopSizeAwait();
                break;

            case RFB::CLIENT_CONTINUOUS_UPDATES:
                co_await recvSetContinuousUpdatesAwait();
                break;

            default:
                Application::error("{}: unknown message: {:#04x}", NS_FuncNameV, msgType);
                throw rfb_error(NS_FuncNameS);
        }

        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvLtsmProtoAwait(void) {
        if(! clientLtsmSupported) {
            Application::error("{}: client not support encoding: {}", NS_FuncNameV, RFB::encodingName(RFB::ENCODING_LTSM));
            throw channel_error(NS_FuncNameS);
        }

        const uint8_t version = co_await stream_->async_recv_byte();

        if(version != LtsmProtocolVersion) {
            Application::error("{}: unknown version: {:#04x}", NS_FuncNameV, version);
            throw channel_error(NS_FuncNameS);
        }

        const uint8_t channel = co_await stream_->async_recv_byte();
        const uint16_t length = co_await stream_->async_recv_be16();
        auto buf = co_await stream_->async_recv_buffer(length);

        if(channelDebug == channel) {
            auto str = Tools::hexString(buf, 2);
            Application::trace(DebugType::Channels, "{}: id: {}, size: {}, content: [{}]",
                           NS_FuncNameV, channel, length, str);
        }

        ChannelListener::recvLtsmEvent(channel, std::move(buf));
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvPixelFormatAwait(void) {
        co_await waitUpdateProcessAwait();

        // RFB: 6.4.1
        auto buf = co_await stream_->async_recv_buffer(19);
        StreamBufRef sb(buf.data(), buf.size());

        // skip padding
        sb.skip(3);
        const uint8_t bitsPerPixel = sb.readInt8();
        const uint8_t depth = sb.readInt8();
        const uint8_t bigEndian = sb.readInt8();
        const uint8_t trueColor = sb.readInt8();
        const uint16_t redMax = sb.readIntBE16();
        const uint16_t greenMax = sb.readIntBE16();
        const uint16_t blueMax = sb.readIntBE16();
        const uint8_t redShift = sb.readInt8();
        const uint8_t greenShift = sb.readInt8();
        const uint8_t blueShift = sb.readInt8();
        // skip padding
        sb.skip(3);

        Application::debug(DebugType::Rfb, "{}: red({},{}), green({},{}), blue({},{})",
                            NS_FuncNameV, redMax, redShift, greenMax, greenShift, blueMax, blueShift);

        switch(bitsPerPixel) {
            case 32:
            case 16:
            case 8:
                break;

            default: {
                Application::error("{}: {}", NS_FuncNameV, " unknown pixel format");
                throw rfb_error(NS_FuncNameS);
            }
        }

        if(trueColor == 0 || redMax == 0 || greenMax == 0 || blueMax == 0) {
            Application::error("{}: {}", NS_FuncNameV, " unsupported pixel format");
            throw rfb_error(NS_FuncNameS);
        }

        clientTrueColor = trueColor;
        clientBigEndian = bigEndian;
        clientPf = PixelFormat(bitsPerPixel, redMax, greenMax, blueMax, 0, redShift, greenShift, blueShift, 0);

        Application::notice("{}: client pf - bpp: {}, depth: {}, bigendian: {}, red({:#010x}), green({:#010x}), blue({:#010x})",
                            NS_FuncNameV, bitsPerPixel, depth, (int) bigEndian,
                            clientPf.rmask(), clientPf.gmask(), clientPf.bmask());

        colourMap.clear();
        serverRecvPixelFormatEvent(clientPf, clientBigEndian);

        co_return;
    }

    bool RFB::ServerEncoder::clientIsBigEndian(void) const {
        return clientBigEndian;
    }

    const PixelFormat & RFB::ServerEncoder::clientFormat(void) const {
        return clientPf;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvSetEncodingsAwait(void) {
        co_await waitUpdateProcessAwait();

        // RFB: 6.4.2
        // skip padding
        [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_byte();
        uint16_t numEncodings = co_await stream_->async_recv_be16();
        Application::info("{}: encoding counts: {}", NS_FuncNameV, numEncodings);

        bool extendedClipboard = false;
        bool continueUpdates = false;
        auto disabledEncodings = serverDisabledEncodings();

        std::vector<int> recvEncodings;
        recvEncodings.reserve(numEncodings);

        while(0 < numEncodings--) {
            int encoding = co_await stream_->async_recv_be32();

            if(! disabledEncodings.empty()) {
                auto enclower = Tools::lower(RFB::encodingName(encoding));

                if(std::ranges::any_of(disabledEncodings, [&](auto & str) { return enclower == Tools::lower(str); })) {
                    Application::warning("{}: request encodings: {} (disabled)", NS_FuncNameV, RFB::encodingName(encoding));
                    continue;
                }
            }

            switch(encoding) {
                case RFB::ENCODING_LTSM:
                case RFB::ENCODING_LTSM_ZQOI:
                case RFB::ENCODING_LTSM_QOI:
                case RFB::ENCODING_LTSM_LZ4:
                case RFB::ENCODING_LTSM_TJPG:
                case RFB::ENCODING_LTSM_H264:
                case RFB::ENCODING_LTSM_MPEG4:
                case RFB::ENCODING_LTSM_CURSOR:
                    clientLtsmSupported = true;
                    break;

                case RFB::ENCODING_LTSM_KEYB:
                    clientLtsmSupported = true;
                    clientLtsmKeyboard = true;
                    break;

                case RFB::ENCODING_CONTINUOUS_UPDATES:
                    continueUpdates = true;
                    break;

                case RFB::ENCODING_EXT_CLIPBOARD:
                    extendedClipboard = true;
                    break;

                default:
                    break;
            }

            recvEncodings.push_back(encoding);
            const char* name = RFB::encodingName(encoding);

            if(0 == std::strcmp(name, "unknown")) {
                Application::info("{}: request encodings: {:#010x}", NS_FuncNameV, encoding);
            } else {
                Application::info("{}: request encodings: {}", NS_FuncNameV, RFB::encodingName(encoding));
            }
        }

        clientEncodings_.setPriority(recvEncodings);

        if(continueUpdates) {
            asio::co_spawn(rfb_strand(), sendContinuousUpdatesAwait(true), asio::detached);
        }

        if(extendedClipboard) {
            // The server must send a ServerCutText message with caps set on
            // each SetEncodings message received which includes the Extended Clipboard pseudo-encoding.

            // The client may send a ClientCutText message with caps set back to indicate its capabilities.
            // Otherwise the client is assumed to support text, rtf, html, request, notify and provide and a maximum size of 20 MiB for text and 0 bytes for the other types.

            setExtClipboardRemoteCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                                      ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);

            setExtClipboardLocalCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                                     ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);

            ExtClip::remoteExtClipTypeTextSz = 20 * 1024 * 1024;
            asio::post(xcb_strand(), std::bind(&ServerEncoder::sendExtClipboardCaps, this));
        }

        serverRecvSetEncodingsEvent(recvEncodings);
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvFramebufferUpdateAwait(void) {
        XCB::Region clientRegion;
        // RFB: 6.4.3
        const bool incremental = co_await stream_->async_recv_byte();
        clientRegion.x = co_await stream_->async_recv_be16();
        clientRegion.y = co_await stream_->async_recv_be16();
        clientRegion.width = co_await stream_->async_recv_be16();
        clientRegion.height = co_await stream_->async_recv_be16();
        Application::debug(DebugType::Rfb, "{}: request update, region: {}, incremental: {}",
                           NS_FuncNameV, clientRegion, incremental);

        serverRecvFBUpdateEvent(incremental, clientRegion);
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvKeyCodeAwait(void) {
        if(clientLtsmKeyboard) {
            const bool pressed = co_await stream_->async_recv_byte();
            const uint16_t scancode = co_await stream_->async_recv_be16();
            const uint32_t keycode = co_await stream_->async_recv_be32();
            Application::debug(DebugType::Rfb, "{}: action {}, keysym: {:#010x}, scancode: {:#06x}", NS_FuncNameV, (pressed ? "pressed" : "released"), keycode, scancode);
            asio::post(xcb_strand_, [this, pressed, keycode, scancode]() {
                serverRecvKeyEvent(pressed, keycode, scancode);
            });
        } else {
            // RFB: 6.4.4
            const bool pressed = co_await stream_->async_recv_byte();
            [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_be16();
            const uint32_t keycode = co_await stream_->async_recv_be32();
            Application::debug(DebugType::Rfb, "{}: action {}, keysym: {:#010x}", NS_FuncNameV, (pressed ? "pressed" : "released"), keycode);
            asio::post(xcb_strand_, [this, pressed, keycode]() {
                serverRecvKeyEvent(pressed, keycode, 0);
            });
        }

        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvPointerAwait(void) {
        // RFB: 6.4.5
        // left 0x01, middle 0x02, right 0x04, scrollUp: 0x08, scrollDn: 0x10, scrollLf: 0x20, scrollRt: 0x40, back: 0x80
        const uint8_t buttons = co_await stream_->async_recv_byte();
        const uint16_t posx = co_await stream_->async_recv_be16();
        const uint16_t posy = co_await stream_->async_recv_be16();
        Application::debug(DebugType::Rfb, "{}: mask: {:#04x}, pos: [ {}, {}]", NS_FuncNameV, buttons, posx, posy);

        asio::post(xcb_strand_, [this, buttons, posx, posy]() {
            serverRecvPointerEvent(buttons, posx, posy);
        });
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvCutTextAwait(void) {
        // RFB: 6.4.6
        // skip padding
        [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_byte();
        [[maybe_unused]] const auto pad2 = co_await stream_->async_recv_byte();
        [[maybe_unused]] const auto pad3 = co_await stream_->async_recv_byte();

        // A negative value of length indicates that the extended message format is used and abs(length) is the total number of following bytes.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        const int32_t length = co_await stream_->async_recv_be32();

        if(0 < length) {
            Application::debug(DebugType::Rfb, "{}: text length: {}, limit: {}", NS_FuncNameV, length, localExtClipTypeTextSz);
            size_t recv = localExtClipTypeTextSz ?
                          std::min(static_cast<uint32_t>(length), localExtClipTypeTextSz) : length;
            auto buffer = co_await stream_->async_recv_buffer(recv);
            [[maybe_unused]] auto skip = co_await stream_->async_recv_buffer(length - recv);

            asio::post(xcb_strand_, [this, buf=std::move(buffer)]() mutable {
                serverRecvCutTextEvent(std::move(buf));
            });
        } else if(length < 0) {
            if(0 == extClipboardLocalCaps()) {
                Application::error("{}: invalid format, failed `{}'", NS_FuncNameV, "ext clipboard");
                throw rfb_error(NS_FuncNameS);
            }

            auto buffer = co_await stream_->async_recv_buffer(std::abs(length));

            co_await asio::dispatch(xcb_strand_, asio::use_awaitable);
            co_await recvExtClipboardCapsAwait(buffer);
        }

        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvSetContinuousUpdatesAwait(void) {
        const bool enable = co_await stream_->async_recv_byte();
        XCB::Region reg;
        reg.x = co_await stream_->async_recv_be16();
        reg.y = co_await stream_->async_recv_be16();
        reg.width = co_await stream_->async_recv_be16();
        reg.height = co_await stream_->async_recv_be16();

        Application::info("{}: region: {}, enabled: {}", NS_FuncNameV, reg, enable);
        continueUpdatesProcessed = enable;

        serverRecvSetContinuousUpdatesEvent(enable, reg);
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::recvSetDesktopSizeAwait(void) {
        // skip padding (one byte!)
        [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_byte();
        const uint16_t width = co_await stream_->async_recv_be16();
        const uint16_t height = co_await stream_->async_recv_be16();
        const int numOfScreens = co_await stream_->async_recv_byte();
        [[maybe_unused]] const auto pad2 = co_await stream_->async_recv_byte();

        Application::info("{}: size: {}, screens: {}", NS_FuncNameV, XCB::Size(width, height), numOfScreens);

        auto buf = co_await stream_->async_recv_buffer(16 * numOfScreens);
        StreamBufRef sb(buf.data(), buf.size());

        // screens array
        std::vector<RFB::ScreenInfo> screens;

        for(int it = 0; it < numOfScreens; it++) {
            RFB::ScreenInfo info;
            info.id = sb.readIntBE32();
            info.x = sb.readIntBE16();
            info.y = sb.readIntBE16();
            info.width = sb.readIntBE16();
            info.height = sb.readIntBE16();
            info.flags = sb.readIntBE32();
            screens.emplace_back(info);
        }

        asio::co_spawn(xcb_strand_, [this, screens=std::move(screens)]() mutable -> asio::awaitable<void> {
            co_await waitUpdateProcessAwait();
            serverRecvDesktopSizeEvent(std::move(screens));
            co_return;
        }, asio::detached);
        co_return;
    }

    void RFB::ServerEncoder::displayResizeEvent(const XCB::Size & dsz) {
        Application::info("{}: display resized, new size: {}", NS_FuncNameV, dsz);
#ifdef LTSM_ENCODING_FFMPEG
        // event background
        if(isEncoderFFmpeg()) {
            asio::dispatch(rfb_strand_, [this, sz = dsz]() {
                this->encoder_->resizedEvent(sz);
            });
        }
#endif
    }

    void RFB::ServerEncoder::clientDisconnectedEvent(int display) {
        Application::warning("{}: display: {}", NS_FuncNameV, display);
    }

    asio::awaitable<void> RFB::ServerEncoder::sendColourMapAwait(int first) const {
        Application::info("{}: first: {}, colour map length: {}", NS_FuncNameV, first, colourMap.size());

        StreamBuf sb(6 + colourMap.size() * 6);
        // RFB: 6.5.2
        sb.writeInt8(RFB::SERVER_SET_COLOURMAP).
            // padding
            writeInt8(0).
            // first color
            writeIntBE16(first).
            writeIntBE16(colourMap.size());

        for(const auto & col : colourMap) {
            sb.writeIntBE16(col.r).
                writeIntBE16(col.g).
                writeIntBE16(col.b);
        }

        co_await send_lock_.async_lock();
        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendBellEventAwait(void) const {
        Application::info("{}: process", NS_FuncNameV);
        // RFB: 6.5.3
        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_byte(RFB::SERVER_BELL);
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendCutTextAwait(std::span<const uint8_t> buf, bool ext) const {
        StreamBuf sb(8 + buf.size());

        // RFB: 6.5.4
        sb.writeInt8(RFB::SERVER_CUT_TEXT).
            // padding
            writeZero(3); 

        if(ext) {
            // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
            if(0 == extClipboardRemoteCaps()) {
                Application::error("{}: invalid format, failed `{}'", NS_FuncNameV, "ext clipboard");
                throw rfb_error(NS_FuncNameS);
            }

            // A negative value of length indicates that the extended message format
            // is used and abs(length) is the total number of following bytes.
            const uint32_t length = static_cast<uint32_t>(0xFFFFFFFF) - buf.size() + 1;
            Application::debug(DebugType::Rfb, "{}: length text: {}", NS_FuncNameV, length);
            sb.writeIntBE32(length);
        } else {
            Application::debug(DebugType::Rfb, "{}: length text: {}", NS_FuncNameV, buf.size());
            sb.writeIntBE32(buf.size());
        }

        sb.write(buf);

        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendContinuousUpdatesAwait(bool enable) const {
        // RFB: 6.5.5
        Application::info("{}: status: {}", NS_FuncNameV, (enable ? "enable" : "disable"));
        co_await send_lock_.async_lock();
        co_await stream_->async_send_byte(RFB::SERVER_CONTINUOUS_UPDATES);
        send_lock_.unlock();
        continueUpdatesProcessed = enable;
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendFrameBufferUpdateAwait(const FrameBuffer & fb) const {
        auto & reg = fb.region();
        Application::debug(DebugType::Rfb, "{}: region: {}", NS_FuncNameV, reg);

        auto packets = encoder_->getFrameBufferPackets(this, fb);

        // add header
        BinaryBuf header(4, 0);
        // RFB: 6.5.1
        header[0] = RFB::SERVER_FB_UPDATE;
        // padding
        header[1] = 0;
        // regions: be16
        endian::store_big_u16(std::addressof(header[2]), packets.size());
        // added header
        packets.emplace_front(std::move(header));

        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        for(const auto & pkt: packets) {
            co_await stream_->async_send_buf(asio::buffer(pkt));
        }
        send_lock_.unlock();
        co_return;
    }

    std::string RFB::ServerEncoder::serverEncryptionInfo(void) const {
        if(auto stream = dynamic_cast<AsioTls::AsyncStream*>(stream_.get())) {
            auto& ssl_st = stream->ssl_stream();
            return OpenSSL::streamDescription(ssl_st.native_handle());
        }

        return "none";
    }

    bool RFB::ServerEncoder::isContinueUpdatesProcessed(void) const {
        return continueUpdatesProcessed;
    }

    bool RFB::ServerEncoder::isClientSupportedEncoding(int enc) const {
        return clientEncodings_.isPresent(enc);
    }

    int RFB::serverSelectCompatibleEncoding(const ClientEncodings & clientEncodings_) {
        // server priority
        std::initializer_list<int> encs = {
#ifdef LTSM_ENCODING_FFMPEG
            RFB::ENCODING_LTSM_H264,
            RFB::ENCODING_LTSM_MPEG4,
#endif
            RFB::ENCODING_LTSM_ZQOI,
            RFB::ENCODING_LTSM_QOI,
            RFB::ENCODING_LTSM_LZ4,
            RFB::ENCODING_LTSM_TJPG,
            // compat
            RFB::ENCODING_ZRLE, RFB::ENCODING_TRLE, RFB::ENCODING_ZLIB, RFB::ENCODING_HEXTILE,
            RFB::ENCODING_CORRE, RFB::ENCODING_RRE, RFB::ENCODING_RAW
        };

        return clientEncodings_.findPriorityFrom(encs);
    }

    void RFB::ServerEncoder::serverSelectEncodings(void) {
        int compatible = serverSelectCompatibleEncoding(clientEncodings_);

        if(encoder_ && encoder_->getType() == compatible) {
            return;
        }

        if((compatible == RFB::ENCODING_LTSM_QOI ||
            compatible == RFB::ENCODING_LTSM_ZQOI) && 24 != serverFormat().depth()) {
            const int change = RFB::ENCODING_LTSM_LZ4;
            Application::notice("{}: server bpp({}), {} not supported, change to: {}",
                NS_FuncNameV, serverFormat().bytePerPixel(), RFB::encodingName(compatible), RFB::encodingName(change));
            compatible = change;
        }

        switch(compatible) {
            case RFB::ENCODING_RAW:
                encoder_ = std::make_unique<EncodingRaw>();
                break;

            case RFB::ENCODING_ZLIB: {
                auto clevels = { ENCODING_COMPRESS1, ENCODING_COMPRESS2, ENCODING_COMPRESS3, ENCODING_COMPRESS4, ENCODING_COMPRESS5, ENCODING_COMPRESS6, ENCODING_COMPRESS7, ENCODING_COMPRESS8, ENCODING_COMPRESS9 };
                int zlevel = Z_BEST_SPEED;

                if(auto it = std::ranges::find_if(clevels, [this](auto & enc) {
                return this->isClientSupportedEncoding(enc);
                }); it != clevels.end()) {
                    zlevel = ENCODING_COMPRESS1 - *it + Z_BEST_SPEED;
                }

                encoder_ = std::make_unique<EncodingZlib>(zlevel);
                break;
            }

            case RFB::ENCODING_HEXTILE:
                encoder_ = std::make_unique<EncodingHexTile>();
                break;

            case RFB::ENCODING_CORRE:
                encoder_ = std::make_unique<EncodingRRE>(true);
                break;

            case RFB::ENCODING_RRE:
                encoder_ = std::make_unique<EncodingRRE>(false);
                break;

            case RFB::ENCODING_TRLE:
                encoder_ = std::make_unique<EncodingTRLE>();
                break;

            case RFB::ENCODING_ZRLE:
                encoder_ = std::make_unique<EncodingZRLE>();
                break;
#ifdef LTSM_ENCODING_FFMPEG

            case RFB::ENCODING_LTSM_H264:
            case RFB::ENCODING_LTSM_MPEG4:
                encoder_ = std::make_unique<EncodingFFmpeg>(compatible);
                break;
#endif
            case RFB::ENCODING_LTSM_ZQOI:
                encoder_ = std::make_unique<EncodingZQOI>();
                break;

            case RFB::ENCODING_LTSM_QOI:
                encoder_ = std::make_unique<EncodingQOI>();
                break;

            case RFB::ENCODING_LTSM_LZ4:
                encoder_ = std::make_unique<EncodingLZ4>();
                break;

            case RFB::ENCODING_LTSM_TJPG:
                encoder_ = std::make_unique<EncodingTJPG>();
                break;

            default:
                encoder_ = std::make_unique<EncodingRaw>();
                break;
        }

        encoderInitEvent(encoder_.get());
        Application::notice("{}: select encoding: {}", NS_FuncNameV, RFB::encodingName(encoder_->getType()));
    }

    /* pseudo encodings DesktopSize/Extended */
    asio::awaitable<void> RFB::ServerEncoder::sendEncodingDesktopResizeAwait(DesktopResizeStatus status, DesktopResizeError error,
            XCB::Size desktopSize) const {
        int statusCode = desktopResizeStatusCode(status);
        int errorCode = desktopResizeErrorCode(error);
        Application::info("{}: status: {}, error: {}, size: {}",
                NS_FuncNameV, statusCode, errorCode, desktopSize);

        if(! isClientSupportedEncoding(RFB::ENCODING_EXT_DESKTOP_SIZE)) {
            Application::error("{}: {}", NS_FuncNameV, "client not supported ExtDesktopResize encoding");
            throw rfb_error(NS_FuncNameS);
        }

        // send
        StreamBuf sb(36);
        sb.writeInt8(RFB::SERVER_FB_UPDATE).
            // padding
            writeInt8(0).
            // number of rects
            writeIntBE16(1).
            writeIntBE16(statusCode).
            writeIntBE16(errorCode).
            writeIntBE16(desktopSize.width).
            writeIntBE16(desktopSize.height).
            writeIntBE32(RFB::ENCODING_EXT_DESKTOP_SIZE).
            // number of screens
            writeInt8(1).
            // padding
            writeZero(3).
            // id
            writeIntBE32(0).
            // xpos
            writeIntBE16(0).
            // ypos
            writeIntBE16(0).
            // width
            writeIntBE16(desktopSize.width).
            // height
            writeIntBE16(desktopSize.height).
            // flags
            writeIntBE32(0);

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendEncodingRichCursorAwait(const FrameBuffer & fb, const XCB::Point & hot) const {
        auto & reg = fb.region();
        Application::debug(DebugType::Rfb, "{}: region: {}, hot: {}",
                           NS_FuncNameV, reg, hot);

        Tools::StreamBitsPack bitmask;

        const uint32_t clientAMask = ~(clientFormat().rmask() | clientFormat().gmask() | clientFormat().bmask());
        const PixelFormat clientFormatAlpha(clientFormat().bitsPerPixel(),
                                            clientFormat().rmask(), clientFormat().gmask(), clientFormat().bmask(), clientAMask);

        // StreamBuf
        RFB::EncodePacket sb(256);

        // RFB: 6.5.1
        sb.writeInt8(RFB::SERVER_FB_UPDATE).
            // padding
            writeInt8(0).
            // regions counts
            writeIntBE16(1).
            // region size
            writeIntBE16(hot.x).
            writeIntBE16(hot.y).
            writeIntBE16(reg.width).
            writeIntBE16(reg.height).
            // region type
            writeIntBE32(RFB::ENCODING_RICH_CURSOR);

        for(int oy = 0; oy < reg.height; ++oy) {
            for(int ox = 0; ox < reg.width; ++ox) {
                auto pixel = fb.pixel(XCB::Point(ox, oy));
                auto pixel2 = fb.pixelFormat().convertTo(pixel, clientFormatAlpha);
                // part1: send pixels buf
                sb.writeRawPixel(pixel2, clientFormat().bytePerPixel(), clientIsBigEndian());
                bitmask.pushBit(fb.pixelFormat().alpha(pixel) == fb.pixelFormat().amax());
            }

            bitmask.pushAlign();
        }

        // The bitmask consists of left-to-right, top-to-bottom scanlines,
        // where each scanline is padded to a whole number of bytes floor((width + 7) / 8
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#cursor-pseudo-encoding
        size_t bitmaskSize = std::floor((reg.width + 7) / 8) * reg.height;
        const std::vector<uint8_t> & bitmaskBuf = bitmask.toVector();

        if(bitmaskSize != bitmaskBuf.size()) {
            Application::error("{}: bitmask missmatch, buf size: {}, bitmask size: {}", NS_FuncNameV, bitmaskBuf.size(),
                               bitmaskSize);
            throw rfb_error(NS_FuncNameS);
        }

        // part2: send bitmask buf
        sb.write(bitmaskBuf);

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendEncodingLtsmCursorAwait(const XCB::Point & hot, const XCB::Size & cur, std::span<const uint8_t> pixels) const {
        Application::debug(DebugType::Rfb, "{}: size: {}, hot: {}", NS_FuncNameV, cur, hot);

        StreamBuf sb(256);
        // LTSM proto
        sb.writeInt8(RFB::SERVER_FB_UPDATE).
            // padding
            writeInt8(0).
            // rects
            writeIntBE16(1).
            writeIntBE16(hot.x).
            writeIntBE16(hot.y).
            writeIntBE16(cur.width).
            writeIntBE16(cur.height).
            writeIntBE32(ENCODING_LTSM_CURSOR);
        // cursor id
        auto cursorId = Tools::crc32b(pixels);
        sb.writeIntBE32(cursorId);

        // cursor rgba data
        if(std::ranges::none_of(cursorSended_, [&cursorId](auto & curid) { return curid == cursorId; })) {
            try {
                auto zlib = Tools::zlibCompress(pixels);
                // raw size
                sb.writeIntBE32(pixels.size()).
                    // compress size
                    writeIntBE32(zlib.size()).
                    // compress data
                    write(zlib);
                cursorSended_.push_front(cursorId);
            } catch(const std::exception & err) {
                Application::error("{}: exception: `{}'", NS_FuncNameV, err.what());
                sb.writeIntBE32(0);
            }
        } else {
            sb.writeIntBE32(0);
        }

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendEncodingLtsmSupportedAwait(void) const {
        Application::debug(DebugType::Rfb, "{}", NS_FuncNameV);

        StreamBuf sb(24);
        sb.writeInt8(RFB::SERVER_FB_UPDATE).
            // padding
            writeInt8(0).
            // rects
            writeIntBE16(1).
            writeIntBE16(0).
            writeIntBE16(0).
            writeIntBE16(0).
            writeIntBE16(0).
            writeIntBE32(ENCODING_LTSM).
            writeIntBE32(0).
            writeIntBE32(LTSM::service_version);

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendEncodingLtsmDataAwait(std::span<const uint8_t> buf) const {

        Application::debug(DebugType::Rfb, "{}: data size: {}", NS_FuncNameV, buf.size());

        StreamBuf sb(24 + buf.size());
        sb.writeInt8(RFB::SERVER_FB_UPDATE).
    	    // padding
    	    writeInt8(0).
    	    // rects
    	    writeIntBE16(1).
    	    writeIntBE16(0).
    	    writeIntBE16(0).
    	    writeIntBE16(0).
    	    writeIntBE16(0).
    	    writeIntBE32(ENCODING_LTSM).
    	    // raw data
    	    writeIntBE32(1).
    	    writeIntBE32(buf.size()).
    	    write(buf);

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    bool RFB::ServerEncoder::isClientLtsmSupported(void) const {
        return clientLtsmSupported;
    }

    bool RFB::ServerEncoder::isClientLtsmKeyboard(void) const {
        return clientLtsmKeyboard;
    }

    bool RFB::ServerEncoder::isEncoderFFmpeg(void) const {
        if(encoder_) {
            switch(encoder_->getType()) {
                case RFB::ENCODING_LTSM_H264:
                case RFB::ENCODING_LTSM_MPEG4:
                    return true;

                default: break;
            }
        }
        return false;
    }

    asio::awaitable<void> RFB::ServerEncoder::sendLtsmChannelAwait(CID channel, std::span<const uint8_t> buf) const {
        if(! clientLtsmSupported) {
            co_return;
        }

        Application::debug(DebugType::Channels, "{}: id: {}, data size: {}", NS_FuncNameV, channel, buf.size());

        if(buf.empty()) {
            Application::warning("{}: empty data", NS_FuncNameV);
            co_return;
        }

        assert(0xFFFF >= buf.size());
        StreamBuf sb(5 + buf.size());

        sb.writeInt8(RFB::PROTOCOL_LTSM).
    	    // version
    	    writeInt8(LtsmProtocolVersion).
    	    //channel
    	    writeInt8(channel).
    	    // data
    	    writeIntBE16(buf.size());

        if(channelDebug == channel) {
            auto str = Tools::rangeHexString(buf.begin(), buf.end(), 2);
            Application::trace(DebugType::Channels, "{}: id: {}, size: {}, content: [{}]",
                           NS_FuncNameV, channel, buf.size(), str);
        }

        sb.write(buf);

	co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        co_await send_lock_.async_lock();
        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        send_lock_.unlock();
        co_return;
    }

    void RFB::ServerEncoder::sendLtsmChannelData(CID channel, std::vector<uint8_t>&& buf) {
        asio::co_spawn(rfb_strand_, [this, channel, buf=std::move(buf)]() -> asio::awaitable<void> {
	    co_await sendLtsmChannelAwait(channel, buf);
	    co_return;
	}, asio::detached);
    }

    void RFB::ServerEncoder::sendLtsmChannelData(CID channel, std::string&& buf) {
        asio::co_spawn(rfb_strand_, [this, channel, buf=std::move(buf)]() -> asio::awaitable<void> {
    	    co_await sendLtsmChannelAwait(channel, std::span{ (const uint8_t*) buf.data(), buf.size() });
	    co_return;
	}, asio::detached);
    }

    std::pair<std::string, std::string> RFB::ServerEncoder::authInfo(void) const {
        return std::make_pair(clientAuthName_, clientAuthDomain_);
    }

    void RFB::ServerEncoder::setEncodingOptions(std::forward_list<std::string> && opts, uint32_t frameRate) {
        if(! encoder_) {
            return;
        }

        asio::dispatch(rfb_strand_, [this,opts=std::move(opts),frameRate]() {
            if(isEncoderFFmpeg()) {
            	encoder_->setFps(frameRate);
        	serverScreenUpdateRequest();
            }

            // apply opts: need full update
            if(encoder_->setEncodingOptions(opts)) {
                serverScreenUpdateRequest();
            }
        });
    }

    void RFB::ServerEncoder::cursorRequest(uint32_t cursorId) {
        Application::info("{}: cursorId: {:#010x}", NS_FuncNameV, cursorId);
        cursorSended_.remove(cursorId);
    }
}
