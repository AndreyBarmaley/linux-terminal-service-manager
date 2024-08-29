/* sane - Scanner Access Now Easy.
   Copyright (C) 1997 David Mosberger-Tang
   Copyright (C) 2003, 2008 Julien BLACHE <jb@jblache.org>
   Copyright (C) 2022, Andrey Afletdinov <public.irkutsk@gmail.com>

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

#include "../include/sane/config.h"
#include "../include/lalloca.h"
#include "../include/_stdint.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netdb.h> /* OS/2 needs this _after_ <netinet/in.h>, grrr... */

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/sanei_net.h"
#include "../include/sane/sanei_codec_bin.h"
#include "unix.h"

#define BACKEND_NAME    unix
#include "../include/sane/sanei_backend.h"

#ifndef PATH_MAX
# define PATH_MAX       1024
#endif

#include "../include/sane/sanei_config.h"
#define UNIX_CONFIG_FILE "unix.conf"

/* Please increase version number with every change
   (don't forget to update net.desc) */

/* define the version string depending on which network code is used */
#define UNIX_VERSION "1.0.0"

static SANE_Auth_Callback auth_callback;
static Unix_Device *first_device;
static Unix_Scanner *first_handle;
static const SANE_Device **devlist;
static int client_big_endian; /* 1 == big endian; 0 == little endian */
static int server_big_endian; /* 1 == big endian; 0 == little endian */
static int depth; /* bits per pixel */

/* This variable is only needed, if the depth is 16bit/channel and
   client/server have different endianness.  A value of -1 means, that there's
   no hang over; otherwise the value has to be casted to SANE_Byte.  hang_over
   means, that there is a remaining byte from a previous call to sane_read,
   which could not be byte-swapped, e.g. because the frontend requested an odd
   number of bytes.
*/
static int hang_over;

/* This variable is only needed, if the depth is 16bit/channel and
   client/server have different endianness.  A value of -1 means, that there's
   no left over; otherwise the value has to be casted to SANE_Byte.  left_over
   means, that there is a remaining byte from a previous call to sane_read,
   which already is in the correct byte order, but could not be returned,
   e.g.  because the frontend requested only one byte per call.
*/
static int left_over;

static SANE_Status
add_device (const char* path, Unix_Device** ndp)
{
    Unix_Device *nd;
    struct sockaddr_un *sun;

    DBG (1, "add_device: adding backend %s\n", path);

    for (nd = first_device; nd; nd = nd->next)
        if (strcmp (nd->path, path) == 0)
        {
            DBG (1, "add_device: already in list\n");

            if (ndp)
            {
                *ndp = nd;
            }

            return SANE_STATUS_GOOD;
        }

    nd = malloc (sizeof (*nd));

    if (!nd)
    {
        DBG (1, "add_device: not enough memory for Unix_Device struct\n");
        return SANE_STATUS_NO_MEM;
    }

    memset (nd, 0, sizeof (*nd));
    nd->path = strdup (path);

    if (!nd->path)
    {
        DBG (1, "add_device: not enough memory to duplicate name\n");
        free (nd);
        return SANE_STATUS_NO_MEM;
    }

    nd->addr.sun_family = AF_UNIX;

    sun = (struct sockaddr_un*) &nd->addr;
    memcpy (sun->sun_path, path, strlen(path));

    nd->ctl = -1;
    nd->next = first_device;
    first_device = nd;

    if (ndp)
    {
        *ndp = nd;
    }

    DBG (2, "add_device: backend %s added\n", path);
    return SANE_STATUS_GOOD;
}

static SANE_Status
connect_dev (Unix_Device* dev)
{
    SANE_Word version_code;
    SANE_Init_Reply reply;
    SANE_Status status = SANE_STATUS_IO_ERROR;
    SANE_Init_Req req;

    DBG (2, "connect_dev: trying to connect to %s\n", dev->path);

    if (dev->addr.sun_family != AF_UNIX)
    {
        DBG (1, "connect_dev: don't know how to deal with addr family %d\n",
             dev->addr.sun_family);
        return SANE_STATUS_IO_ERROR;
    }

    dev->ctl = socket (dev->addr.sun_family, SOCK_STREAM, 0);

    if (dev->ctl < 0)
    {
        DBG (1, "connect_dev: failed to obtain socket (%s)\n",
             strerror (errno));
        dev->ctl = -1;
        return SANE_STATUS_IO_ERROR;
    }

    if (connect (dev->ctl, &dev->addr, sizeof (dev->addr)) < 0)
    {
        DBG (1, "connect_dev: failed to connect (%s)\n", strerror (errno));
        dev->ctl = -1;
        return SANE_STATUS_IO_ERROR;
    }

    DBG (3, "connect_dev: connection succeeded\n");

    DBG (2, "connect_dev: sanei_w_init\n");
    sanei_w_init (&dev->wire, sanei_codec_bin_init);
    dev->wire.io.fd = dev->ctl;
    dev->wire.io.read = read;
    dev->wire.io.write = write;

    /* exchange version codes with the server: */
    req.version_code = SANE_VERSION_CODE (V_MAJOR, V_MINOR,
                                          SANEI_NET_PROTOCOL_VERSION);
    req.username = getlogin ();
    DBG (2, "connect_dev: unix_init (user=%s, local version=%d.%d.%d)\n",
         req.username, V_MAJOR, V_MINOR, SANEI_NET_PROTOCOL_VERSION);
    sanei_w_call (&dev->wire, SANE_NET_INIT,
                  (WireCodecFunc) sanei_w_init_req, &req,
                  (WireCodecFunc) sanei_w_init_reply, &reply);

    if (dev->wire.status != 0)
    {
        DBG (1, "connect_dev: argument marshalling error (%s)\n",
             strerror (dev->wire.status));
        status = SANE_STATUS_IO_ERROR;
        goto fail;
    }

    status = reply.status;
    version_code = reply.version_code;
    DBG (2, "connect_dev: freeing init reply (status=%s, remote "
         "version=%d.%d.%d)\n", sane_strstatus (status),
         SANE_VERSION_MAJOR (version_code),
         SANE_VERSION_MINOR (version_code), SANE_VERSION_BUILD (version_code));
    sanei_w_free (&dev->wire, (WireCodecFunc) sanei_w_init_reply, &reply);

    if (status != 0)
    {
        DBG (1, "connect_dev: access to %s denied\n", dev->path);
        goto fail;
    }

    if (SANE_VERSION_MAJOR (version_code) != V_MAJOR)
    {
        DBG (1, "connect_dev: major version mismatch: got %d, expected %d\n",
             SANE_VERSION_MAJOR (version_code), V_MAJOR);
        status = SANE_STATUS_IO_ERROR;
        goto fail;
    }

    if (SANE_VERSION_BUILD (version_code) != SANEI_NET_PROTOCOL_VERSION
            && SANE_VERSION_BUILD (version_code) != 2)
    {
        DBG (1, "connect_dev: network protocol version mismatch: "
             "got %d, expected %d\n",
             SANE_VERSION_BUILD (version_code), SANEI_NET_PROTOCOL_VERSION);
        status = SANE_STATUS_IO_ERROR;
        goto fail;
    }

    dev->wire.version = SANE_VERSION_BUILD (version_code);
    DBG (4, "connect_dev: done\n");
    return SANE_STATUS_GOOD;

fail:
    DBG (2, "connect_dev: closing connection to %s\n", dev->path);
    close (dev->ctl);
    dev->ctl = -1;
    return status;
}


