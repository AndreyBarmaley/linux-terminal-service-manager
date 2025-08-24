/***************************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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
 **************************************************************************/

#include <cstdio>
#include "winscard.h"

namespace
{
    char hexbuf[11];
}

const char* pcsc_stringify_error(int err)
{
    switch(err)
    {
	case ERROR_BROKEN_PIPE: return "ERROR_BROKEN_PIPE";
	case SCARD_E_BAD_SEEK: return "SCARD_E_BAD_SEEK";
	case SCARD_E_CANCELLED: return "SCARD_E_CANCELLED";
	case SCARD_E_CANT_DISPOSE: return "SCARD_E_CANT_DISPOSE";
	case SCARD_E_CARD_UNSUPPORTED: return "SCARD_E_CARD_UNSUPPORTED";
	case SCARD_E_CERTIFICATE_UNAVAILABLE: return "SCARD_E_CERTIFICATE_UNAVAILABLE";
	case SCARD_E_COMM_DATA_LOST: return "SCARD_E_COMM_DATA_LOST";
	case SCARD_E_DIR_NOT_FOUND: return "SCARD_E_DIR_NOT_FOUND";
	case SCARD_E_DUPLICATE_READER: return "SCARD_E_DUPLICATE_READER";
	case SCARD_E_FILE_NOT_FOUND: return "SCARD_E_FILE_NOT_FOUND";
	case SCARD_E_ICC_CREATEORDER: return "SCARD_E_ICC_CREATEORDER";
	case SCARD_E_ICC_INSTALLATION: return "SCARD_E_ICC_INSTALLATION";
	case SCARD_E_INSUFFICIENT_BUFFER: return "SCARD_E_INSUFFICIENT_BUFFER";
	case SCARD_E_INVALID_ATR: return "SCARD_E_INVALID_ATR";
	case SCARD_E_INVALID_CHV: return "SCARD_E_INVALID_CHV";
	case SCARD_E_INVALID_HANDLE: return "SCARD_E_INVALID_HANDLE";
	case SCARD_E_INVALID_PARAMETER: return "SCARD_E_INVALID_PARAMETER";
	case SCARD_E_INVALID_TARGET: return "SCARD_E_INVALID_TARGET";
	case SCARD_E_INVALID_VALUE: return "SCARD_E_INVALID_VALUE";
	case SCARD_E_NO_ACCESS: return "SCARD_E_NO_ACCESS";
	case SCARD_E_NO_DIR: return "SCARD_E_NO_DIR";
	case SCARD_E_NO_FILE: return "SCARD_E_NO_FILE";
	case SCARD_E_NO_KEY_CONTAINER: return "SCARD_E_NO_KEY_CONTAINER";
	case SCARD_E_NO_MEMORY: return "SCARD_E_NO_MEMORY";
	case SCARD_E_NO_PIN_CACHE: return "SCARD_E_NO_PIN_CACHE";
	case SCARD_E_NO_READERS_AVAILABLE: return "SCARD_E_NO_READERS_AVAILABLE";
	case SCARD_E_NO_SERVICE: return "SCARD_E_NO_SERVICE";
	case SCARD_E_NO_SMARTCARD: return "SCARD_E_NO_SMARTCARD";
	case SCARD_E_NO_SUCH_CERTIFICATE: return "SCARD_E_NO_SUCH_CERTIFICATE";
	case SCARD_E_NOT_READY: return "SCARD_E_NOT_READY";
	case SCARD_E_NOT_TRANSACTED: return "SCARD_E_NOT_TRANSACTED";
	case SCARD_E_PCI_TOO_SMALL: return "SCARD_E_PCI_TOO_SMALL";
	case SCARD_E_PIN_CACHE_EXPIRED: return "SCARD_E_PIN_CACHE_EXPIRED";
	case SCARD_E_PROTO_MISMATCH: return "SCARD_E_PROTO_MISMATCH";
	case SCARD_E_READ_ONLY_CARD: return "SCARD_E_READ_ONLY_CARD";
	case SCARD_E_READER_UNAVAILABLE: return "SCARD_E_READER_UNAVAILABLE";
	case SCARD_E_READER_UNSUPPORTED: return "SCARD_E_READER_UNSUPPORTED";
	case SCARD_E_SERVER_TOO_BUSY: return "SCARD_E_SERVER_TOO_BUSY";
	case SCARD_E_SERVICE_STOPPED: return "SCARD_E_SERVICE_STOPPED";
	case SCARD_E_SHARING_VIOLATION: return "SCARD_E_SHARING_VIOLATION";
	case SCARD_E_SYSTEM_CANCELLED: return "SCARD_E_SYSTEM_CANCELLED";
	case SCARD_E_TIMEOUT: return "SCARD_E_TIMEOUT";
	case SCARD_E_UNEXPECTED: return "SCARD_E_UNEXPECTED";
	case SCARD_E_UNKNOWN_CARD: return "SCARD_E_UNKNOWN_CARD";
	case SCARD_E_UNKNOWN_READER: return "SCARD_E_UNKNOWN_READER";
	case SCARD_E_UNKNOWN_RES_MNG: return "SCARD_E_UNKNOWN_RES_MNG";
	case SCARD_E_UNSUPPORTED_FEATURE: return "SCARD_E_UNSUPPORTED_FEATURE";
	case SCARD_E_WRITE_TOO_MANY: return "SCARD_E_WRITE_TOO_MANY";
	case SCARD_F_COMM_ERROR: return "SCARD_F_COMM_ERROR";
	case SCARD_F_INTERNAL_ERROR: return "SCARD_F_INTERNAL_ERROR";
	case SCARD_F_UNKNOWN_ERROR: return "SCARD_F_UNKNOWN_ERROR";
	case SCARD_F_WAITED_TOO_LONG: return "SCARD_F_WAITED_TOO_LONG";
	case SCARD_P_SHUTDOWN: return "SCARD_P_SHUTDOWN";
	case SCARD_S_SUCCESS: return "SCARD_S_SUCCESS";
	case SCARD_W_CANCELLED_BY_USER: return "SCARD_W_CANCELLED_BY_USER";
	case SCARD_W_CACHE_ITEM_NOT_FOUND: return "SCARD_W_CACHE_ITEM_NOT_FOUND";
	case SCARD_W_CACHE_ITEM_STALE: return "SCARD_W_CACHE_ITEM_STALE";
	case SCARD_W_CACHE_ITEM_TOO_BIG: return "SCARD_W_CACHE_ITEM_TOO_BIG";
	case SCARD_W_CARD_NOT_AUTHENTICATED: return "SCARD_W_CARD_NOT_AUTHENTICATED";
	case SCARD_W_CHV_BLOCKED: return "SCARD_W_CHV_BLOCKED";
	case SCARD_W_EOF: return "SCARD_W_EOF";
	case SCARD_W_REMOVED_CARD: return "SCARD_W_REMOVED_CARD";
	case SCARD_W_RESET_CARD: return "SCARD_W_RESET_CARD";
	case SCARD_W_SECURITY_VIOLATION: return "SCARD_W_SECURITY_VIOLATION";
	case SCARD_W_UNPOWERED_CARD: return "SCARD_W_UNPOWERED_CARD";
	case SCARD_W_UNRESPONSIVE_CARD: return "SCARD_W_UNRESPONSIVE_CARD";
	case SCARD_W_UNSUPPORTED_CARD: return "SCARD_W_UNSUPPORTED_CARD";
	case SCARD_W_WRONG_CHV: return "SCARD_W_WRONG_CHV";
        default: break;
    }
    
    snprintf(hexbuf, sizeof(hexbuf), "0x%08x", err);
    hexbuf[sizeof(hexbuf)-1] = 0;
    
    return hexbuf;
}
