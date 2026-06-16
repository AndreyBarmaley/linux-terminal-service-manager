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

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <exception>

#include <boost/asio.hpp>
#include "cups/backend.h"

#include "ltsm_tools.h"
#include "ltsm_application.h"

using namespace std::chrono_literals;
using namespace boost;

/*
    CUPS_BACKEND_OK, CUPS_BACKEND_FAILED, CUPS_BACKEND_AUTH_REQUIRED, CUPS_BACKEND_HOLD,
    CUPS_BACKEND_STOP, CUPS_BACKEND_CANCEL, CUPS_BACKEND_RETRY, CUPS_BACKEND_RETRY_CURRENT
*/

namespace LTSM {
    auto backendType = "direct";
    auto backendName = "ltsm";
    auto backendDescription = "LTSM Virtual Print";

    class CupsBackend : public Application {
        int jobFd_ = STDIN_FILENO;
        int jobId_ = 0;
        int jobNumPage_ = 0;
        int cupsStatus_ = CUPS_BACKEND_CANCEL;

        std::string_view jobUser_;
        std::string_view jobTitle_;
        std::string_view jobOpts_;

      public:
        CupsBackend(asio::io_context & ctx, int argc, const char** argv) : Application("ltsm_cups") {
            setDebugTarget(DebugTarget::Syslog);
            setDebugLevel(DebugLevel::Info);

            // job from stdin
            if(argc == 6) {
                jobId_ = std::stoi(argv[1]);
                jobUser_ = argv[2];
                jobTitle_ = argv[3];
                jobNumPage_ = std::stoi(argv[4]);
                jobOpts_ = argv[5];
            } else {
                jobId_ = std::stoi(argv[1]);
                jobUser_ = argv[2];
                jobTitle_ = argv[3];
                jobNumPage_ = std::stoi(argv[4]);
                jobOpts_ = argv[5];
                jobFd_ = open(argv[6], O_RDONLY);

                if(0 > jobFd_) {
                    Application::error("{}: {} failed, path: '{}'", NS_FuncNameV, "open", argv[6]);
                    throw std::runtime_error(NS_FuncNameS);
                }
            }
        }

        asio::awaitable<void> start(void) {
            Application::info("{}: get uid: {}, get gid: {}", NS_FuncNameV, getuid(), getgid());
            std::string socketFormat = "/var/run/ltsm/cups/printer_username";

            if(auto deviceURI = getenv("DEVICE_URI")) {
                if(auto ptr = std::strstr(deviceURI, "://")) {
                    socketFormat = ptr + 3;
                }
            }
            auto socket_path = Tools::replace(socketFormat, "username", jobUser_);

            Application::debug(DebugType::App, "{}: job - id: {}, user: {}, title: '{}', opts: '{}', socket: '{}'",
                NS_FuncNameV, jobId_, jobUser_, jobTitle_, jobOpts_, socket_path);

            auto executor = co_await asio::this_coro::executor;
            std::array<char, 4094> buf;

            // open dst socket
            asio::local::stream_protocol::socket dst_sock{executor};

            co_await dst_sock.async_connect(
                asio::local::stream_protocol::endpoint(socket_path), asio::use_awaitable);

            try {
                // open src stream
                asio::posix::stream_descriptor src_stream{executor, jobFd_};
                for(;;) {
                    size_t rlen = co_await src_stream.async_read_some(asio::buffer(buf), asio::use_awaitable);
                    co_await asio::async_write(dst_sock, asio::buffer(buf.data(), rlen), asio::use_awaitable);
                }
            } catch(const system::system_error& err) {
                if(err.code() == asio::error::eof) {
                    cupsStatus_ = CUPS_BACKEND_OK;
                    co_return;
                }
                throw;
            }
        }

        int status(void) const {
            return cupsStatus_;
        }
    };
}

int main(int argc, const char** argv) {
    if(2 > argc) {
        std::cout << LTSM::backendType << " " << LTSM::backendName << " " <<
                  std::quoted("Unknown") << " " << std::quoted(LTSM::backendDescription) << std::endl;
        return CUPS_BACKEND_OK;
    }

    if(6 > argc || 7 < argc) {
        std::cerr << "Usage: " << argv[0] << " job-id user title copies options [file]" << std::endl;
        return CUPS_BACKEND_FAILED;
    }

    asio::io_context ctx;

    try {
        LTSM::CupsBackend backend(ctx, argc, argv);
        asio::co_spawn(ctx, std::bind(&LTSM::CupsBackend::start, &backend), asio::detached);
        ctx.run();
        return backend.status();
    } catch(const std::exception & err) {
        LTSM::Application::error("exception: {}", err.what());
    }

    return CUPS_BACKEND_FAILED;
}
