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
#include <chrono>
#include <cstring>
#include <algorithm>

#include "ltsm_zlib.h"
#include "ltsm_tools.h"
#include "librfb_client.h"
#include "ltsm_application.h"

#ifdef LTSM_WITH_GSSAPI
#include "ltsm_gsslayer.h"
#endif

#ifdef LTSM_DECODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

#include <boost/system/system_error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/container/small_vector.hpp>

#include "ltsm_boost_socket.h"

using namespace std::chrono_literals;
using namespace boost;

namespace LTSM {
    /* RFB::ClientDecoder */
    asio::awaitable<void> RFB::ClientDecoder::rfbHostConnectAwait(std::string_view host, uint16_t port, bool no_delay) {
        asio::ip::tcp::resolver resolver{rfb_strand_};
        auto endpoints = resolver.resolve(host, std::to_string(port));
        auto tcp_stream = std::make_unique<AsyncTcpStream>(rfb_strand_);
        co_await asio::async_connect(tcp_stream->socket(), endpoints, asio::use_awaitable);
        // set no delay
        if(no_delay) {
            boost::asio::ip::tcp::no_delay option(true);
            tcp_stream->socket().set_option(option);
        }
        stream_ = std::move(tcp_stream);
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::authVncInitAwait(std::string_view password) const {
        // recv challenge 16 bytes
        auto challenge = co_await stream_->async_recv_buffer(16);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto tmp = Tools::hexString(challenge, 2);
            Application::debug(DebugType::Rfb, "{}: challenge: {}", NS_FuncNameV, tmp);
        }

        auto crypt = OpenSSL::encryptDES(challenge, password);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto tmp = Tools::hexString(crypt, 2);
            Application::debug(DebugType::Rfb, "{}: encrypt: {}", NS_FuncNameV, tmp);
        }

        co_await stream_->async_send_buf(asio::buffer(crypt));
        co_return;
    }

    asio::awaitable<bool> RFB::ClientDecoder::authVenCryptInitAwait(const SecurityInfo & sec) {
        // server VenCrypt version
        const uint8_t majorVer = co_await stream_->async_recv_byte();
        const uint8_t minorVer = co_await stream_->async_recv_byte();
        Application::debug(DebugType::Rfb, "{}: server vencrypt version {}.{}", NS_FuncNameV, majorVer, minorVer);

        // client VenCrypt version 0.2
        co_await stream_->async_send_byte(0);
        co_await stream_->async_send_byte(2);

        // recv support flag
        const uint8_t zeroVer = co_await stream_->async_recv_byte();
        if(zeroVer != 0) {
            Application::error("{}: server unsupported vencrypt version: {}", NS_FuncNameV, zeroVer);
            co_return false;
        }

        // rect vencrypt types
        uint8_t typesCount = co_await stream_->async_recv_byte();

        if(0 == typesCount) {
            Application::error("{}: server vencrypt sub-types failure: {}", NS_FuncNameV, typesCount);
            co_return false;
        }

        container::small_vector<uint32_t, 4> venCryptTypes;

        while(typesCount--) {
            auto val = co_await stream_->async_recv_be32();
            venCryptTypes.push_back(val);
        }

        int mode = RFB::SECURITY_VENCRYPT02_TLSNONE;

        if(sec.tlsAnonMode) {
            if(std::ranges::none_of(venCryptTypes, [=](auto & val) { return val == RFB::SECURITY_VENCRYPT02_TLSNONE; })) {
                Application::error("{}: server unsupported tls: {} mode", NS_FuncNameV, "anon");
                co_return false;
            }
        } else {
            if(std::ranges::none_of(venCryptTypes, [=](auto & val) { return val == RFB::SECURITY_VENCRYPT02_X509NONE; })) {
                Application::error("{}: server unsupported tls: {} mode", NS_FuncNameV, "x509");
                co_return false;
            }

            mode = RFB::SECURITY_VENCRYPT02_X509NONE;
        }

        Application::debug(DebugType::Rfb, "{}: send vencrypt mode: {}", NS_FuncNameV, mode);
        co_await stream_->async_send_be32(mode);
        int status = co_await stream_->async_recv_byte();

        if(0 == status) {
            Application::error("{}: server invalid status", NS_FuncNameV);
            co_return false;
        }

        // TLS handshake
        try {
            auto tcp_stream = std::move(static_cast<AsyncTcpStream&>(*stream_).socket());
            auto sock = std::make_unique<AsioTls::AsyncStream>(std::move(tcp_stream), asio::ssl::context::tlsv12_client);

            auto& ssl_ctx = sock->ssl_context();
            auto& ssl_stream = sock->ssl_stream();
            int verify_mode = asio::ssl::verify_none;

            if(mode == RFB::SECURITY_VENCRYPT02_X509NONE) {
                ssl_ctx.set_default_verify_paths();

                if(! sec.caFile.empty()) {
                    ssl_ctx.load_verify_file(sec.caFile);
                }

                ssl_ctx.use_certificate_chain_file(sec.certFile);
                ssl_ctx.use_private_key_file(sec.keyFile, asio::ssl::context::pem);
                verify_mode = asio::ssl::verify_peer;
            }

            ssl_stream.set_verify_mode(verify_mode);
            sock->setCipherSuite("AECDH-AES256-SHA:@SECLEVEL=0");
            sock->sslHandshake(AsioTls::HandshakeType::Client);
            Application::info("{}: {}", NS_FuncNameV, "TLS handshake success");
            stream_ = std::move(sock);
        } catch(system::system_error & err) {
            auto ec = err.code();
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            co_return false;
        }

        co_return true;
    }