static SANE_Status
fetch_options (Unix_Scanner* s)
{
    int option_number;
    DBG (3, "fetch_options: %p\n", (void*) s);

    if (s->opt.num_options)
    {
        DBG (2, "fetch_options: %d option descriptors cached... freeing\n",
             s->opt.num_options);
        sanei_w_set_dir (&s->hw->wire, WIRE_FREE);
        s->hw->wire.status = 0;
        sanei_w_option_descriptor_array (&s->hw->wire, &s->opt);

        if (s->hw->wire.status)
        {
            DBG (1, "fetch_options: failed to free old list (%s)\n",
                 strerror (s->hw->wire.status));
            return SANE_STATUS_IO_ERROR;
        }
    }

    DBG (3, "fetch_options: get_option_descriptors\n");
    sanei_w_call (&s->hw->wire, SANE_NET_GET_OPTION_DESCRIPTORS,
                  (WireCodecFunc) sanei_w_word, &s->handle,
                  (WireCodecFunc) sanei_w_option_descriptor_array, &s->opt);

    if (s->hw->wire.status)
    {
        DBG (1, "fetch_options: failed to get option descriptors (%s)\n",
             strerror (s->hw->wire.status));
        return SANE_STATUS_IO_ERROR;
    }

    if (s->local_opt.num_options == 0)
    {
        DBG (3, "fetch_options: creating %d local option descriptors\n",
             s->opt.num_options);
        s->local_opt.desc =
            malloc (s->opt.num_options* sizeof (s->local_opt.desc));

        if (!s->local_opt.desc)
        {
            DBG (1, "fetch_options: couldn't malloc s->local_opt.desc\n");
            return SANE_STATUS_NO_MEM;
        }

        for (option_number = 0;
                option_number < s->opt.num_options;
                option_number++)
        {
            s->local_opt.desc[option_number] =
                malloc (sizeof (SANE_Option_Descriptor));

            if (!s->local_opt.desc[option_number])
            {
                DBG (1, "fetch_options: couldn't malloc "
                     "s->local_opt.desc[%d]\n", option_number);
                return SANE_STATUS_NO_MEM;
            }
        }

        s->local_opt.num_options = s->opt.num_options;
    }
    else if (s->local_opt.num_options != s->opt.num_options)
    {
        DBG (1, "fetch_options: option number count changed during runtime?\n");
        return SANE_STATUS_INVAL;
    }

    DBG (3, "fetch_options: copying %d option descriptors\n",
         s->opt.num_options);

    for (option_number = 0; option_number < s->opt.num_options; option_number++)
    {
        memcpy (s->local_opt.desc[option_number], s->opt.desc[option_number],
                sizeof (SANE_Option_Descriptor));
    }

    s->options_valid = 1;
    DBG (3, "fetch_options: %d options fetched\n", s->opt.num_options);
    return SANE_STATUS_GOOD;
}

static SANE_Status
do_cancel (Unix_Scanner* s)
{
    DBG (2, "do_cancel: %p\n", (void*) s);
    s->hw->auth_active = 0;

    if (s->data >= 0)
    {
        DBG (3, "do_cancel: closing data pipe\n");
        close (s->data);
        s->data = -1;
    }

    return SANE_STATUS_CANCELLED;
}

