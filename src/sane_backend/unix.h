/* sane - Scanner Access Now Easy.
   Copyright (C) 1997 David Mosberger-Tang
   Copyright (C) 2003 Julien BLACHE <jb@jblache.org>
   Copyright (C) 2022 Andrey Afletdinov <public.irkutsk@gmail.com>

   This file is part of the LTSM sane_backend package.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#ifndef unix_h
#define unix_h

#include <sys/types.h>
#include <sys/socket.h>

#include "../include/sane/sanei_wire.h"
#include "../include/sane/config.h"

typedef struct Unix_Device
{
    struct Unix_Device *next;
    const char *name;
    struct sockaddr addr;
    int ctl; /* socket descriptor (or -1) */
    Wire wire;
    int auth_active;
}

Unix_Device;

typedef struct Unix_Scanner
{
    /* all the state needed to define a scan request: */
    struct Unix_Scanner *next;

    int options_valid; /* are the options current? */
    SANE_Option_Descriptor_Array opt, local_opt;

    SANE_Word handle; /* remote handle (it's a word, not a ptr!) */

    int data; /* data socket descriptor */
    int reclen_buf_offset;
    u_char reclen_buf[4];
    size_t bytes_remaining; /* how many bytes left in this record? */

    /* device (host) info: */
    Unix_Device *hw;
}

Unix_Scanner;

#endif /* unix_h */