#ifdef LTSM_WITH_GSSAPI
    namespace GssWrapper {
        /// @brief: gss api client layer
        class Client : public Gss::ClientContext {
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
            Client(AsyncSocketBase & sock) : sock_{sock} {
            }

            bool checkUserCredential(std::string_view username) const {
                Gss::ErrorCodes err;
                if(auto cred = acquireUserCredential(username, & err)) {
                    return true;
                }
                error(NS_FuncNameV, err.func, err.code1, err.code2);
                return false;
            }

            bool handshakeLayer(std::string_view service, bool mutual = false, std::string_view username = "") {
                if(username.empty()) {
                    return connectService(service, mutual);
                }
                Gss::ErrorCodes err;
                if(auto cred = acquireUserCredential(username, & err)) {
                    return connectService(service, mutual, std::move(cred));
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
        
            void sendToken(const void* buf, size_t len) override {
                sendLength(len);
                sock_.sync_send_buf(buf, len);
            }
        };
    }

    asio::awaitable<bool> RFB::ClientDecoder::authGssApiInitAwait(const SecurityInfo & sec) {
        try {
            auto krb = std::make_unique<GssWrapper::Client>(*stream_);
            // a remote peer asked for mutual authentication
            const bool mutual = true;

            if(krb->handshakeLayer(sec.krb5Service, mutual, sec.krb5Name)) {
                Application::info("{}: kerberos auth: {}", NS_FuncNameV, "success");
                JsonObjectStream jo;
                jo.push("continue:tls", sec.authVenCrypt);
                auto json = jo.flush();
                // send security info json
                krb->sendToken(json.data(), json.size());
                // stop kerberos session
                krb.reset();

                // continue tls
                if(sec.authVenCrypt) {
                    co_return co_await authVenCryptInitAwait(sec);
                }

                co_return true;
            }
        } catch(const std::exception & err) {
            LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }

        const std::string err("security kerberos failed");
        Application::error("{}: error: {}", NS_FuncNameV, err);
        co_return false;
    }

#endif

    asio::awaitable<bool> RFB::ClientDecoder::rfbHandshakeAwait(const SecurityInfo & sec) {
        // https://vncdotool.readthedocs.io/en/0.8.0/rfbproto.html
        // RFB 1.7.1.1 version
        const auto version = fmt::format("RFB {:03}.{:03}\n", RFB::VERSION_MAJOR, RFB::VERSION_MINOR);
        const auto magick = co_await stream_->async_recv_string(12);

        if(magick.empty()) {
            Application::error("{}: handshake failure", NS_FuncNameV);
            co_return false;
        }

        Application::debug(DebugType::Rfb, "{}: handshake version: {}", NS_FuncNameV, magick.substr(0, magick.size() - 1));

        if(magick != version) {
            Application::error("{}: handshake failure", NS_FuncNameV);
            co_return false;
        }

        // 12 bytes
        co_await stream_->async_send_buf(asio::buffer(version));

        // RFB 1.7.1.2 security
        const uint8_t counts = co_await stream_->async_recv_byte();
        Application::debug(DebugType::Rfb, "{}: security counts: {}", NS_FuncNameV, counts);

        if(0 == counts) {
            auto len = co_await stream_->async_recv_be32();
            auto err = co_await stream_->async_recv_string(len);
            Application::error("{}: receive error: {}", NS_FuncNameV, err);
            co_return false;
        }

        auto security = co_await stream_->async_recv_buffer(counts);

#ifdef LTSM_WITH_GSSAPI
        Gss::CredentialPtr krb5Cred;

        if(sec.authKrb5 &&
            std::ranges::any_of(security, [=](auto & val) { return val == RFB::SECURITY_TYPE_GSSAPI; })) {
            // check local ticket
            if(krb5Cred = Gss::acquireUserCredential(sec.krb5Name); krb5Cred) {
                auto canon = Gss::displayName(krb5Cred->name);
                Application::info("{}: kerberos local ticket: {}", NS_FuncNameV, canon);
            }
        }

        if(krb5Cred) {
            Application::debug(DebugType::Rfb, "{}: security: {} selected", NS_FuncNameV, "gssapi");
            co_await stream_->async_send_byte(RFB::SECURITY_TYPE_GSSAPI);
            const bool authInit = co_await authGssApiInitAwait(sec);

            if(! authInit) {
                co_return false;
            }
        } else
#endif
        if(sec.authVenCrypt &&
                std::ranges::any_of(security, [=](auto & val) { return val == RFB::SECURITY_TYPE_VENCRYPT; })) {
            Application::debug(DebugType::Rfb, "{}: security: {} selected", NS_FuncNameV, "vencrypt");
            co_await stream_->async_send_byte(RFB::SECURITY_TYPE_VENCRYPT);
            const bool authInit = co_await authVenCryptInitAwait(sec);

            if(! authInit) {
                co_return false;
            }
        } else if(sec.authVnc &&
            std::ranges::any_of(security, [=](auto & val) { return val == RFB::SECURITY_TYPE_VNC; })) {
            auto & password = sec.passwdFile;

            if(password.empty()) {
                Application::error("{}: security vnc: password empty", NS_FuncNameV);
                co_return false;
            }

            Application::debug(DebugType::Rfb, "{}: security: {} selected", NS_FuncNameV, "vncauth");

            co_await stream_->async_send_byte(RFB::SECURITY_TYPE_VNC);
            co_await authVncInitAwait(password);
        } else if(sec.authNone &&
                std::ranges::any_of(security, [=](auto & val) { return val == RFB::SECURITY_TYPE_NONE; })) {
            Application::debug(DebugType::Rfb, "{}: security: {} selected", NS_FuncNameV, "noauth");
            co_await stream_->async_send_byte(RFB::SECURITY_TYPE_NONE);
        } else {
            Application::error("{}: security vnc: not supported", NS_FuncNameV);
            co_return false;
        }

        const auto secReply = co_await stream_->async_recv_be32();

        // RFB 1.7.1.3 security result
        if(RFB::SECURITY_RESULT_OK != secReply) {
            auto len = co_await stream_->async_recv_be32();
            auto err = co_await stream_->async_recv_string(len);
            Application::error("{}: receive error: {}", NS_FuncNameV, err);
            co_return false;
        }

        bool shared = false;
        Application::debug(DebugType::Rfb, "{}: send share flags: {}", NS_FuncNameV, (int) shared);
        // RFB 6.3.1 client init (shared flag)
        co_await stream_->async_send_byte(shared ? 1 : 0);

        // RFB 6.3.2 server init
        auto buf = co_await stream_->async_recv_buffer(20);
        StreamBufRef sb(buf.data(), buf.size());

        auto fbWidth = sb.readIntBE16();
        auto fbHeight = sb.readIntBE16();
        // recv server pixel format
        auto bpp = sb.readInt8();
        auto depth = sb.readInt8();
        server_big_endian_ = sb.readInt8();
        server_true_color_ = sb.readInt8();
        auto rmax = sb.readIntBE16();
        auto gmax = sb.readIntBE16();
        auto bmax = sb.readIntBE16();
        auto rshift = sb.readInt8();
        auto gshift = sb.readInt8();
        auto bshift = sb.readInt8();
        // also align (3 byte)

        server_pf_ = PixelFormat(bpp, rmax, gmax, bmax, 0, rshift, gshift, bshift, 0);
        Application::debug(DebugType::Rfb, "{}: remote framebuffer size: {}", NS_FuncNameV, XCB::Size(fbWidth, fbHeight));

        Application::info("{}: remote pixel format: bpp: {}, depth: {}, bigendian: {}, true color: {}, red({:#010x}), green({:#010x}), blue({:#010x})",
                           NS_FuncNameV, server_pf_.bitsPerPixel(), depth, (int) server_big_endian_, (int) server_true_color_,
                           server_pf_.rmask(), server_pf_.gmask(), server_pf_.bmask());

        // check server format
        switch(bpp) {
            case 32:
            case 24:
            case 16:
            case 8:
                break;

            default:
                Application::error("{}: unknown pixel format, bpp: {}, depth: {}", NS_FuncNameV, bpp, depth);
                co_return false;
        }

        if(! server_true_color_ || server_pf_.rmax() == 0 || server_pf_.gmax() == 0 || server_pf_.bmax() == 0) {
            Application::error("{}: unsupported pixel format", NS_FuncNameV);
            co_return false;
        }

        clientRecvPixelFormatEvent(server_pf_, XCB::Size(fbWidth, fbHeight));
        // recv name desktop
        auto nameLen = co_await stream_->async_recv_be32();
        auto nameDesktop = co_await stream_->async_recv_string(nameLen);

        Application::debug(DebugType::Rfb, "{}: server desktop name: {}", NS_FuncNameV, nameDesktop);
        co_return true;
    }

    bool RFB::ClientDecoder::isContinueUpdatesSupport(void) const {
        return continueUpdatesSupport;
    }

    bool RFB::ClientDecoder::isContinueUpdatesProcessed(void) const {
        return continueUpdatesProcessed;
    }

    const PixelFormat & RFB::ClientDecoder::serverFormat(void) const {
        return server_pf_;
    }

    void RFB::ClientDecoder::rfbMessagesShutdown(void) {
        channelsShutdown();
        incr_update_timer_.cancel();
        if(stream_) {
            stream_->closeSocket();
        }
    }

    std::list<int> RFB::ClientDecoder::supportedEncodings(bool extclip) {
        std::list<int> encodings = {
            // first preffered
#ifdef LTSM_DECODING_LZ4
            ENCODING_LTSM_ZQOI,
#endif
#ifdef LTSM_DECODING_TJPG
            ENCODING_LTSM_TJPG,
#endif
#ifdef LTSM_DECODING_FFMPEG
#ifdef LTSM_DECODING_H264
            ENCODING_LTSM_H264,
#endif
#ifdef LTSM_DECODING_MPEG4
            ENCODING_LTSM_MPEG4,
#endif
#endif
#ifdef LTSM_DECODING_LZ4
            ENCODING_LTSM_LZ4,
#endif
            ENCODING_LTSM_QOI,
            ENCODING_LTSM_KEYB,
            ENCODING_LTSM_CURSOR,
            // compatible RFB encodings
            ENCODING_ZRLE, ENCODING_TRLE, ENCODING_HEXTILE,
            ENCODING_ZLIB, ENCODING_CORRE, ENCODING_RRE,
            // audio
#ifdef LTSM_WITH_OPUS
            ENCODING_LTSM_OPUS,
#endif
            ENCODING_LTSM_PCM
        };

        if(extclip) {
            encodings.push_back(ENCODING_EXT_CLIPBOARD);
        }

        // last
        encodings.push_back(ENCODING_RAW);

        return encodings;
    }

    asio::awaitable<void> RFB::ClientDecoder::rfbRequestIncrUpdate(void) {
        for(;;) {
            incr_update_timer_.expires_after(300ms);
            co_await incr_update_timer_.async_wait(asio::use_awaitable);

            if(! (continueUpdatesSupport && continueUpdatesProcessed)) {
                // request incr update
                co_await sendFrameBufferUpdateAwait(true);
            }
        }
    }

    asio::awaitable<void> RFB::ClientDecoder::rfbMessagesLoopAwait(void) {
        auto encodings = supportedEncodings();
        // added pseudo encodings
        encodings.push_front(ENCODING_LAST_RECT);
        encodings.push_front(ENCODING_RICH_CURSOR);
        encodings.push_front(ENCODING_EXT_DESKTOP_SIZE);
        encodings.push_front(ENCODING_CONTINUOUS_UPDATES);

        if(auto audioEncoding = clientPrefferedAudioEncoding(); 0 < audioEncoding) {
            encodings.push_front(audioEncoding);
        }

        if(clientLtsmSupported()) {
            encodings.push_front(ENCODING_LTSM);
        }

        if(auto prefferedEncoding = clientPrefferedVideoEncoding(); 0 < prefferedEncoding) {
#ifdef LTSM_DECODING_FFMPEG

            // experimental feature remove and added from preffered
            if(prefferedEncoding != ENCODING_LTSM_H264) {
                encodings.remove(ENCODING_LTSM_H264);
            }

            if(prefferedEncoding != ENCODING_LTSM_MPEG4) {
                encodings.remove(ENCODING_LTSM_MPEG4);
            }

#endif
            // preffered set priority
            auto it = std::ranges::find_if(encodings, [&](auto & enc) {
                return prefferedEncoding == enc;
            });

            if(it != encodings.end()) {
                auto enc = *it;
                encodings.erase(it);
                it = std::ranges::find(encodings, ENCODING_LTSM);
                encodings.insert(it == encodings.end() ? it : std::next(it), enc);
            }
        }

        co_await sendEncodingsAwait(encodings);
        co_await sendPixelFormatAwait();

        // request full update
        co_await sendFrameBufferUpdateAwait(false);
        Application::debug(DebugType::Rfb, "{}: wait remote messages...", NS_FuncNameV);

        // request incr update job
        asio::co_spawn(rfb_strand_, rfbRequestIncrUpdate(), asio::detached);

        while(true) {
            co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
            int msgType = co_await stream_->async_recv_byte();

            switch(msgType) {
                case PROTOCOL_LTSM:
                    co_await recvLtsmProtoAwait();
                    break;

                case SERVER_FB_UPDATE:
                    co_await recvFBUpdateEventAwait();
                    break;

                case SERVER_SET_COLOURMAP:
                    co_await recvColorMapEventAwait();
                    break;

                case SERVER_BELL:
                    co_await recvBellEventAwait();
                    break;

                case SERVER_CUT_TEXT:
                    co_await recvCutTextEventAwait();
                    break;

                case SERVER_CONTINUOUS_UPDATES:
                    co_await recvContinuousUpdatesEventAwait();
                    break;

                default: {
                    Application::error("{}: unknown message: {:#04x}", NS_FuncNameV, msgType);
                    throw system::system_error(asio::error::operation_aborted);
                }
            }
        }

        co_return;
    }

    bool RFB::ClientDecoder::isDecoderFFmpeg(void) const {
        if(decoder_) {
            switch(decoder_->type()) {
                case RFB::ENCODING_LTSM_H264:
                case RFB::ENCODING_LTSM_MPEG4:
                    return true;

                default: break;
            }
        }
        return false;
    }

    void RFB::ClientDecoder::displayResizeEvent(const XCB::Size & dsz) {
        Application::info("{}: display resized, new size: {}", NS_FuncNameV, dsz);
#ifdef LTSM_DECODING_FFMPEG
        // event background
        if(isDecoderFFmpeg()) {
            asio::dispatch(rfb_strand_, [this, sz = dsz]() {
                this->decoder_->resizedEvent(sz);
            });
        }
#endif
    }

    asio::awaitable<void> RFB::ClientDecoder::sendPixelFormatAwait(void) const {
        const auto & pf = clientFormat();
        Application::debug(DebugType::Rfb, "{}: local pixel format: bpp: {}, bigendian: {}, red({:#010x}), green({:#010x}), blue({:#010x})",
                           NS_FuncNameV, pf.bitsPerPixel(), (int) platformBigEndian(),
                           pf.rmask(), pf.gmask(), pf.bmask());

        StreamBuf sb(20);

        const uint8_t trueColor = 1;
        const uint8_t defaultDepth = 24;

        sb.writeInt8(RFB::CLIENT_SET_PIXEL_FORMAT).
            writeZero(3). // padding
            writeInt8(pf.bitsPerPixel()).
            writeInt8(defaultDepth).
            writeInt8(platformBigEndian()).
            writeInt8(trueColor).
            writeIntBE16(pf.rmax()).
            writeIntBE16(pf.gmax()).
            writeIntBE16(pf.bmax()).
            writeInt8(pf.rshift()).
            writeInt8(pf.gshift()).
            writeInt8(pf.bshift()).
            writeZero(3); // padding

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendEncodingsAwait(const std::list<int> & encodings) const {
        for(const auto & type : encodings) {
            Application::debug(DebugType::Rfb, "{}: {}", NS_FuncNameV, encodingName(type));
        }

        StreamBuf sb(4 + encodings.size() * sizeof(uint32_t));

        sb.writeInt8(RFB::CLIENT_SET_ENCODINGS).
            writeZero(1). // padding
            writeIntBE16(encodings.size());

        for(const auto & val : encodings) {
            sb.writeIntBE32(val);
        }

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendFrameBufferUpdateAwait(bool incr) const {
        auto csz = clientSize();
        co_await sendFrameBufferUpdateAwait(XCB::Region{0, 0, csz.width, csz.height}, incr);
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendFrameBufferUpdateAwait(const XCB::Region & reg, bool incr) const {
        Application::debug(DebugType::Rfb, "{}: region: {}", NS_FuncNameV, reg);

        StreamBuf sb(10);

        sb.writeInt8(RFB::CLIENT_REQUEST_FB_UPDATE).
            writeInt8(incr ? 1 : 0).
            writeIntBE16(reg.x).
            writeIntBE16(reg.y).
            writeIntBE16(reg.width).
            writeIntBE16(reg.height);

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendContinuousUpdatesAwait(bool enable, const XCB::Region & reg) {
        Application::debug(DebugType::Rfb, "{}: status: {}, region: {}", NS_FuncNameV,
                           (enable ? "enable" : "disable"), reg);

        StreamBuf sb(10);

        sb.writeInt8(CLIENT_CONTINUOUS_UPDATES).
            writeInt8(enable ? 1 : 0).
            writeIntBE16(reg.x).
            writeIntBE16(reg.y).
            writeIntBE16(reg.width).
            writeIntBE16(reg.height);

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        continueUpdatesProcessed = enable;
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendSetDesktopSizeAwait(const XCB::Size & wsz) {
        Application::info("{}: size: {}", NS_FuncNameV, wsz);

        StreamBuf sb(24);

        sb.writeInt8(RFB::CLIENT_SET_DESKTOP_SIZE).
            writeZero(1).
            writeIntBE16(wsz.width).
            writeIntBE16(wsz.height).
            // num of screens
            writeInt8(1).
            writeZero(1).
            // screen id
            writeIntBE32(0).
            // posx, posy
            writeIntBE32(0).
            writeIntBE16(wsz.width).
            writeIntBE16(wsz.height).
            // flag
            writeIntBE32(0);

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendKeyEventAwait(bool pressed, uint32_t keysym, uint16_t scancode) {
        Application::debug(DebugType::Rfb, "{}: keysym: {:#010x}, pressed: {}", NS_FuncNameV, keysym, (int) pressed);

        // support: ENCODING_LTSM_KEYB
        const bool ltsmKeybSupport = true; 
        StreamBuf sb(8);

        if(ltsmKeybSupport) {
            sb.writeInt8(RFB::CLIENT_EVENT_KEY).
                writeInt8(pressed ? 1 : 0).
                writeIntBE16(scancode).
                writeIntBE32(keysym);
        } else {
            sb.writeInt8(RFB::CLIENT_EVENT_KEY).
                writeInt8(pressed ? 1 : 0).
                // padding
                writeZero(2).
                writeIntBE32(keysym);
        }

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendPointerEventAwait(uint8_t buttons, uint16_t posx, uint16_t posy) {
        Application::debug(DebugType::Rfb, "{}: pointer: {}, buttons: {:#04x}", NS_FuncNameV, XCB::Point(posx, posy), buttons);

        StreamBuf sb(6);

        sb.writeInt8(RFB::CLIENT_EVENT_POINTER).
            writeInt8(buttons).
            writeIntBE16(posx).
            writeIntBE16(posy);

        co_await stream_->async_send_buf(asio::buffer(sb.rawbuf()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendCutTextEventAwait(std::span<const uint8_t> buf, bool ext) {
        StreamBuf sb(8);

        sb.writeInt8(RFB::CLIENT_CUT_TEXT).
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
            sb.writeIntBE32(static_cast<uint32_t>(0xFFFFFFFF) - buf.size() + 1);
        } else {
            Application::debug(DebugType::Rfb, "{}: length text: {}", NS_FuncNameV, buf.size());
            sb.writeIntBE32(buf.size());
        }

        // send
        co_await stream_->async_send_values(asio::buffer(sb.rawbuf()), asio::buffer(buf.data(), buf.size()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::sendLtsmChannelAwait(uint8_t channel, std::span<const uint8_t> buf) {
        Application::debug(DebugType::Channels, "{}: id: {}, data size: {}", NS_FuncNameV, channel, buf.size());

        StreamBuf sb(5);

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

        // send
        co_await stream_->async_send_values(asio::buffer(sb.rawbuf()), asio::buffer(buf.data(), buf.size()));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvLtsmProtoAwait(void) {
        if(0 == server_ltsm_version_) {
            Application::error("{}: server not supported: {}", NS_FuncNameV, RFB::encodingName(RFB::ENCODING_LTSM));
            throw system::system_error(asio::error::operation_aborted);
        }

        Application::debug(DebugType::Rfb, "{}", NS_FuncNameV);

        const uint8_t version = co_await stream_->async_recv_byte();

        if(version != LtsmProtocolVersion) {
            Application::error("{}: unknown version: {:#04x}", NS_FuncNameV, version);
            throw channel_error(NS_FuncNameS);
        }

        const uint8_t channel = co_await stream_->async_recv_byte();
        const uint16_t length = co_await stream_->async_recv_be16();
        auto buf = co_await stream_->async_recv_buffer(length);

        ChannelClient::recvLtsmProto(channel, std::move(buf));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvFBUpdateEventAwait(void) {
        auto start = std::chrono::steady_clock::now();
        // format -
        // u8: padding
        // u16: num rects

        [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_byte();
        const uint16_t numRects = co_await stream_->async_recv_be16();
    
        Application::debug(DebugType::Rfb, "{}: num rects: {}", NS_FuncNameV, numRects);

        for(int it = 0; it < numRects; ++it) {
            co_await recvFBUpdateRegionAwait();
        }

        waitDecoderJobs();

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto dt = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            Application::debug(DebugType::Rfb, "{}: update time: {}us", NS_FuncNameV, dt.count());
        }

        clientRecvFBUpdateEvent();
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvFBUpdateRegionAwait(void) {
        // region format -
        // u16: rx
        // u16: ry
        // u16: rw
        // u16: rh
        // u32: encoding type

        XCB::Region reg;
        reg.x = co_await stream_->async_recv_be16();
        reg.y = co_await stream_->async_recv_be16();
        reg.width = co_await stream_->async_recv_be16();
        reg.height = co_await stream_->async_recv_be16();

        int32_t encodingType = co_await stream_->async_recv_be32();

        Application::debug(DebugType::Rfb, "{}: region: {}, encodingType: {}",
                           NS_FuncNameV, reg, RFB::encodingName(encodingType));

        switch(encodingType) {
            case ENCODING_LTSM:
                co_await recvDecodingLtsmAwait(reg);
                break;

            case ENCODING_LAST_RECT:
                co_await recvDecodingLastRectAwait(reg);
                throw std::runtime_error("unsupported LastRect");
                break;

            case ENCODING_LTSM_CURSOR:
                co_await recvDecodingLtsmCursorAwait(reg);
                break;

            case ENCODING_RICH_CURSOR:
                co_await recvDecodingRichCursorAwait(reg);
                break;

            case ENCODING_EXT_DESKTOP_SIZE:
                co_await recvDecodingExtDesktopSizeAwait(reg.x, reg.y, reg.toSize());
                break;

            case RFB::ENCODING_DESKTOP_SIZE:
                Application::warning("{}: {}", NS_FuncNameV, "old desktop_size");
                break;

            default:
                co_await recvDecodingUpdateRegionAwait(encodingType, reg);
                break;
        }
        
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvDecodingLtsmAwait(const XCB::Region & reg) {

        uint32_t type = co_await stream_->async_recv_be32();
        Application::info("{}: success, type: {}", NS_FuncNameV, type);

        // type 0: handshake part
        if(type == 0) {
            // ltsm 1.1 ver: flags
            // ltsm 2.1 ver: version
            server_ltsm_version_ = co_await stream_->async_recv_be32();

            clientRecvLtsmHandshakeEvent(0 /* flags */);
        }
        // type 1: data part
        else if(type == 1) {
            // type: LTSM version
            auto len = co_await stream_->async_recv_be32();
            auto buf = co_await stream_->async_recv_buffer(len);

            clientRecvLtsmDataEvent(std::move(buf));
        } else {
            Application::error("{}: unknown type: {}", NS_FuncNameV, type);
            throw rfb_error(NS_FuncNameS);
        }
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvDecodingLastRectAwait(const XCB::Region & reg) {
        Application::debug(DebugType::Rfb, "{}: decoding region: {}", NS_FuncNameV, reg);
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvDecodingLtsmCursorAwait(const XCB::Region & reg) {
        Application::debug(DebugType::Rfb, "{}: decoding region: {}", NS_FuncNameV, reg);

        BinaryBuf buf;

        const uint32_t cursorId = co_await stream_->async_recv_be32();
        const uint32_t rawsz = co_await stream_->async_recv_be32();

        if(rawsz) {
            auto zipsz = co_await stream_->async_recv_be32();

            if(zipsz) {
                auto zip = co_await stream_->async_recv_buffer(zipsz);
                buf = Tools::zlibUncompress(zip, rawsz);
            } else {
                buf = co_await stream_->async_recv_buffer(rawsz);
            }
        }

        clientRecvLtsmCursorEvent(reg, cursorId, std::move(buf));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvDecodingRichCursorAwait(const XCB::Region & reg) {
        Application::debug(DebugType::Rfb, "{}: decoding region: {}", NS_FuncNameV, reg);
        const auto bufsz = static_cast<uint32_t>(reg.width) * reg.height * clientFormat().bytePerPixel();
        const auto masksz = std::floor((static_cast<uint32_t>(reg.width) + 7) / 8) * reg.height;

        auto buf = co_await stream_->async_recv_buffer(bufsz);
        auto mask = co_await stream_->async_recv_buffer(masksz);

        Application::trace(DebugType::Rfb, "{}: bufsz: {}, masksz: {}", NS_FuncNameV, buf.size(), mask.size());

        clientRecvRichCursorEvent(reg, std::move(buf), std::move(mask));
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvDecodingExtDesktopSizeAwait(int status, int err, const XCB::Size & sz) {
        Application::info("{}: status: {}, error: {}, size: {}", NS_FuncNameV, status, err, sz);

        const uint8_t numOfScreens = co_await stream_->async_recv_byte();
        [[maybe_unused]] const uint8_t pad1 = co_await stream_->async_recv_byte();
        [[maybe_unused]] const uint8_t pad2 = co_await stream_->async_recv_byte();
        [[maybe_unused]] const uint8_t pad3 = co_await stream_->async_recv_byte();

        const auto bufsz = static_cast<uint32_t>(numOfScreens) *
            (sizeof(uint32_t) /* screen id */ + 4 * sizeof(uint16_t) /* region */ + sizeof(uint32_t) /* flags */);
        auto buf = co_await stream_->async_recv_buffer(bufsz);

        StreamBufRef sb(buf.data(), buf.size());
        std::vector<RFB::ScreenInfo> screens(numOfScreens);

        for(auto & screen : screens) {
            screen.id = sb.readIntBE32();
            auto posx = sb.readIntBE16();
            auto posy = sb.readIntBE16();
            screen.width = sb.readIntBE16();
            screen.height = sb.readIntBE16();
            auto flags = sb.readIntBE32();

            Application::debug(DebugType::Rfb, "{}: screen: {}, area: {}, flags: {:#010x}",
                               NS_FuncNameV, screen.id, XCB::Region(posx, posy, screen.width, screen.height), flags);
        }

        clientRecvDecodingDesktopSizeEvent(status, err, sz, screens);
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvColorMapEventAwait(void) {
        [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_byte();
        const uint16_t firstColor = co_await stream_->async_recv_be16();
        const uint16_t numColors = co_await stream_->async_recv_be16();

        const auto bufsz = static_cast<uint32_t>(numColors) * 3 /* rgb - 3 bytes */;
        auto buf = co_await stream_->async_recv_buffer(bufsz);

        Application::debug(DebugType::Rfb, "{}: num colors: {}, first color: {}", NS_FuncNameV, numColors, firstColor);

        StreamBufRef sb(buf.data(), buf.size());
        std::vector<Color> colors(numColors);

        for(auto & col : colors) {
            col.r = sb.readInt8();
            col.g = sb.readInt8();
            col.b = sb.readInt8();

            Application::trace(DebugType::Rfb, "{}: color [{:#04x},{:#04x},{:#04x}]", NS_FuncNameV, col.r, col.g, col.b);
        }

        clientRecvSetColorMapEvent(colors);
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvBellEventAwait(void) {
        Application::debug(DebugType::Rfb, "{}: message", NS_FuncNameV);
        clientRecvBellEvent();
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvCutTextEventAwait(void) {
        [[maybe_unused]] const auto pad1 = co_await stream_->async_recv_byte();
        [[maybe_unused]] const auto pad2 = co_await stream_->async_recv_byte();
        [[maybe_unused]] const auto pad3 = co_await stream_->async_recv_byte();
        const int32_t length = co_await stream_->async_recv_be32();

        if(length < 0 && 0 == extClipboardLocalCaps()) {
            Application::error("{}: invalid format, failed `{}'", NS_FuncNameV, "ext clipboard");
            throw rfb_error(NS_FuncNameS);
        }

        auto buf = co_await stream_->async_recv_buffer(std::abs(length));

        if(0 == length) {
            co_return;
        }

        // A negative value of length indicates that the extended message format is used and abs(length) is the total number of following bytes.
        // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#extended-clipboard-pseudo-encoding
        if(0 < length) {
            Application::debug(DebugType::Rfb, "{}: length: {}", NS_FuncNameV, length);
            clientRecvCutTextEvent(std::move(buf));
        } else {
            Application::debug(DebugType::Rfb, "{}: length: {}, extclip", NS_FuncNameV, length);
            recvExtClipboardCapsEvent(std::move(buf));
        }
        co_return;
    }

    asio::awaitable<void> RFB::ClientDecoder::recvContinuousUpdatesEventAwait(void) {
        Application::debug(DebugType::Rfb, "{}: message", NS_FuncNameV);
        continueUpdatesSupport = true;

        asio::co_spawn(rfb_strand_, sendContinuousUpdatesAwait(false, { XCB::Point(0, 0), clientSize() }), asio::detached);
        co_return;
    }

    void RFB::ClientDecoder::sendCutText(std::vector<uint8_t>&& buf, bool ext) {
        if(! buf.empty()) {
            asio::co_spawn(rfb_strand_, [this, ext, buf = std::move(buf)]() -> asio::awaitable<void> {
                co_await sendCutTextEventAwait(buf, ext);
                co_return;
            }, asio::detached);
        }
    }

    void RFB::ClientDecoder::sendLtsmChannelData(uint8_t channel, std::vector<uint8_t>&& buf) {
        if(! buf.empty()) {
            assert(0xFFFF >= buf.size());
            asio::co_spawn(rfb_strand_, [this, channel, buf = std::move(buf)]() -> asio::awaitable<void> {
                co_await sendLtsmChannelAwait(channel, buf);
                co_return;
            }, asio::detached);
        }
    }

    void RFB::ClientDecoder::sendLtsmChannelData(uint8_t channel, std::string&& buf) {
        if(! buf.empty()) {
            assert(0xFFFF >= buf.size());
            asio::co_spawn(rfb_strand_, [this, channel, buf = std::move(buf)]() -> asio::awaitable<void> {
                co_await sendLtsmChannelAwait(channel, {(const uint8_t*) buf.data(), buf.size()});
                co_return;
            }, asio::detached);
        }
    }

    asio::awaitable<void> RFB::ClientDecoder::recvDecodingUpdateRegionAwait(int type, const XCB::Region & reg) {
        if(! decoder_ || type != decoder_->type()) {
            switch(type) {
                case ENCODING_RAW:
                    decoder_ = std::make_unique<DecodingRaw>();
                    break;

                case ENCODING_RRE:
                    decoder_ = std::make_unique<DecodingRRE>(false);
                    break;

                case ENCODING_CORRE:
                    decoder_ = std::make_unique<DecodingRRE>(true);
                    break;

                case ENCODING_HEXTILE:
                    decoder_ = std::make_unique<DecodingHexTile>(false);
                    break;

                case ENCODING_TRLE:
                    decoder_ = std::make_unique<DecodingTRLE>(false);
                    break;

                case ENCODING_ZRLE:
                    decoder_ = std::make_unique<DecodingTRLE>(true);
                    break;

                case ENCODING_ZLIB:
                    decoder_ = std::make_unique<DecodingZlib>();
                    break;

                case ENCODING_LTSM_QOI:
                    decoder_ = std::make_unique<DecodingQOI>(false);
                    break;
#ifdef LTSM_DECODING_LZ4
                case ENCODING_LTSM_ZQOI:
                    decoder_ = std::make_unique<DecodingQOI>(true);
                    break;
                case ENCODING_LTSM_LZ4:
                    decoder_ = std::make_unique<DecodingLZ4>();
                    break;
#endif
#ifdef LTSM_DECODING_TJPG
                case ENCODING_LTSM_TJPG:
                    decoder_ = std::make_unique<DecodingTJPG>();
                    break;
#endif

#ifdef LTSM_DECODING_FFMPEG
#ifdef LTSM_DECODING_H264
                case ENCODING_LTSM_H264:
#endif
#ifdef LTSM_DECODING_MPEG4
                case ENCODING_LTSM_MPEG4:
#endif
                    decoder_ = std::make_unique<DecodingFFmpeg>(type, frameRateOption());
                    // FIXME
                    // decoder_->setDebug(4 /* AV_LOG_VERBOSE */);
                    break;
#endif

                default: {
                    Application::error("{}: {}", NS_FuncNameV, "unknown decoding");
                    throw rfb_error(NS_FuncNameS);
                }
            }

            decoderInitEvent(decoder_.get());
        }

        const AsioDecoderStream wrap(*stream_);

        switch(decoder_->type()) {
            case ENCODING_RAW:
                {
                    auto len = static_cast<uint32_t>(reg.width) * reg.height * clientFormat().bytePerPixel();
                    auto buf = co_await stream_->async_recv_buffer(len);
                    decoder_->updateRegionBuf(std::move(buf), *this, reg);
                }
                break;

            case ENCODING_LTSM_QOI:
            case ENCODING_LTSM_ZQOI:
            case ENCODING_LTSM_LZ4:
            case ENCODING_LTSM_TJPG:
            case ENCODING_LTSM_H264:
            case ENCODING_LTSM_MPEG4:
            case ENCODING_ZLIB:
                {
                    auto len = co_await stream_->async_recv_be32();
                    auto buf = co_await stream_->async_recv_buffer(len);
                    decoder_->updateRegionBuf(std::move(buf), *this, reg);
                }
                break;

            default:
                decoder_->updateRegionStream(wrap, *this, reg);
                break;
        }

        co_return;
    }

    void RFB::ClientDecoder::recvChannelSystemEvent(const std::vector<uint8_t> & buf) {
        JsonContent jc;
        jc.parseBinary(reinterpret_cast<const char*>(buf.data()), buf.size());

        if(! jc.isObject()) {
            Application::error("{}: {}", NS_FuncNameV, "json broken");
            throw std::invalid_argument(NS_FuncNameS);
        }

        auto jo = jc.toObject();
        auto cmd = jo.getString("cmd");

        if(cmd.empty()) {
            Application::error("{}: {}", NS_FuncNameV, "format message broken");
            throw std::invalid_argument(NS_FuncNameS);
        }

        if(cmd == SystemCommand::ChannelOpen) {
            systemChannelOpen(jo);
        } else if(cmd == SystemCommand::ChannelListen) {
            systemChannelListen(jo);
        } else if(cmd == SystemCommand::ChannelClose) {
            systemChannelClose(jo);
        } else if(cmd == SystemCommand::ChannelConnected) {
            systemChannelConnected(jo);
        } else if(cmd == SystemCommand::ChannelError) {
            systemChannelError(jo);
        } else if(cmd == SystemCommand::LoginSuccess) {
            systemLoginSuccess(jo);
        } else {
            Application::error("{}: {}", NS_FuncNameV, "unknown cmd");
            throw std::invalid_argument(NS_FuncNameS);
        }
    }
}