static void
do_authorization (Unix_Device* dev, SANE_String resource)
{
    SANE_Authorization_Req req;
    SANE_Char username[SANE_MAX_USERNAME_LEN];
    SANE_Char password[SANE_MAX_PASSWORD_LEN];
    char *unix_resource;

    DBG (2, "do_authorization: dev=%p resource=%s\n", (void*) dev, resource);

    dev->auth_active = 1;

    memset (&req, 0, sizeof (req));
    memset (username, 0, sizeof (SANE_Char) * SANE_MAX_USERNAME_LEN);
    memset (password, 0, sizeof (SANE_Char) * SANE_MAX_PASSWORD_LEN);

    unix_resource = malloc (strlen (resource) + 6 + strlen (dev->path));

    if (unix_resource != NULL)
    {
        sprintf (unix_resource, "unix:%s:%s", dev->path, resource);

        if (auth_callback)
        {
            DBG (2, "do_authorization: invoking auth_callback, resource = %s\n",
                 unix_resource);
            (*auth_callback) (unix_resource, username, password);
        }
        else
        {
            DBG (1, "do_authorization: no auth_callback present\n");
        }

        free (unix_resource);
    }
    else /* Is this necessary? If we don't have these few bytes we will get
      in trouble later anyway */
    {
        DBG (1, "do_authorization: not enough memory for unix_resource\n");

        if (auth_callback)
        {
            DBG (2, "do_authorization: invoking auth_callback, resource = %s\n",
                 resource);
            (*auth_callback) (resource, username, password);
        }
        else
        {
            DBG (1, "do_authorization: no auth_callback present\n");
        }
    }

    if (dev->auth_active)
    {
        SANE_Word ack;

        req.resource = resource;
        req.username = username;
        req.password = password;
        DBG (2, "do_authorization: relaying authentication data\n");
        sanei_w_call (&dev->wire, SANE_NET_AUTHORIZE,
                      (WireCodecFunc) sanei_w_authorization_req, &req,
                      (WireCodecFunc) sanei_w_word, &ack);
    }
    else
    {
        DBG (1, "do_authorization: auth_active is false... strange\n");
    }
}

SANE_Status
sane_init (SANE_Int* version_code, SANE_Auth_Callback authorize)
{
    char unix_path[PATH_MAX];
    //const char *optval;
    const char *env;
    size_t len;
    FILE *fp;
    short ns = 0x1234;
    unsigned char *p = (unsigned char*)(&ns);

    DBG_INIT ();

    DBG (2, "sane_init: authorize %s null, version_code %s null\n", (authorize) ? "!=" : "==",
         (version_code) ? "!=" : "==");

    devlist = NULL;
    first_device = NULL;
    first_handle = NULL;

    auth_callback = authorize;

    /* Return the version number of the sane-backends package to allow
       the frontend to print them. This is done only for net and dll,
       because these backends are usually called by the frontend. */
    if (version_code)
        *version_code = SANE_VERSION_CODE (SANE_DLL_V_MAJOR, SANE_DLL_V_MINOR,
                                           SANE_DLL_V_BUILD);

    DBG (1, "sane_init: SANE unix backend version %s from %s\n", UNIX_VERSION,
         PACKAGE_STRING);

    /* determine (client) machine byte order */
    if (*p == 0x12)
    {
        client_big_endian = 1;
        DBG (3, "sane_init: Client has big endian byte order\n");
    }
    else
    {
        client_big_endian = 0;
        DBG (3, "sane_init: Client has little endian byte order\n");
    }

    DBG (2, "sane_init: searching for config file\n");
    fp = sanei_config_open (UNIX_CONFIG_FILE);

    if (fp)
    {
        while (sanei_config_read (unix_path, sizeof (unix_path), fp))
        {
            if (unix_path[0] == '#') /* ignore line comments */
            {
                continue;
            }

            len = strlen (unix_path);

            if (!len)
            {
                continue; /* ignore empty lines */
            }

            /*
             * Check for unix backend options.
             * Anything that isn't an option is a saned host.
             */
            DBG (2, "sane_init: trying to add %s\n", unix_path);
            add_device (unix_path, 0);
            break;
        }

        fclose (fp);
        DBG (2, "sane_init: done reading config\n");
    }
    else
        DBG (1, "sane_init: could not open config file (%s): %s\n",
             UNIX_CONFIG_FILE, strerror (errno));

    DBG (2, "sane_init: evaluating environment variable SANE_UNIX_PATH\n");
    env = getenv ("SANE_UNIX_PATH");

    if (env)
    {
        add_device (env, 0);
    }

    DBG (2, "sane_init: done\n");
    return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
    Unix_Scanner *handle, *next_handle;
    Unix_Device *dev, *next_device;
    int i;

    DBG (1, "sane_exit: exiting\n");

    /* first, close all handles: */
    for (handle = first_handle; handle; handle = next_handle)
    {
        next_handle = handle->next;
        sane_close (handle);
    }

    first_handle = 0;

    /* now close all devices: */
    for (dev = first_device; dev; dev = next_device)
    {
        next_device = dev->next;

        DBG (2, "sane_exit: closing dev %p, ctl=%d\n", (void*) dev, dev->ctl);

        if (dev->ctl >= 0)
        {
            sanei_w_call (&dev->wire, SANE_NET_EXIT,
                          (WireCodecFunc) sanei_w_void, 0,
                          (WireCodecFunc) sanei_w_void, 0);
            sanei_w_exit (&dev->wire);
            close (dev->ctl);
        }

        if (dev->path)
        {
            free ((void*) dev->path);
        }

        free (dev);
    }

    if (devlist)
    {
        for (i = 0; devlist[i]; ++i)
        {
            if (devlist[i]->vendor)
            {
                free ((void*) devlist[i]->vendor);
            }

            if (devlist[i]->model)
            {
                free ((void*) devlist[i]->model);
            }

            if (devlist[i]->type)
            {
                free ((void*) devlist[i]->type);
            }

            free ((void*) devlist[i]);
        }

        free (devlist);
    }

    DBG (3, "sane_exit: finished.\n");
}

/* Note that a call to get_devices() implies that we'll have to
   connect to all remote hosts.  To avoid this, you can call
   sane_open() directly (assuming you know the name of the
   backend/device).  This is appropriate for the command-line
   interface of SANE, for example.
 */
