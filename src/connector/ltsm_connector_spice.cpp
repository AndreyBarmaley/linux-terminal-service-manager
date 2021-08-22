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

#include "spice/error_codes.h"

#include "ltsm_tools.h"
#include "ltsm_connector_spice.h"

using namespace std::chrono_literals;

namespace LTSM
{
    void Connector::SPICE::linkReplyData(const std::vector<uint32_t> & commonCaps, const std::vector<uint32_t> & channelCaps)
    {
	// https://www.spice-space.org/spice-protocol.html	
	// 11.4 SpiceLinkReply definition
	sendIntLE32(SPICE_MAGIC);
	sendIntLE32(SPICE_VERSION_MAJOR);
	sendIntLE32(SPICE_VERSION_MINOR);
	sendIntLE32(sizeof(SpiceLinkReply) + (commonCaps.size() + channelCaps.size()) * sizeof(uint32_t));
	// SpiceLinkReply
	sendIntLE32(SPICE_LINK_ERR_OK);
	// pub_key
	sendRaw(publicKey.data(), publicKey.size());
	// num_common_caps
	sendIntLE32(commonCaps.size());
	// num_channel_caps
	sendIntLE32(channelCaps.size());
	// caps_offset
	sendIntLE32(publicKey.size() + 16);

	for(auto & word : commonCaps)
	    sendIntLE32(word);

	for(auto & word : channelCaps)
	    sendIntLE32(word);

	sendFlush();
    }

    void Connector::SPICE::linkReplyOk(void)
    {
	linkReplyError(SPICE_LINK_ERR_OK);
    }

    void Connector::SPICE::linkReplyError(int err)
    {
	sendIntLE32(SPICE_MAGIC);
	sendIntLE32(SPICE_VERSION_MAJOR);
	sendIntLE32(SPICE_VERSION_MINOR);
	sendIntLE32(sizeof(SpiceLinkReply));
	// SpiceLinkReply
	sendIntLE32(err);
	// pub_key
	sendRaw(publicKey.data(), publicKey.size());
	// num_common_caps
	sendIntLE32(0);
	// num_channel_caps
	sendIntLE32(0);
	// caps_offset
	sendIntLE32(publicKey.size() + 16);

	sendFlush();
    }

    std::pair<RedLinkMess, bool> Connector::SPICE::recvLinkMess(void)
    {
	// https://www.spice-space.org/spice-protocol.html	
	// 11.3 SpiceLinkMess definition
	RedLinkMess msg;

	int magic = recvIntLE32();
	if(magic != SPICE_MAGIC)
	{
	    linkReplyError(SPICE_LINK_ERR_INVALID_MAGIC);
	    Application::error("handshake failure: 0x%08X", magic);

	    return std::make_pair(msg, false);
	}

	int majorVer = recvIntLE32();
	int minorVer = recvIntLE32();
	if(majorVer != SPICE_VERSION_MAJOR || minorVer != SPICE_VERSION_MINOR)
	{
	    linkReplyError(SPICE_LINK_ERR_VERSION_MISMATCH);
	    Application::error("version mismatch: %d.%d", majorVer, minorVer);
	    return std::make_pair(msg, false);
	}

	int msgSize = recvIntLE32();
	if(msgSize < sizeof(SpiceLinkMess))
	{
	    linkReplyError(SPICE_LINK_ERR_INVALID_DATA);
	    Application::error("msg size failed: %d", msgSize);
	    return std::make_pair(msg, false);
	}

	msg.connectionId = recvIntLE32();
	msg.channelType = recvInt8();
	msg.channelId = recvInt8();

	int numCommonCaps = recvIntLE32();
	int numChannelCaps = recvIntLE32();
	int capsOffset = recvIntLE32();

	if(capsOffset + (numCommonCaps + numChannelCaps) * sizeof(uint32_t) != msgSize)
	{
	    linkReplyError(SPICE_LINK_ERR_INVALID_DATA);
	    Application::error("msg size failed: %d", msgSize);
	    return std::make_pair(msg, false);
	}

        // check data
        if(numCommonCaps > 1024)
        {
	    linkReplyError(SPICE_LINK_ERR_INVALID_DATA);
	    Application::error("huge common caps: %d", numCommonCaps);
	    return std::make_pair(msg, false);
        }

        // check data
        if(numChannelCaps > 1024)
        {
	    linkReplyError(SPICE_LINK_ERR_INVALID_DATA);
	    Application::error("huge common caps: %d", numChannelCaps);
	    return std::make_pair(msg, false);
        }

	for(int num = 0; num < numCommonCaps; ++num)
	    msg.commonCaps.push_back(recvIntLE32());

	for(int num = 0; num < numChannelCaps; ++num)
	    msg.channelCaps.push_back(recvIntLE32());

        Application::info("- connected id: %d\n", msg.connectionId);
        Application::info("- channel type: %d\n", (int) msg.channelType);
        Application::info("- channel id: %d\n", (int) msg.channelId);
        Application::info("- num common caps: %d\n", numCommonCaps);
        Application::info("- num channel caps: %d\n", numChannelCaps);
        Application::info("- caps offset: %d\n", capsOffset);

	return std::make_pair(msg, true);
    }

