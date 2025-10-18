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

#include <unistd.h>

#include <thread>
#include <chrono>
#include <fstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <exception>
#include <filesystem>

#include "cups/backend.h"

#include "ltsm_tools.h"
#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_application.h"

using namespace std::chrono_literals;

/*
    CUPS_BACKEND_OK, CUPS_BACKEND_FAILED, CUPS_BACKEND_AUTH_REQUIRED, CUPS_BACKEND_HOLD,
    CUPS_BACKEND_STOP, CUPS_BACKEND_CANCEL, CUPS_BACKEND_RETRY, CUPS_BACKEND_RETRY_CURRENT
*/

namespace LTSM {
    const size_t blocksz = 4096;

    auto backendType = "direct";
    auto backendName = "ltsm";
    auto backendDescription = "LTSM Virtual Print";

    class CupsBackend : public Application {
        int jobId = 0;
        int jobNumPage = 0;

        std::string jobUser;
        std::string jobTitle;
        std::string jobOpts;
        std::string jobFile;

      public:

        CupsBackend(int argc, const char** argv) : Application("ltsm_cups") {
            setDebug(DebugTarget::Syslog, DebugLevel::Info);

            // job from stdin
            if(argc == 6) {
                jobId = std::stoi(argv[1]);
                jobUser = argv[2];
                jobTitle = argv[3];
                jobNumPage = std::stoi(argv[4]);
                jobOpts = argv[5];
            } else {
                jobId = std::stoi(argv[1]);
                jobUser = argv[2];
                jobTitle = argv[3];
                jobNumPage = std::stoi(argv[4]);
                jobOpts = argv[5];
                jobFile = argv[6];
            }
        }

        int readWriteStream(std::istream* is, int fd, size_t delay) {
            std::vector<uint8_t> buf(blocksz);

            while(*is) {
                is->read(reinterpret_cast<char*>(buf.data()), buf.size());

                try {
                    DescriptorStream::writeFromTo(buf.data(), is->gcount(), fd);
                } catch(const std::exception & err) {
                    Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
                    return CUPS_BACKEND_HOLD;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            return CUPS_BACKEND_OK;
        }

        int start(void) {
            Application::info("%s: get uid: %d, get gid: %d", __FUNCTION__, getuid(), getgid());
            std::string socketFormat = "/var/run/ltsm/cups/printer_username";

            if(auto deviceURI = getenv("DEVICE_URI")) {
                if(auto ptr = std::strstr(deviceURI, "://")) {
                    socketFormat = ptr + 3;
                }
            }

            std::filesystem::path socketPath = Tools::replace(socketFormat, "username", jobUser);

            if(! std::filesystem::is_socket(socketPath)) {
                Application::error("%s: socket not found: %s", __FUNCTION__, socketPath.c_str());
                return CUPS_BACKEND_HOLD;
            }

            if(0 != access(socketPath.c_str(), W_OK)) {
                Application::error("%s: write access failed, socket: %s", __FUNCTION__, socketPath.c_str());
                return CUPS_BACKEND_HOLD;
            }

            auto sock = UnixSocket::connect(socketPath);

            if(0 > sock) {
                return CUPS_BACKEND_HOLD;
            }

            // wait for data from the stdin
            if(jobFile.empty()) {
                return readWriteStream(& std::cin, sock, 75);
            }

            std::ifstream ifs(jobFile, std::ifstream::in | std::ifstream::binary);

            if(! ifs.is_open()) {
                return CUPS_BACKEND_CANCEL;
            }

            return readWriteStream(& ifs, sock, 75);
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

    try {
        return LTSM::CupsBackend(argc, argv).start();
    } catch(const std::exception & err) {
        std::cerr << "exception: " << err.what() << std::endl;
    }

    return EXIT_FAILURE;
}