SANE_Status
sane_get_devices (const SANE_Device*** device_list, SANE_Bool local_only)
{
    static int devlist_size = 0, devlist_len = 0;
    static const SANE_Device *empty_devlist[1] = { 0 };
    SANE_Get_Devices_Reply reply;
    SANE_Status status;
    Unix_Device *dev;
    char *full_name;
    int i, num_devs;
    size_t len;
#define ASSERT_SPACE(n)                                                    \
  {                                                                        \
    if (devlist_len + (n) > devlist_size)                                  \
      {                                                                    \
        devlist_size += (n) + 15;                                          \
        if (devlist)                                                       \
          devlist = realloc (devlist, devlist_size * sizeof (devlist[0])); \
        else                                                               \
          devlist = malloc (devlist_size * sizeof (devlist[0]));           \
        if (!devlist)                                                      \
          {                                                                \
             DBG (1, "sane_get_devices: not enough memory\n");             \
             return SANE_STATUS_NO_MEM;                                    \
          }                                                                \
      }                                                                    \
  }

    DBG (3, "sane_get_devices: local_only = %d\n", local_only);

    if (local_only)
    {
        *device_list = empty_devlist;
        return SANE_STATUS_GOOD;
    }

    if (devlist)
    {
        DBG (2, "sane_get_devices: freeing devlist\n");

        for (i = 0; devlist[i]; ++i)
        {
            if (devlist[i]->vendor)
            {
                free ((void*) devlist[i]->vendor);
            }

            if (devlist[i]->model)
            {
                free ((void*) devlist[i]->model);
            }

            if (devlist[i]->type)
            {
                free ((void*) devlist[i]->type);
            }

            free ((void*) devlist[i]);
        }

        free (devlist);
        devlist = 0;
    }

    devlist_len = 0;
    devlist_size = 0;

    for (dev = first_device; dev; dev = dev->next)
    {
        if (dev->ctl < 0)
        {
            status = connect_dev (dev);

            if (status != SANE_STATUS_GOOD)
            {
                DBG (1, "sane_get_devices: ignoring failure to connect to %s\n",
                     dev->path);
                continue;
            }
        }

        sanei_w_call (&dev->wire, SANE_NET_GET_DEVICES,
                      (WireCodecFunc) sanei_w_void, 0,
                      (WireCodecFunc) sanei_w_get_devices_reply, &reply);

        if (reply.status != SANE_STATUS_GOOD)
        {
            DBG (1, "sane_get_devices: ignoring rpc-returned status %s\n",
                 sane_strstatus (reply.status));
            sanei_w_free (&dev->wire,
                          (WireCodecFunc) sanei_w_get_devices_reply, &reply);
            continue;
        }

        /* count the number of devices for this backend: */
        for (num_devs = 0; reply.device_list[num_devs]; ++num_devs);

        ASSERT_SPACE (num_devs);

        for (i = 0; i < num_devs; ++i)
        {
            SANE_Device *rdev;
            char *mem;

            /* create a new device entry with a device name that is the
               sum of the backend name a colon and the backend's device
               name: */
            len = strlen (dev->path) + 1 + strlen (reply.device_list[i]->name);

            mem = malloc (sizeof (*dev) + len + 1);

            if (!mem)
            {
                DBG (1, "sane_get_devices: not enough free memory\n");
                sanei_w_free (&dev->wire,
                              (WireCodecFunc) sanei_w_get_devices_reply,
                              &reply);
                return SANE_STATUS_NO_MEM;
            }

            memset (mem, 0, sizeof (*dev) + len);
            full_name = mem + sizeof (*dev);

            strcat (full_name, dev->path);

            strcat (full_name, ":");
            strcat (full_name, reply.device_list[i]->name);
            DBG (3, "sane_get_devices: got %s\n", full_name);

            rdev = (SANE_Device*) mem;
            rdev->name = full_name;
            rdev->vendor = strdup (reply.device_list[i]->vendor);
            rdev->model = strdup (reply.device_list[i]->model);
            rdev->type = strdup (reply.device_list[i]->type);

            if ((!rdev->vendor) || (!rdev->model) || (!rdev->type))
            {
                DBG (1, "sane_get_devices: not enough free memory\n");

                if (rdev->vendor)
                {
                    free ((void*) rdev->vendor);
                }

                if (rdev->model)
                {
                    free ((void*) rdev->model);
                }

                if (rdev->type)
                {
                    free ((void*) rdev->type);
                }

                free (rdev);
                sanei_w_free (&dev->wire,
                              (WireCodecFunc) sanei_w_get_devices_reply,
                              &reply);
                return SANE_STATUS_NO_MEM;
            }

            devlist[devlist_len++] = rdev;
        }

        /* now free up the rpc return value: */
        sanei_w_free (&dev->wire,
                      (WireCodecFunc) sanei_w_get_devices_reply, &reply);
    }

    /* terminate device list with NULL entry: */
    ASSERT_SPACE (1);
    devlist[devlist_len++] = 0;

    *device_list = devlist;
    DBG (2, "sane_get_devices: finished (%d devices)\n", devlist_len - 1);
    return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const full_name, SANE_Handle* meta_handle)
{
    SANE_Open_Reply reply;
    const char *dev_name;
    SANE_String nd_name;
    SANE_Status status;
    SANE_Word handle;
    SANE_Word ack;
    Unix_Device *dev;
    Unix_Scanner *s;
    int need_auth;

    DBG (3, "sane_open(\"%s\")\n", full_name);

    dev_name = strchr (full_name, ':');

    if (dev_name)
    {
#ifdef strndupa
        nd_name = strndupa (full_name, dev_name - full_name);

        if (!nd_name)
        {
            DBG (1, "sane_open: not enough free memory\n");
            return SANE_STATUS_NO_MEM;
        }

#else
        char *tmp;

        tmp = alloca (dev_name - full_name + 1);

        if (!tmp)
        {
            DBG (1, "sane_open: not enough free memory\n");
            return SANE_STATUS_NO_MEM;
        }

        memcpy (tmp, full_name, dev_name - full_name);
        tmp[dev_name - full_name] = '\0';

        nd_name = tmp;
#endif
        ++dev_name; /* skip colon */
    }
    else
    {
        /* if no colon interpret full_name as the host name; an empty
           device name will cause us to open the first device of that
           host.  */

        nd_name = (char*) full_name;

        dev_name = "";
    }

    DBG (2, "sane_open: host = %s, device = %s\n", nd_name, dev_name);

    if (!nd_name[0])
    {
        /* Unlike other backends, we never allow an empty backend-name.
           Otherwise, it's possible that sane_open("") will result in
           endless looping (consider the case where NET is the first
           backend...) */

        DBG (1, "sane_open: empty backend name is not allowed\n");
        return SANE_STATUS_INVAL;
    }
    else
        for (dev = first_device; dev; dev = dev->next)
            if (strcmp (dev->path, nd_name) == 0)
            {
                break;
            }

    if (!dev)
    {
        DBG (1,
             "sane_open: device %s not found, trying to register it anyway\n",
             nd_name);
        status = add_device (nd_name, &dev);

        if (status != SANE_STATUS_GOOD)
        {
            DBG (1, "sane_open: could not open device\n");
            return status;
        }
    }
    else
    {
        DBG (2, "sane_open: device found in list\n");
    }

    if (dev->ctl < 0)
    {
        DBG (2, "sane_open: device not connected yet...\n");
        status = connect_dev (dev);

        if (status != SANE_STATUS_GOOD)
        {
            DBG (1, "sane_open: could not connect to device\n");
            return status;
        }
    }

    DBG (3, "sane_open: unix_open\n");
    sanei_w_call (&dev->wire, SANE_NET_OPEN,
                  (WireCodecFunc) sanei_w_string, &dev_name,
                  (WireCodecFunc) sanei_w_open_reply, &reply);

    do
    {
        if (dev->wire.status != 0)
        {
            DBG (1, "sane_open: open rpc call failed (%s)\n",
                 strerror (dev->wire.status));
            return SANE_STATUS_IO_ERROR;
        }

        status = reply.status;
        handle = reply.handle;
        need_auth = (reply.resource_to_authorize != 0);

        if (need_auth)
        {
            DBG (3, "sane_open: authorization required\n");
            do_authorization (dev, reply.resource_to_authorize);

            sanei_w_free (&dev->wire, (WireCodecFunc) sanei_w_open_reply,
                          &reply);

            if (dev->wire.direction != WIRE_DECODE)
            {
                sanei_w_set_dir (&dev->wire, WIRE_DECODE);
            }

            sanei_w_open_reply (&dev->wire, &reply);

            continue;
        }
        else
        {
            sanei_w_free (&dev->wire, (WireCodecFunc) sanei_w_open_reply, &reply);
        }

        if (need_auth && !dev->auth_active)
        {
            DBG (2, "sane_open: open cancelled\n");
            return SANE_STATUS_CANCELLED;
        }

        if (status != SANE_STATUS_GOOD)
        {
            DBG (1, "sane_open: remote open failed\n");
            return reply.status;
        }
    }
    while (need_auth);

    s = malloc (sizeof (*s));

    if (!s)
    {
        DBG (1, "sane_open: not enough free memory\n");
        return SANE_STATUS_NO_MEM;
    }

    memset (s, 0, sizeof (*s));
    s->hw = dev;
    s->handle = handle;
    s->data = -1;
    s->next = first_handle;
    s->local_opt.desc = 0;
    s->local_opt.num_options = 0;

    DBG (3, "sane_open: getting option descriptors\n");
    status = fetch_options (s);

    if (status != SANE_STATUS_GOOD)
    {
        DBG (1, "sane_open: fetch_options failed (%s), closing device again\n",
             sane_strstatus (status));

        sanei_w_call (&s->hw->wire, SANE_NET_CLOSE,
                      (WireCodecFunc) sanei_w_word, &s->handle,
                      (WireCodecFunc) sanei_w_word, &ack);

        free (s);

        return status;
    }

    first_handle = s;
    *meta_handle = s;

    DBG (3, "sane_open: success\n");
    return SANE_STATUS_GOOD;
}