    /* Connector::SPICE */
    int Connector::SPICE::communication(void)
    {
        Application::info("connected: %s\n", _remoteaddr.c_str());
        //Application::info("using encoding threads: %d", _encodingThreads);

	// wait RedLinkMess
	const auto & [msg, res] = recvLinkMess();
	if(!res)
	    return EXIT_FAILURE;

        // check bus
	if(0 >= busGetServiceVersion())
        {
            Application::error("%s", "bus service failure");
	    linkReplyError(SPICE_LINK_ERR_ERROR);
            return EXIT_FAILURE;
        }

        // debub caps
	for(auto & cap : msg.commonCaps)
	    Application::info("common cap: 0x%08x\n", cap);

	for(auto & cap : msg.channelCaps)
	    Application::info("channel cap: 0x%08x\n", cap);

        // init rsa keys
	int ret = gnutls_privkey_init(& rsaPrivate);
	if(ret < 0)
	{
	    Application::error("gnutls_privkey_init: %s", gnutls_strerror(ret));
	    linkReplyError(SPICE_LINK_ERR_ERROR);
	    return EXIT_FAILURE;
	}

        ret = gnutls_pubkey_init(& rsaPublic);
	if(ret < 0)
	{
	    Application::error("gnutls_pubkey_init: %s", gnutls_strerror(ret));
	    linkReplyError(SPICE_LINK_ERR_ERROR);
	    return EXIT_FAILURE;
	}

        // rsa generate
	ret = gnutls_privkey_generate(rsaPrivate, GNUTLS_PK_RSA, SPICE_TICKET_KEY_PAIR_LENGTH, 0);
	if(ret < 0)
	{
	    Application::error("gnutls_privkey_generate: %s", gnutls_strerror(ret));
	    linkReplyError(SPICE_LINK_ERR_ERROR);
	    return EXIT_FAILURE;
	}

        ret = gnutls_pubkey_import_privkey(rsaPublic, rsaPrivate, 0, 0);
	if(ret < 0)
	{
	    Application::error("gnutls_pubkey_import_privkey: %s", gnutls_strerror(ret));
	    linkReplyError(SPICE_LINK_ERR_ERROR);
	    return EXIT_FAILURE;
	}

        // get public
        size_t bufsz = publicKey.size();
        ret = gnutls_pubkey_export(rsaPublic, GNUTLS_X509_FMT_DER, publicKey.data(), & bufsz);
	if(ret < 0)
	{
	    Application::error("gnutls_pubkey_export: %s, size: %d", gnutls_strerror(ret), bufsz);
	    linkReplyError(SPICE_LINK_ERR_ERROR);
	    return EXIT_FAILURE;
	}

	// send reply
	linkReplyOk();

	// https://www.spice-space.org/spice-protocol.html	
	// 11.5 Encrypted Password
	// Client sends RSA encrypted password, with public key received from server (in SpiceLinkReply).
	// Format is EME-OAEP as described in PKCS#1 v2.0 with SHA-1, MGF1 and an empty encoding parameter.

        while(int tmp = recvInt8())
	{
	    Application::info("recv byte: 0x%02x\n", tmp);
	    if(tmp == 0xffffffff) break;
	}

        Application::debug("under construction, remoteaddr: %s\n", _remoteaddr.c_str());
        return EXIT_SUCCESS;
    }
}
