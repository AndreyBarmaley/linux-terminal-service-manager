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

#include <string>
#include <thread>
#include <iostream>
#include <iterator>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_connector_spice.h"

using namespace std::chrono_literals;

namespace LTSM
{
    const int RED_VERSION_MAJOR	= 1;
    const int RED_VERSION_MINOR	= 0;
    const int RED_CHANNEL_MAIN	= 1;
    const int RED_CHANNEL_DISPLAY	= 2;
    const int RED_CHANNEL_INPUTS	= 3;
    const int RED_CHANNEL_CURSOR	= 4;
    const int RED_CHANNEL_PLAYBACK	= 5;
    const int RED_CHANNEL_RECORD	= 6;

    const int RED_ERROR_OK			= 0;
    const int RED_ERROR_ERROR			= 1;
    const int RED_ERROR_INVALID_MAGIC 		= 2;
    const int RED_ERROR_INVALID_DATA 		= 3;
    const int RED_ERROR_VERSION_MISMATCH	= 4;
    const int RED_ERROR_NEED_SECURED 		= 5;
    const int RED_ERROR_NEED_UNSECURED 		= 6;
    const int RED_ERROR_PERMISSION_DENIED 	= 7;
    const int RED_ERROR_BAD_CONNECTION_ID 	= 8;
    const int RED_ERROR_CHANNEL_NOT_AVAILABLE 	= 9;

    const int RED_WARN_GENERAL	= 0;
    const int RED_INFO_GENERAL	= 0;
    const int RED_TICKET_PUBKEY_BYTES = 162;

    const int RED_MIGRATE		= 1;
    const int RED_MIGRATE_DATA		= 2;
    const int RED_SET_ACK		= 3;
    const int RED_PING			= 4;
    const int RED_WAIT_FOR_CHANNELS	= 5;
    const int RED_DISCONNECTING		= 6;
    const int RED_NOTIFY		= 7;
    const int RED_FIRST_AVAIL_MESSAGE 	= 101;

    const int REDC_ACK_SYNC		= 1;
    const int REDC_ACK			= 2;
    const int REDC_PONG			= 3;
    const int REDC_MIGRATE_FLUSH_MARK	= 4;
    const int REDC_MIGRATE_DATA		= 5;
    const int REDC_DISCONNECTING	= 6;
    const int REDC_FIRST_AVAIL_MESSAGE	= 101;

    const int RED_MAIN_MIGRATE_BEGIN	= 101;
    const int RED_MAIN_MIGRATE_CANCEL	= 102;
    const int RED_MAIN_INIT		= 103;
    const int RED_MAIN_CHANNELS_LIST	= 104;
    const int RED_MAIN_MOUSE_MODE	= 105;
    const int RED_MAIN_MULTI_MEDIA_TIME	= 106;
    const int RED_MAIN_AGENT_CONNECTED	= 107;
    const int RED_MAIN_AGENT_DISCONNECTED= 108;
    const int RED_MAIN_AGENT_DATA	= 109;
    const int RED_MAIN_AGENT_TOKEN	= 110;

    const int REDC_MAIN_RESERVED	= 101;
    const int REDC_MAIN_MIGRATE_READY	= 102;
    const int REDC_MAIN_MIGRATE_ERROR	= 103;
    const int REDC_MAIN_ATTACH_CHANNELS	= 104;
    const int REDC_MAIN_MOUSE_MODE_REQUEST= 105;
    const int REDC_MAIN_AGENT_START	= 106;
    const int REDC_MAIN_AGENT_DATA	= 107;
    const int REDC_MAIN_AGENT_TOKEN	= 108;

    const int RED_MOUSE_MODE_SERVER	= 1;
    const int RED_MOUSE_MODE_CLIENT	= 2;

    const int REDC_INPUTS_KEY_DOWN	= 101;
    const int REDC_INPUTS_KEY_UP	= 102;
    const int REDC_INPUTS_KEY_MODIFAIERS= 103;
    const int REDC_INPUTS_MOUSE_MOTION	= 111;
    const int REDC_INPUTS_MOUSE_POSITION= 112;
    const int REDC_INPUTS_MOUSE_PRESS	= 113;
    const int REDC_INPUTS_MOUSE_RELEASE = 114;

    const int RED_INPUTS_INIT		= 101;
    const int RED_INPUTS_KEY_MODIFAIERS	= 102;
    const int RED_INPUTS_MOUSE_MOTION_ACK=111;


/*
    RedLinkMess::RedLinkMess() : magic(0), majorVer(0), minorVer(0), size(0), connectionId(0), channelType(0), 
	channelId(0), numCommonCaps(0), numChannelCaps(0), capsOffset(0)
    {
    }


    RedLinkReply::RedLinkReply() : magic(0), majorVer(0), minorVer(0), size(0), error(0),
	numCommonCaps(0), numChannelCaps(0), capsOffset(0)
    {
	std::fill(std::begin(pubKey), std::end(pubKey), 0);
    }
*/

    /* Connector::SPICE */
    int Connector::SPICE::communication(void)
    {
	if(0 >= busGetServiceVersion())
        {
            Application::error("%s", "bus service failure");
            return EXIT_FAILURE;
        }

        const std::string remoteaddr = Tools::getenv("REMOTE_ADDR", "local");
        Application::info("connected: %s\n", remoteaddr.c_str());
        Application::info("using encoding threads: %d", _encodingThreads);

	// wait RedLinkMess
	int redMagick = recvIntBE32();
	if(redMagick != 0x52454451)
	{
	    Application::error("handshake failure: 0x%08X", redMagick);
	    return EXIT_FAILURE;
	}

	int redMajorVer = recvIntLE32();
	int redMinorVer = recvIntLE32();
	if(redMajorVer != RED_VERSION_MAJOR || redMinorVer != RED_VERSION_MINOR)
	{
	    Application::error("version mismatch: %d.%d", redMajorVer, redMinorVer);
	    return EXIT_FAILURE;
	}

	int redNextSize = recvIntLE32();
        Application::info("red next size: %d\n", redNextSize);

        Application::debug("under construction, remoteaddr: %s\n", remoteaddr.c_str());
        return EXIT_SUCCESS;
    }
}