void
sane_close (SANE_Handle handle)
{
    Unix_Scanner *prev, *s;
    SANE_Word ack;
    int option_number;

    DBG (3, "sane_close: handle %p\n", handle);

    prev = 0;

    for (s = first_handle; s; s = s->next)
    {
        if (s == handle)
        {
            break;
        }

        prev = s;
    }

    if (!s)
    {
        DBG (1, "sane_close: invalid handle %p\n", handle);
        return; /* oops, not a handle we know about */
    }

    if (prev)
    {
        prev->next = s->next;
    }
    else
    {
        first_handle = s->next;
    }

    if (s->opt.num_options)
    {
        DBG (2, "sane_close: removing cached option descriptors\n");
        sanei_w_set_dir (&s->hw->wire, WIRE_FREE);
        s->hw->wire.status = 0;
        sanei_w_option_descriptor_array (&s->hw->wire, &s->opt);

        if (s->hw->wire.status)
            DBG (1, "sane_close: couldn't free sanei_w_option_descriptor_array "
                 "(%s)\n", sane_strstatus (s->hw->wire.status));
    }

    DBG (2, "sane_close: removing local option descriptors\n");

    for (option_number = 0; option_number < s->local_opt.num_options;
            option_number++)
    {
        free (s->local_opt.desc[option_number]);
    }

    if (s->local_opt.desc)
    {
        free (s->local_opt.desc);
    }

    DBG (2, "sane_close: unix_close\n");
    sanei_w_call (&s->hw->wire, SANE_NET_CLOSE,
                  (WireCodecFunc) sanei_w_word, &s->handle,
                  (WireCodecFunc) sanei_w_word, &ack);

    if (s->data >= 0)
    {
        DBG (2, "sane_close: closing data pipe\n");
        close (s->data);
    }

    free (s);
    DBG (2, "sane_close: done\n");
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
    Unix_Scanner *s = handle;
    SANE_Status status;

    DBG (3, "sane_get_option_descriptor: option %d\n", option);

    if (!s->options_valid)
    {
        DBG (3, "sane_get_option_descriptor: getting option descriptors\n");
        status = fetch_options (s);

        if (status != SANE_STATUS_GOOD)
        {
            DBG (1, "sane_get_option_descriptor: fetch_options failed (%s)\n",
                 sane_strstatus (status));
            return 0;
        }
    }

    if (((SANE_Word) option >= s->opt.num_options) || (option < 0))
    {
        DBG (2, "sane_get_option_descriptor: invalid option number\n");
        return 0;
    }

    return s->local_opt.desc[option];
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option,
                     SANE_Action action, void* value, SANE_Word* info)
{
    Unix_Scanner *s = handle;
    SANE_Control_Option_Req req;
    SANE_Control_Option_Reply reply;
    SANE_Status status;
    size_t value_size;
    int need_auth;
    SANE_Word local_info;

    DBG (3, "sane_control_option: option %d, action %d\n", option, action);

    if (!s->options_valid)
    {
        DBG (1, "sane_control_option: FRONTEND BUG: option descriptors reload needed\n");
        return SANE_STATUS_INVAL;
    }

    if (((SANE_Word) option >= s->opt.num_options) || (option < 0))
    {
        DBG (1, "sane_control_option: invalid option number\n");
        return SANE_STATUS_INVAL;
    }

    switch (s->opt.desc[option]->type)
    {
        case SANE_TYPE_BUTTON:
        case SANE_TYPE_GROUP: /* shouldn't happen... */
            /* the SANE standard defines that the option size of a BUTTON or
               GROUP is IGNORED.  */
            value_size = 0;
            break;

        case SANE_TYPE_STRING: /* strings can be smaller than size */
            value_size = s->opt.desc[option]->size;

            if ((action == SANE_ACTION_SET_VALUE)
                    && (((SANE_Int) strlen ((SANE_String) value) + 1)
                        < s->opt.desc[option]->size))
            {
                value_size = strlen ((SANE_String) value) + 1;
            }

            break;

        default:
            value_size = s->opt.desc[option]->size;
            break;
    }

    /* Avoid leaking memory bits */
    if (value && (action != SANE_ACTION_SET_VALUE))
    {
        memset (value, 0, value_size);
    }

    /* for SET_AUTO the parameter ``value'' is ignored */
    if (action == SANE_ACTION_SET_AUTO)
    {
        value_size = 0;
    }

    req.handle = s->handle;
    req.option = option;
    req.action = action;
    req.value_type = s->opt.desc[option]->type;
    req.value_size = value_size;
    req.value = value;

    local_info = 0;

    DBG (3, "sane_control_option: remote control option\n");
    sanei_w_call (&s->hw->wire, SANE_NET_CONTROL_OPTION,
                  (WireCodecFunc) sanei_w_control_option_req, &req,
                  (WireCodecFunc) sanei_w_control_option_reply, &reply);

    do
    {
        status = reply.status;
        need_auth = (reply.resource_to_authorize != 0);

        if (need_auth)
        {
            DBG (3, "sane_control_option: auth required\n");
            do_authorization (s->hw, reply.resource_to_authorize);
            sanei_w_free (&s->hw->wire,
                          (WireCodecFunc) sanei_w_control_option_reply, &reply);

            sanei_w_set_dir (&s->hw->wire, WIRE_DECODE);

            sanei_w_control_option_reply (&s->hw->wire, &reply);
            continue;

        }
        else if (status == SANE_STATUS_GOOD)
        {
            local_info = reply.info;

            if (info)
            {
                *info = reply.info;
            }

            if (value_size > 0)
            {
                if ((SANE_Word) value_size == reply.value_size)
                {
                    memcpy (value, reply.value, reply.value_size);
                }
                else
                    DBG (1, "sane_control_option: size changed from %d to %d\n",
                         s->opt.desc[option]->size, reply.value_size);
            }

            if (reply.info & SANE_INFO_RELOAD_OPTIONS)
            {
                s->options_valid = 0;
            }
        }

        sanei_w_free (&s->hw->wire,
                      (WireCodecFunc) sanei_w_control_option_reply, &reply);

        if (need_auth && !s->hw->auth_active)
        {
            return SANE_STATUS_CANCELLED;
        }
    }
    while (need_auth);

    DBG (2, "sane_control_option: remote done (%s, info %x)\n", sane_strstatus (status), local_info);

    if ((status == SANE_STATUS_GOOD) && (info == NULL) && (local_info & SANE_INFO_RELOAD_OPTIONS))
    {
        DBG (2, "sane_control_option: reloading options as frontend does not care\n");

        status = fetch_options (s);

        DBG (2, "sane_control_option: reload done (%s)\n", sane_strstatus (status));
    }

    DBG (2, "sane_control_option: done (%s, info %x)\n", sane_strstatus (status), local_info);

    return status;
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters* params)
{
    Unix_Scanner *s = handle;
    SANE_Get_Parameters_Reply reply;
    SANE_Status status;

    DBG (3, "sane_get_parameters\n");

    if (!params)
    {
        DBG (1, "sane_get_parameters: parameter params not supplied\n");
        return SANE_STATUS_INVAL;
    }

    DBG (3, "sane_get_parameters: remote get parameters\n");
    sanei_w_call (&s->hw->wire, SANE_NET_GET_PARAMETERS,
                  (WireCodecFunc) sanei_w_word, &s->handle,
                  (WireCodecFunc) sanei_w_get_parameters_reply, &reply);

    status = reply.status;
    *params = reply.params;
    depth = reply.params.depth;
    sanei_w_free (&s->hw->wire,
                  (WireCodecFunc) sanei_w_get_parameters_reply, &reply);

    DBG (3, "sane_get_parameters: returned status %s\n",
         sane_strstatus (status));
    return status;
}

