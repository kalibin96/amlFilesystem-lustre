/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_SEC
#ifdef __KERNEL__
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#else
#include <liblustre.h>
#endif

#include <libcfs/kp30.h>
#include <linux/obd.h>
#include <linux/obd_class.h>
#include <linux/obd_support.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_sec.h>

#include "gss_internal.h"

int rawobj_alloc(rawobj_t *obj, char *buf, int len)
{
        LASSERT(obj);
        LASSERT(len >= 0);

        obj->len = len;
        if (len) {
                OBD_ALLOC(obj->data, len);
                if (!obj->data)
                        RETURN(-ENOMEM);
                memcpy(obj->data, buf, len);
        } else
                obj->data = NULL;
        return 0;
}

void rawobj_free(rawobj_t *obj)
{
        LASSERT(obj);

        if (obj->len) {
                LASSERT(obj->data);
                OBD_FREE(obj->data, obj->len);
                obj->len = 0;
                obj->data = NULL;
        } else
                LASSERT(!obj->data);
}

int rawobj_equal(rawobj_t *a, rawobj_t *b)
{
        LASSERT(a && b);

        return (a->len == b->len &&
                !memcmp(a->data, b->data, a->len));
}

int rawobj_dup(rawobj_t *dest, rawobj_t *src)
{
        LASSERT(src && dest);

        dest->len = src->len;
        if (dest->len) {
                OBD_ALLOC(dest->data, dest->len);
                if (!dest->data)
                        return -ENOMEM;
                memcpy(dest->data, src->data, dest->len);
        } else
                dest->data = NULL;
        return 0;
}

int rawobj_serialize(rawobj_t *obj, __u32 **buf, __u32 *buflen)
{
        __u32 len;

        LASSERT(obj);
        LASSERT(buf);
        LASSERT(buflen);

        len = size_round4(obj->len);

        if (*buflen < 4 + len) {
                CERROR("buflen %u <  %u\n", *buflen, 4 + len);
                return -EINVAL;
        }

        *(*buf)++ = cpu_to_le32(obj->len);
        memcpy(*buf, obj->data, obj->len);
        *buf += (len >> 2);
        *buflen -= (4 + len);

        return 0;
}

static int __rawobj_extract(rawobj_t *obj, __u32 **buf, __u32 *buflen,
                     	int alloc, int local)
{
        __u32 len;

        if (*buflen < sizeof(__u32)) {
        	CERROR("buflen %u\n", *buflen);
                return -EINVAL;
        }

        obj->len = *(*buf)++;
        if (!local)
        	obj->len = le32_to_cpu(obj->len);
        *buflen -= sizeof(__u32);

        if (!obj->len) {
                obj->data = NULL;
                return 0;
        }

        len = local ? obj->len : size_round4(obj->len);
        if (*buflen < len) {
        	CERROR("buflen %u < %u\n", *buflen, len);
                return -EINVAL;
        }

        if (!alloc)
        	obj->data = (__u8 *) *buf;
        else {
        	OBD_ALLOC(obj->data, obj->len);
        	if (!obj->data) {
        		CERROR("fail to alloc %u bytes\n", obj->len);
        		return -ENOMEM;
        	}
        	memcpy(obj->data, *buf, obj->len);
        }

        *((char **)buf) += len;
        *buflen -= len;

        return 0;
}

int rawobj_extract(rawobj_t *obj, __u32 **buf, __u32 *buflen)
{
        return __rawobj_extract(obj, buf, buflen, 0, 0);
}

int rawobj_extract_local(rawobj_t *obj, __u32 **buf, __u32 *buflen)
{
        return __rawobj_extract(obj, buf, buflen, 0, 1);
}
