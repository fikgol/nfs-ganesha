/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * -------------
 */

/* main.c
 * Module core functions
 */

#include "config.h"

#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include "fsal.h"
#include "FSAL/fsal_init.h"
#include "fsal_handle_syscalls.h"

/* VFS FSAL module private storage
 */

/* defined the set of attributes supported with POSIX */
#ifndef ENABLE_VFS_DEBUG_ACL
#define XFS_SUPPORTED_ATTRIBUTES ((const attrmask_t) (ATTRS_POSIX))
#else
#define XFS_SUPPORTED_ATTRIBUTES ((const attrmask_t) (ATTRS_POSIX | ATTR_ACL))
#endif

struct xfs_fsal_module {
	struct fsal_module fsal;
	struct fsal_staticfsinfo_t fs_info;
	/* xfsfs_specific_initinfo_t specific_info;  placeholder */
};

const char myname[] = "XFS";

/* filesystem info for XFS */
static struct fsal_staticfsinfo_t default_posix_info = {
	.maxfilesize = UINT64_MAX,
	.maxlink = _POSIX_LINK_MAX,
	.maxnamelen = 1024,
	.maxpathlen = 1024,
	.no_trunc = true,
	.chown_restricted = true,
	.case_insensitive = false,
	.case_preserving = true,
	.lock_support = false,
	.lock_support_owner = true,
	.lock_support_async_block = false,
	.named_attr = true,
	.unique_handles = true,
	.lease_time = {10, 0},
	.acl_support = FSAL_ACLSUPPORT_ALLOW,
	.homogenous = true,
	.supported_attrs = XFS_SUPPORTED_ATTRIBUTES,
	.maxread = FSAL_MAXIOSIZE,
	.maxwrite = FSAL_MAXIOSIZE,
	.link_supports_permission_checks = false,
};

static struct config_item xfs_params[] = {
	CONF_ITEM_BOOL("link_support", true,
		       fsal_staticfsinfo_t, link_support),
	CONF_ITEM_BOOL("symlink_support", true,
		       fsal_staticfsinfo_t, symlink_support),
	CONF_ITEM_BOOL("cansettime", true,
		       fsal_staticfsinfo_t, cansettime),
	CONF_ITEM_UI64("maxread", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
		       fsal_staticfsinfo_t, maxread),
	CONF_ITEM_UI64("maxwrite", 512, FSAL_MAXIOSIZE, FSAL_MAXIOSIZE,
		       fsal_staticfsinfo_t, maxwrite),
	CONF_ITEM_MODE("umask", 0,
		       fsal_staticfsinfo_t, umask),
	CONF_ITEM_BOOL("auth_xdev_export", false,
		       fsal_staticfsinfo_t, auth_exportpath_xdev),
	CONF_ITEM_MODE("xattr_access_rights", 0400,
		       fsal_staticfsinfo_t, xattr_access_rights),
	CONFIG_EOL
};

struct config_block xfs_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.xfs",
	.blk_desc.name = "XFS",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = xfs_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/* private helper for export object
 */

struct fsal_staticfsinfo_t *vfs_staticinfo(struct fsal_module *hdl)
{
	struct xfs_fsal_module *myself;

	myself = container_of(hdl, struct xfs_fsal_module, fsal);
	return &myself->fs_info;
}

/* Module methods
 */

/* init_config
 * must be called with a reference taken (via lookup_fsal)
 */

static fsal_status_t init_config(struct fsal_module *fsal_hdl,
				 config_file_t config_struct,
				 struct config_error_type *err_type)
{
	struct xfs_fsal_module *xfs_me =
	    container_of(fsal_hdl, struct xfs_fsal_module, fsal);
#ifdef F_OFD_GETLK
	int fd, rc;
	struct flock lock;
	char *temp_name;
#endif

	xfs_me->fs_info = default_posix_info;	/* copy the consts */

#ifdef F_OFD_GETLK
	/* If on a system that might support OFD locks, verify them.
	 * Only if they exist will we declare lock support.
	 */
	LogInfo(COMPONENT_FSAL, "FSAL_XFS testing OFD Locks");
	temp_name = gsh_strdup("/tmp/ganesha.nfsd.locktestXXXXXX");
	fd = mkstemp(temp_name);
	if (fd >= 0) {
		lock.l_whence = SEEK_SET;
		lock.l_type = F_RDLCK;
		lock.l_start = 0;
		lock.l_len = 0;
		lock.l_pid = 0;

		rc = fcntl(fd, F_OFD_GETLK, &lock);

		if (rc == 0)
			xfs_me->fs_info.lock_support = true;
		else
			LogInfo(COMPONENT_FSAL, "Could not use OFD locks");

		close(fd);
		unlink(temp_name);
	} else {
		LogCrit(COMPONENT_FSAL,
			"Could not create file %s to test OFD locks",
			temp_name);
	}
	gsh_free(temp_name);
#endif

	if (xfs_me->fs_info.lock_support)
		LogInfo(COMPONENT_FSAL, "FSAL_XFS enabling OFD Locks");
	else
		LogInfo(COMPONENT_FSAL, "FSAL_XFS disabling lock support");

	(void) load_config_from_parse(config_struct,
				      &xfs_param,
				      &xfs_me->fs_info,
				      true,
				      err_type);
	if (!config_error_is_harmless(err_type))
		return fsalstat(ERR_FSAL_INVAL, 0);
	display_fsinfo(&xfs_me->fs_info);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes constant = 0x%" PRIx64,
		     XFS_SUPPORTED_ATTRIBUTES);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes default = 0x%" PRIx64,
		     default_posix_info.supported_attrs);
	LogDebug(COMPONENT_FSAL,
		 "FSAL INIT: Supported attributes mask = 0x%" PRIx64,
		 xfs_me->fs_info.supported_attrs);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Internal XFS method linkage to export object
 */

fsal_status_t vfs_create_export(struct fsal_module *fsal_hdl,
				void *parse_node,
				struct config_error_type *err_type,
				const struct fsal_up_vector *up_ops);

/**
 * @brief Indicate support for extended operations.
 *
 * @retval true if extended operations are supported.
 */

bool vfs_support_ex(struct fsal_obj_handle *obj)
{
	return true;
}

/* Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

/* my module private storage
 */

static struct xfs_fsal_module XFS;

/* linkage to the exports and handle ops initializers
 */

MODULE_INIT void xfs_init(void)
{
	int retval;
	struct fsal_module *myself = &XFS.fsal;

	retval = register_fsal(myself, myname, FSAL_MAJOR_VERSION,
			       FSAL_MINOR_VERSION, FSAL_ID_NO_PNFS);
	if (retval != 0) {
		fprintf(stderr, "XFS module failed to register");
		return;
	}
	myself->m_ops.create_export = vfs_create_export;
	myself->m_ops.init_config = init_config;
	myself->m_ops.support_ex = vfs_support_ex;
}

MODULE_FINI void xfs_unload(void)
{
	int retval;

	retval = unregister_fsal(&XFS.fsal);
	if (retval != 0) {
		fprintf(stderr, "XFS module failed to unregister");
		return;
	}
}