SANE_Status
sane_start (SANE_Handle handle)
{
    Unix_Scanner *s = handle;
    SANE_Start_Reply reply;
    struct sockaddr_un sun;
    SANE_Status status;
    int fd, need_auth;
    socklen_t len;

    DBG (3, "sane_start\n");

    hang_over = -1;
    left_over = -1;

    if (s->data >= 0)
    {
        DBG (2, "sane_start: data pipe already exists\n");
        return SANE_STATUS_INVAL;
    }

    /* Do this ahead of time so in case anything fails, we can
       recover gracefully (without hanging our server).  */
    len = sizeof (sun);

    fd = socket (s->hw->addr.sun_family, SOCK_STREAM, 0);

    if (fd < 0)
    {
        DBG (1, "sane_start: socket() failed (%s)\n", strerror (errno));
        return SANE_STATUS_IO_ERROR;
    }

    DBG (3, "sane_start: remote start\n");
    sanei_w_call (&s->hw->wire, SANE_NET_START,
                  (WireCodecFunc) sanei_w_word, &s->handle,
                  (WireCodecFunc) sanei_w_start_reply, &reply);

    do
    {

        status = reply.status;

        if (reply.byte_order == 0x1234)
        {
            server_big_endian = 0;
            DBG (1, "sane_start: server has little endian byte order\n");
        }
        else
        {
            server_big_endian = 1;
            DBG (1, "sane_start: server has big endian byte order\n");
        }

        need_auth = (reply.resource_to_authorize != 0);

        if (need_auth)
        {
            DBG (3, "sane_start: auth required\n");
            do_authorization (s->hw, reply.resource_to_authorize);

            sanei_w_free (&s->hw->wire,
                          (WireCodecFunc) sanei_w_start_reply, &reply);

            sanei_w_set_dir (&s->hw->wire, WIRE_DECODE);

            sanei_w_start_reply (&s->hw->wire, &reply);

            continue;
        }

        sanei_w_free (&s->hw->wire, (WireCodecFunc) sanei_w_start_reply,
                      &reply);

        if (need_auth && !s->hw->auth_active)
        {
            return SANE_STATUS_CANCELLED;
        }

        if (status != SANE_STATUS_GOOD)
        {
            DBG (1, "sane_start: remote start failed (%s)\n",
                 sane_strstatus (status));
            close (fd);
            return status;
        }
    }
    while (need_auth);

    if (connect (fd, (struct sockaddr*) &sun, len) < 0)
    {
        DBG (1, "sane_start: connect() failed (%s)\n", strerror (errno));
        close (fd);
        return SANE_STATUS_IO_ERROR;
    }

    shutdown (fd, 1);
    s->data = fd;
    s->reclen_buf_offset = 0;
    s->bytes_remaining = 0;
    DBG (3, "sane_start: done (%s)\n", sane_strstatus (status));
    return status;
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte* data, SANE_Int max_length,
           SANE_Int* length)
{
    Unix_Scanner *s = handle;
    ssize_t nread;
    SANE_Int cnt;
    SANE_Int start_cnt;
    SANE_Int end_cnt;
    SANE_Byte swap_buf;
    SANE_Byte temp_hang_over;
    int is_even;

    DBG (3, "sane_read: handle=%p, data=%p, max_length=%d, length=%p\n",
         handle, data, max_length, (void*) length);

    if (!length)
    {
        DBG (1, "sane_read: length == NULL\n");
        return SANE_STATUS_INVAL;
    }

    is_even = 1;
    *length = 0;

    /* If there's a left over, i.e. a byte already in the correct byte order,
       return it immediately; otherwise read may fail with a SANE_STATUS_EOF and
       the caller never can read the last byte */
    if ((depth == 16) && (server_big_endian != client_big_endian))
    {
        if (left_over > -1)
        {
            DBG (3, "sane_read: left_over from previous call, return "
                 "immediately\n");
            /* return the byte, we've currently scanned; hang_over becomes
               left_over */
            *data = (SANE_Byte) left_over;
            left_over = -1;
            *length = 1;
            return SANE_STATUS_GOOD;
        }
    }

    if (s->data < 0)
    {
        DBG (1, "sane_read: data pipe doesn't exist, scan cancelled?\n");
        return SANE_STATUS_CANCELLED;
    }

    if (s->bytes_remaining == 0)
    {
        /* boy, is this painful or what? */

        DBG (4, "sane_read: reading packet length\n");
        nread = read (s->data, s->reclen_buf + s->reclen_buf_offset,
                      4 - s->reclen_buf_offset);

        if (nread < 0)
        {
            DBG (3, "sane_read: read failed (%s)\n", strerror (errno));

            if (errno == EAGAIN)
            {
                DBG (3, "sane_read: try again later\n");
                return SANE_STATUS_GOOD;
            }
            else
            {
                DBG (1, "sane_read: cancelling read\n");
                do_cancel (s);
                return SANE_STATUS_IO_ERROR;
            }
        }

        DBG (4, "sane_read: read %lu bytes, %d from 4 total\n", (u_long) nread,
             s->reclen_buf_offset);
        s->reclen_buf_offset += nread;

        if (s->reclen_buf_offset < 4)
        {
            DBG (4, "sane_read: enough for now\n");
            return SANE_STATUS_GOOD;
        }

        s->reclen_buf_offset = 0;
        s->bytes_remaining = (((u_long) s->reclen_buf[0] << 24)
                              | ((u_long) s->reclen_buf[1] << 16)
                              | ((u_long) s->reclen_buf[2] << 8)
                              | ((u_long) s->reclen_buf[3] << 0));
        DBG (3, "sane_read: next record length=%ld bytes\n",
             (long) s->bytes_remaining);

        if (s->bytes_remaining == 0xffffffff)
        {
            char ch;

            DBG (2, "sane_read: received error signal\n");

            /* turn off non-blocking I/O (s->data will be closed anyhow): */
            fcntl (s->data, F_SETFL, 0);

            /* read the status byte: */
            if (read (s->data, &ch, sizeof (ch)) != 1)
            {
                DBG (1, "sane_read: failed to read error code\n");
                ch = SANE_STATUS_IO_ERROR;
            }

            DBG (1, "sane_read: error code %s\n",
                 sane_strstatus ((SANE_Status) ch));
            do_cancel (s);
            return (SANE_Status) ch;
        }
    }

    if (max_length > (SANE_Int) s->bytes_remaining)
    {
        max_length = s->bytes_remaining;
    }

    nread = read (s->data, data, max_length);

    if (nread < 0)
    {
        DBG (2, "sane_read: error code %s\n", strerror (errno));

        if (errno == EAGAIN)
        {
            return SANE_STATUS_GOOD;
        }
        else
        {
            DBG (1, "sane_read: cancelling scan\n");
            do_cancel (s);
            return SANE_STATUS_IO_ERROR;
        }
    }

    s->bytes_remaining -= nread;

    *length = nread;

    /* Check whether we are scanning with a depth of 16 bits/pixel and whether
       server and client have different byte order. If this is true, then it's
       necessary to check whether read returned an odd number. If an odd number
       has been returned, we must save the last byte.
    */
    if ((depth == 16) && (server_big_endian != client_big_endian))
    {
        DBG (1,"sane_read: client/server have different byte order; "
             "must swap\n");

        /* special case: 1 byte scanned and hang_over */
        if ((nread == 1) && (hang_over > -1))
        {
            /* return the byte, we've currently scanned; hang_over becomes
               left_over */
            left_over = hang_over;
            hang_over = -1;
            return SANE_STATUS_GOOD;
        }

        /* check whether an even or an odd number of bytes has been scanned */
        if ((nread % 2) == 0)
        {
            is_even = 1;
        }
        else
        {
            is_even = 0;
        }

        /* check, whether there's a hang over from a previous call;
        in this case we memcopy the data up one byte */
        if ((nread > 1) && (hang_over > -1))
        {
            /* store last byte */
            temp_hang_over = *(data + nread - 1);
            memmove (data + 1, data, nread - 1);
            *data = (SANE_Byte) hang_over;

            /* what happens with the last byte depends on whether the number
               of bytes is even or odd */
            if (is_even == 1)
            {
                /* number of bytes is even; no new hang_over, exchange last
                byte with hang over; last byte becomes left_over */
                left_over = *(data + nread - 1);
                *(data + nread - 1) = temp_hang_over;
                hang_over = -1;
                start_cnt = 0;
                /* last byte already swapped */
                end_cnt = nread - 2;
            }
            else
            {
                /* number of bytes is odd; last byte becomes new hang_over */
                hang_over = temp_hang_over;
                left_over = -1;
                start_cnt = 0;
                end_cnt = nread - 1;
            }
        }
        else if (nread == 1)
        {
            /* if only one byte has been read, save it as hang_over and return
               length=0 */
            hang_over = (int) *data;
            *length = 0;
            return SANE_STATUS_GOOD;
        }
        else
        {
            /* no hang_over; test for even or odd byte number */
            if(is_even == 1)
            {
                start_cnt = 0;
                end_cnt = *length;
            }
            else
            {
                start_cnt = 0;
                hang_over = *(data + *length - 1);
                *length -= 1;
                end_cnt = *length;
            }
        }

        /* swap the bytes */
        for (cnt = start_cnt; cnt < end_cnt - 1; cnt += 2)
        {
            swap_buf = *(data + cnt);
            *(data + cnt) = *(data + cnt + 1);
            *(data + cnt + 1) = swap_buf;
        }
    }

    DBG (3, "sane_read: %lu bytes read, %lu remaining\n", (u_long) nread,
         (u_long) s->bytes_remaining);

    return SANE_STATUS_GOOD;
}

void
sane_cancel (SANE_Handle handle)
{
    Unix_Scanner *s = handle;
    SANE_Word ack;

    DBG (3, "sane_cancel: sending unix_cancel\n");

    sanei_w_call (&s->hw->wire, SANE_NET_CANCEL,
                  (WireCodecFunc) sanei_w_word, &s->handle,
                  (WireCodecFunc) sanei_w_word, &ack);
    do_cancel (s);
    DBG (4, "sane_cancel: done\n");
}

SANE_Status
sane_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking)
{
    Unix_Scanner *s = handle;

    DBG (3, "sane_set_io_mode: non_blocking = %d\n", non_blocking);

    if (s->data < 0)
    {
        DBG (1, "sane_set_io_mode: pipe doesn't exist\n");
        return SANE_STATUS_INVAL;
    }

    if (fcntl (s->data, F_SETFL, non_blocking ? O_NONBLOCK : 0) < 0)
    {
        DBG (1, "sane_set_io_mode: fcntl failed (%s)\n", strerror (errno));
        return SANE_STATUS_IO_ERROR;
    }

    return SANE_STATUS_GOOD;
}

SANE_Status
sane_get_select_fd (SANE_Handle handle, SANE_Int* fd)
{
    Unix_Scanner *s = handle;

    DBG (3, "sane_get_select_fd\n");

    if (s->data < 0)
    {
        DBG (1, "sane_get_select_fd: pipe doesn't exist\n");
        return SANE_STATUS_INVAL;
    }

    *fd = s->data;
    DBG (3, "sane_get_select_fd: done; *fd = %d\n", *fd);
    return SANE_STATUS_GOOD;
}
