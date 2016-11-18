/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * lustre/utils/lustre_cfg.c
 *
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Robert Read <rread@clusterfs.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#include <libcfs/util/ioctl.h>
#include <libcfs/util/string.h>
#include <libcfs/util/param.h>
#include <libcfs/util/parser.h>
#include <lustre/lustreapi.h>
#include <linux/lnet/nidstr.h>
#include <linux/lnet/lnetctl.h>
#include <linux/lustre/lustre_cfg.h>
#include <linux/lustre/lustre_ioctl.h>
#include <linux/lustre/lustre_ver.h>

#include <linux/lustre/lustre_kernelcomm.h>
#include <lnetconfig/liblnetconfig.h>
#include "lustreapi_internal.h"

#include <sys/un.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>

#include "obdctl.h"
#include <stdio.h>
#include <yaml.h>

static char *lcfg_devname;

int lcfg_set_devname(char *name)
{
	char *ptr;
	int digit = 1;

	if (name) {
		if (lcfg_devname)
			free(lcfg_devname);
		/* quietly strip the unnecessary '$' */
		if (*name == '$' || *name == '%')
			name++;

		ptr = name;
		while (*ptr != '\0') {
			if (!isdigit(*ptr)) {
				digit = 0;
				break;
			}
			ptr++;
		}

		if (digit) {
			/* We can't translate from dev # to name */
			lcfg_devname = NULL;
		} else {
			lcfg_devname = strdup(name);
		}
	} else {
		lcfg_devname = NULL;
	}
	return 0;
}

char *lcfg_get_devname(void)
{
	return lcfg_devname;
}

int jt_lcfg_device(int argc, char **argv)
{
	return jt_obd_device(argc, argv);
}

static int jt_lcfg_ioctl(struct lustre_cfg_bufs *bufs, char *arg, int cmd)
{
	struct lustre_cfg *lcfg;
	int rc;

	lcfg = malloc(lustre_cfg_len(bufs->lcfg_bufcount, bufs->lcfg_buflen));
	if (!lcfg) {
		rc = -ENOMEM;
	} else {
		lustre_cfg_init(lcfg, cmd, bufs);
		rc = lcfg_ioctl(arg, OBD_DEV_ID, lcfg);
		free(lcfg);
	}
	if (rc < 0)
		fprintf(stderr, "error: %s: %s\n", jt_cmdname(arg),
			strerror(rc = errno));
	return rc;
}

int jt_lcfg_attach(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;
	int rc;

	if (argc != 4)
		return CMD_HELP;

	lustre_cfg_bufs_reset(&bufs, NULL);

	lustre_cfg_bufs_set_string(&bufs, 1, argv[1]);
	lustre_cfg_bufs_set_string(&bufs, 0, argv[2]);
	lustre_cfg_bufs_set_string(&bufs, 2, argv[3]);

	rc = jt_lcfg_ioctl(&bufs, argv[0], LCFG_ATTACH);
	if (rc == 0)
		lcfg_set_devname(argv[2]);

	return rc;
}

int jt_lcfg_setup(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;
	int i;

	if (!lcfg_devname) {
		fprintf(stderr,
			"%s: please use 'device name' to set the device name for config commands.\n",
			jt_cmdname(argv[0]));
		return -EINVAL;
	}

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	if (argc > 6)
		return CMD_HELP;

	for (i = 1; i < argc; i++)
		lustre_cfg_bufs_set_string(&bufs, i, argv[i]);

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_SETUP);
}

int jt_obd_detach(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;

	if (!lcfg_devname) {
		fprintf(stderr,
			"%s: please use 'device name' to set the device name for config commands.\n",
			jt_cmdname(argv[0]));
		return -EINVAL;
	}

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	if (argc != 1)
		return CMD_HELP;

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_DETACH);
}

int jt_obd_cleanup(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;
	char force = 'F';
	char failover = 'A';
	char flags[3] = { 0 };
	int flag_cnt = 0, n;

	if (!lcfg_devname) {
		fprintf(stderr,
			"%s: please use 'device name' to set the device name for config commands.\n",
			jt_cmdname(argv[0]));
		return -EINVAL;
	}

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	if (argc < 1 || argc > 3)
		return CMD_HELP;

	/*
	 * we are protected from overflowing our buffer by the argc
	 * check above
	 */
	for (n = 1; n < argc; n++) {
		if (strcmp(argv[n], "force") == 0) {
			flags[flag_cnt++] = force;
		} else if (strcmp(argv[n], "failover") == 0) {
			flags[flag_cnt++] = failover;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[n]);
			return CMD_HELP;
		}
	}

	if (flag_cnt)
		lustre_cfg_bufs_set_string(&bufs, 1, flags);

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_CLEANUP);
}

static
int do_add_uuid(char *func, char *uuid, struct lnet_nid *nid)
{
	int rc;
	char nidstr[LNET_NIDSTR_SIZE];
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *lcfg;

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);
	if (uuid)
		lustre_cfg_bufs_set_string(&bufs, 1, uuid);
	if (!nid_is_nid4(nid)) {
		libcfs_nidstr_r(nid, nidstr, sizeof(nidstr));
		lustre_cfg_bufs_set_string(&bufs, 2, nidstr);
	}

	lcfg = malloc(lustre_cfg_len(bufs.lcfg_bufcount, bufs.lcfg_buflen));
	if (!lcfg) {
		rc = -ENOMEM;
	} else {
		lustre_cfg_init(lcfg, LCFG_ADD_UUID, &bufs);
		if (nid_is_nid4(nid))
			lcfg->lcfg_nid = lnet_nid_to_nid4(nid);
		else
			lcfg->lcfg_nid = 0;

		rc = lcfg_ioctl(func, OBD_DEV_ID, lcfg);
		free(lcfg);
	}
	if (rc) {
		fprintf(stderr, "IOC_PORTAL_ADD_UUID failed: %s\n",
			strerror(errno));
		return -1;
	}

	if (uuid)
		printf("Added uuid %s: %s\n", uuid, libcfs_nidstr(nid));

	return 0;
}

int jt_lcfg_add_uuid(int argc, char **argv)
{
	struct lnet_nid nid;

	if (argc != 3)
		return CMD_HELP;

	if (libcfs_strnid(&nid, argv[2]) < 0) {
		fprintf(stderr, "Can't parse NID %s\n", argv[2]);
		return (-1);
	}

	return do_add_uuid(argv[0], argv[1], &nid);
}

int jt_lcfg_del_uuid(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <uuid>\n", argv[0]);
		return 0;
	}

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);
	if (strcmp(argv[1], "_all_"))
		lustre_cfg_bufs_set_string(&bufs, 1, argv[1]);

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_DEL_UUID);
}

int jt_lcfg_del_mount_option(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;

	if (argc != 2)
		return CMD_HELP;

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	/* profile name */
	lustre_cfg_bufs_set_string(&bufs, 1, argv[1]);

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_DEL_MOUNTOPT);
}

int jt_lcfg_set_timeout(int argc, char **argv)
{
	int rc;
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *lcfg;

	fprintf(stderr,
		"%s has been deprecated. Use conf_param instead.\ne.g. conf_param lustre-MDT0000 obd_timeout=50\n",
		jt_cmdname(argv[0]));
	return CMD_HELP;

	if (argc != 2)
		return CMD_HELP;

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	lcfg = malloc(lustre_cfg_len(bufs.lcfg_bufcount, bufs.lcfg_buflen));
	if (!lcfg) {
		rc = -ENOMEM;
	} else {
		lustre_cfg_init(lcfg, LCFG_SET_TIMEOUT, &bufs);
		lcfg->lcfg_num = atoi(argv[1]);

		rc = lcfg_ioctl(argv[0], OBD_DEV_ID, lcfg);
		free(lcfg);
	}
	if (rc < 0) {
		fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
			strerror(rc = errno));
	}
	return rc;
}

int jt_lcfg_add_conn(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *lcfg;
	int priority;
	int rc;

	if (argc == 2)
		priority = 0;
	else if (argc == 3)
		priority = 1;
	else
		return CMD_HELP;

	if (!lcfg_devname) {
		fprintf(stderr,
			"%s: please use 'device name' to set the device name for config commands.\n",
			jt_cmdname(argv[0]));
		return -EINVAL;
	}

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	lustre_cfg_bufs_set_string(&bufs, 1, argv[1]);

	lcfg = malloc(lustre_cfg_len(bufs.lcfg_bufcount, bufs.lcfg_buflen));
	if (!lcfg) {
		rc = -ENOMEM;
	} else {
		lustre_cfg_init(lcfg, LCFG_ADD_CONN, &bufs);
		lcfg->lcfg_num = priority;

		rc = lcfg_ioctl(argv[0], OBD_DEV_ID, lcfg);
		free(lcfg);
	}
	if (rc < 0) {
		fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
			strerror(rc = errno));
	}

	return rc;
}

int jt_lcfg_del_conn(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;

	if (argc != 2)
		return CMD_HELP;

	if (!lcfg_devname) {
		fprintf(stderr,
			"%s: please use 'device name' to set the device name for config commands.\n",
			jt_cmdname(argv[0]));
		return -EINVAL;
	}

	lustre_cfg_bufs_reset(&bufs, lcfg_devname);

	/* connection uuid */
	lustre_cfg_bufs_set_string(&bufs, 1, argv[1]);

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_DEL_MOUNTOPT);
}

/* Param set locally, directly on target */
int jt_lcfg_param(int argc, char **argv)
{
	struct lustre_cfg_bufs bufs;
	int i;

	if (argc >= LUSTRE_CFG_MAX_BUFCOUNT)
		return CMD_HELP;

	lustre_cfg_bufs_reset(&bufs, NULL);

	for (i = 1; i < argc; i++)
		lustre_cfg_bufs_set_string(&bufs, i, argv[i]);

	return jt_lcfg_ioctl(&bufs, argv[0], LCFG_PARAM);
}

struct param_opts {
	unsigned int po_only_path:1;
	unsigned int po_show_path:1;
	unsigned int po_show_type:1;
	unsigned int po_recursive:1;
	unsigned int po_perm:1;
	unsigned int po_delete:1;
	unsigned int po_only_dir:1;
	unsigned int po_file:1;
	unsigned int po_yaml:1;
	unsigned int po_detail:1;
};

int lcfg_setparam_perm(char *func, char *buf)
{
	int rc = 0;
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *lcfg;

	lustre_cfg_bufs_reset(&bufs, NULL);
	/*
	 * This same command would be executed on all nodes, many
	 * of which should fail (silently) because they don't have
	 * that proc file existing locally. There would be no
	 * preprocessing on the MGS to try to figure out which
	 * parameter files to add this to, there would be nodes
	 * processing on the cluster nodes to try to figure out
	 * if they are the intended targets. They will blindly
	 * try to set the parameter, and ENOTFOUND means it wasn't
	 * for them.
	 * Target name "general" means call on all targets. It is
	 * left here in case some filtering will be added in
	 * future.
	 */
	lustre_cfg_bufs_set_string(&bufs, 0, "general");

	lustre_cfg_bufs_set_string(&bufs, 1, buf);

	lcfg = malloc(lustre_cfg_len(bufs.lcfg_bufcount,
				     bufs.lcfg_buflen));
	if (!lcfg) {
		rc = -ENOMEM;
		fprintf(stderr, "error: allocating lcfg for %s: %s\n",
			jt_cmdname(func), strerror(rc));

	} else {
		lustre_cfg_init(lcfg, LCFG_SET_PARAM, &bufs);
		rc = lcfg_mgs_ioctl(func, OBD_DEV_ID, lcfg);
		if (rc != 0)
			fprintf(stderr, "error: executing %s: %s\n",
				jt_cmdname(func), strerror(errno));
		free(lcfg);
	}

	return rc;
}

/*
 * Param set to single log file, used by all clients and servers.
 * This should be loaded after the individual config logs.
 * Called from set param with -P option.
 */
static int jt_lcfg_setparam_perm(int argc, char **argv,
				 struct param_opts *popt)
{
	int rc;
	int i;
	int first_param;
	char *buf = NULL;

	first_param = optind;
	if (first_param < 0 || first_param >= argc)
		return CMD_HELP;

	for (i = first_param, rc = 0; i < argc; i++) {
		buf = argv[i];
		if (popt->po_delete) {
			char *end_pos;
			size_t len;

			len = strlen(buf);
			/* Consider param ends at the first '=' in the buffer
			 * and make sure it always ends with '=' as well
			 */
			end_pos = memchr(buf, '=', len - 1);
			if (end_pos) {
				*(++end_pos) = '\0';
			} else if (buf[len - 1] != '=') {
				buf = malloc(len + 2);
				if (buf == NULL)
					return -ENOMEM;
				sprintf(buf, "%s=", argv[i]);
			}
		}

		rc = lcfg_setparam_perm(argv[0], buf);
		if (buf != argv[i])
			free(buf);
	}

	return rc;
}

int lcfg_conf_param(char *func, char *buf)
{
	int rc;
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *lcfg;

	lustre_cfg_bufs_reset(&bufs, NULL);
	lustre_cfg_bufs_set_string(&bufs, 1, buf);

	/* We could put other opcodes here. */
	lcfg = malloc(lustre_cfg_len(bufs.lcfg_bufcount, bufs.lcfg_buflen));
	if (!lcfg) {
		rc = -ENOMEM;
	} else {
		lustre_cfg_init(lcfg, LCFG_PARAM, &bufs);
		rc = lcfg_mgs_ioctl(func, OBD_DEV_ID, lcfg);
		if (rc < 0)
			rc = -errno;
		free(lcfg);
	}

	return rc;
}

/*
 * Param set in config log on MGS
 * conf_param key=value
 *
 * Note we can actually send mgc conf_params from clients, but currently
 * that's only done for default file striping (see ll_send_mgc_param),
 * and not here.
 *
 * After removal of a parameter (-d) Lustre will use the default
 * AT NEXT REBOOT, not immediately.
 */
int jt_lcfg_confparam(int argc, char **argv)
{
	int rc;
	int del = 0;
	char *buf = NULL;

	/* mgs_setparam processes only lctl buf #1 */
	if ((argc > 3) || (argc <= 1))
		return CMD_HELP;

	while ((rc = getopt(argc, argv, "d")) != -1) {
		switch (rc) {
		case 'd':
			del = 1;
			break;
		default:
			return CMD_HELP;
		}
	}

	buf = argv[optind];

	if (del) {
		char *ptr;

		/* for delete, make it "<param>=\0" */
		buf = malloc(strlen(argv[optind]) + 2);
		if (!buf) {
			rc = -ENOMEM;
			goto out;
		}
		/* put an '=' on the end in case it doesn't have one */
		sprintf(buf, "%s=", argv[optind]);
		/* then truncate after the first '=' */
		ptr = strchr(buf, '=');
		*(++ptr) = '\0';
	}

	rc = lcfg_conf_param(argv[0], buf);

	if (buf != argv[optind])
		free(buf);
out:
	if (rc < 0) {
		fprintf(stderr, "error: %s: %s\n", jt_cmdname(argv[0]),
			strerror(-rc));
	}

	return rc;
}

/**
 * Display a parameter path in the same format as sysctl.
 * E.g. obdfilter.lustre-OST0000.stats
 *
 * \param[in] filename	file name of the parameter
 * \param[in] st	parameter file stats
 * \param[in] popt	set/get param options
 *
 * \retval allocated pointer containing modified filename
 */
static char *
display_name(const char *filename, struct stat *st, struct param_opts *popt)
{
	size_t suffix_len = 0;
	char *suffix = NULL;
	char *param_name;
	char *tmp;

	if (popt->po_show_type) {
		if (S_ISDIR(st->st_mode))
			suffix = "/";
		else if (S_ISLNK(st->st_mode))
			suffix = "@";
		else if (st->st_mode & S_IWUSR)
			suffix = "=";
	}

	/* Take the original filename string and chop off the glob addition */
	tmp = strstr(filename, "/lustre/");
	if (!tmp) {
		tmp = strstr(filename, "/lnet/");
		if (tmp)
			tmp += strlen("/lnet/");
	} else {
		tmp += strlen("/lustre/");
	}

	/* Allocate return string */
	param_name = strdup(tmp);
	if (!param_name)
		return NULL;

	/* replace '/' with '.' to match conf_param and sysctl */
	for (tmp = strchr(param_name, '/'); tmp != NULL; tmp = strchr(tmp, '/'))
		*tmp = '.';

	/* Append the indicator to entries if needed. */
	if (popt->po_show_type && suffix != NULL) {
		suffix_len = strlen(suffix);

		tmp = realloc(param_name, suffix_len + strlen(param_name) + 1);
		if (tmp) {
			param_name = tmp;
			strncat(param_name, suffix,
				strlen(param_name) + suffix_len);
		}
	}

	return param_name;
}

/**
 * Turns a lctl parameter string into a procfs/sysfs subdirectory path pattern.
 *
 * \param[in] popt		Used to control parameter usage. For this
 *				function it is used to see if the path has
 *				a added suffix.
 * \param[in,out] path		lctl parameter string that is turned into
 *				the subdirectory path pattern that is used
 *				to search the procfs/sysfs tree.
 *
 * \retval -errno on error.
 */
static int
clean_path(struct param_opts *popt, char *path)
{
	char *nidstart = NULL;
	char *nidend = NULL;
	char *tmp;

	if (popt == NULL || path == NULL || strlen(path) == 0)
		return -EINVAL;

	/* If path contains a suffix we need to remove it */
	if (popt->po_show_type) {
		size_t path_end = strlen(path) - 1;

		tmp = path + path_end;
		switch (*tmp) {
		case '@':
		case '=':
		case '/':
			*tmp = '\0';
		default:
			break;
		}
	}

	/* get rid of '\', glob doesn't like it */
	tmp = strrchr(path, '\\');
	if (tmp) {
		char *tail = path + strlen(path);

		while (tmp != path) {
			if (*tmp == '\\') {
				memmove(tmp, tmp + 1, tail - tmp);
				--tail;
			}
			--tmp;
		}
	}

	/* Does path contain a NID string?  Skip '.->/' replacement for it. */
	tmp = strchr(path, '@');
	if (tmp) {
		/* First find the NID start.  NIDs may have variable (0-4) '.',
		 * so find the common NID prefixes instead of trying to count
		 * the dots.  Not great, but there are only two, and faster
		 * than multiple speculative NID parses and bad DNS lookups.
		 */
		if ((tmp = strstr(path, ".exports.")))
			nidstart = tmp + strlen(".exports.");
		else if ((tmp = strstr(path, ".MGC")))
			nidstart = tmp + 1;

		/* Next, find the end of the NID string. */
		if (nidstart)
			nidend = strchrnul(strchr(nidstart, '@'), '.');
	}

	/* replace param '.' with '/' */
	for (tmp = strchr(path, '.'); tmp != NULL; tmp = strchr(tmp, '.')) {
		*tmp++ = '/';

		/*
		 * There exist cases where some of the subdirectories of the
		 * the parameter tree has embedded in its name a NID string.
		 * This means that it is possible that these subdirectories
		 * could have actual '.' in its name. If this is the case we
		 * don't want to blindly replace the '.' with '/', so skip
		 * over the part of the parameter containing the NID.
		 */
		if (tmp == nidstart)
			tmp = nidend;
	}

	return 0;
}

/**
 * The application lctl can perform three operations for lustre
 * tunables. This enum defines those three operations which are
 *
 * 1) LIST_PARAM	- list available tunables
 * 2) GET_PARAM		- report the current setting of a tunable
 * 3) SET_PARAM		- set the tunable to a new value
 */
enum parameter_operation {
	LIST_PARAM,
	GET_PARAM,
	SET_PARAM,
};

char *parameter_opname[] = {
	[LIST_PARAM] = "list_param",
	[GET_PARAM] = "get_param",
	[SET_PARAM] = "set_param",
};

/**
 * Read the value of parameter
 *
 * \param[in]	path		full path to the parameter
 * \param[in]	param_name	lctl parameter format of the
 *				parameter path
 * \param[in]	popt		set/get param options
 *
 * \retval 0 on success.
 * \retval -errno on error.
 */
static int
read_param(const char *path, const char *param_name, struct param_opts *popt)
{
	int rc = 0;
	char *buf = NULL;
	size_t buflen;

	rc = llapi_param_get_value(path, &buf, &buflen);
	if (rc != 0) {
		fprintf(stderr,
			"error: read_param: \'%s\': %s\n",
			path, strerror(-rc));
		goto free_buf;
	}
	/* don't print anything for empty files */
	if (buf[0] == '\0')
		goto free_buf;

	if (popt->po_show_path) {
		bool longbuf;

		longbuf = memchr(buf, '\n', buflen - 1) ||
			  buflen + strlen(param_name) >= 80;
		printf("%s=%s", param_name, longbuf ? "\n" : "");
	}
	printf("%s", buf);

free_buf:
	free(buf);
	return rc;
}

/**
 * Set a parameter to a specified value
 *
 * \param[in] path		full path to the parameter
 * \param[in] param_name	lctl parameter format of the parameter path
 * \param[in] popt		set/get param options
 * \param[in] value		value to set the parameter to
 *
 * \retval number of bytes written on success.
 * \retval -errno on error.
 */
static int
write_param(const char *path, const char *param_name, struct param_opts *popt,
	    const char *value)
{
	int fd, rc = 0;
	ssize_t count;

	if (!value)
		return -EINVAL;

	/* Write the new value to the file */
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		rc = -errno;
		fprintf(stderr, "error: set_param: opening '%s': %s\n",
			path, strerror(errno));
		return rc;
	}

	count = write(fd, value, strlen(value));
	if (count < 0) {
		rc = -errno;
		if (errno != EIO) {
			fprintf(stderr, "error: set_param: setting %s=%s: %s\n",
				path, value, strerror(errno));
		}
	} else if (count < strlen(value)) { /* Truncate case */
		rc = -EINVAL;
		fprintf(stderr,
			"error: set_param: setting %s=%s: wrote only %zd\n",
			path, value, count);
	} else if (popt->po_show_path) {
		printf("%s=%s\n", param_name, value);
	}
	close(fd);

	return rc;
}

void print_obd_line(char *s)
{
	const char *param = "osc/%s/ost_conn_uuid";
	char obd_name[MAX_OBD_NAME];
	char buf[MAX_OBD_NAME];
	FILE *fp = NULL;
	glob_t path;
	char *ptr;
retry:
	/* obd device type is the first 3 characters of param name */
	snprintf(buf, sizeof(buf), " %%*d %%*s %.3s %%%zus %%*s %%*d ",
		 param, sizeof(obd_name) - 1);
	if (sscanf(s, buf, obd_name) == 0)
		goto try_mdc;
	if (cfs_get_param_paths(&path, param, obd_name) != 0)
		goto try_mdc;
	fp = fopen(path.gl_pathv[0], "r");
	if (!fp) {
		/* need to free path data before retry */
		cfs_free_param_data(&path);
try_mdc:
		if (param[0] == 'o') { /* failed with osc, try mdc */
			param = "mdc/%s/mds_conn_uuid";
			goto retry;
		}
		buf[0] = '\0';
		goto fail_print;
	}

	/* should not ignore fgets(3)'s return value */
	if (!fgets(buf, sizeof(buf), fp)) {
		fprintf(stderr, "reading from %s: %s", buf, strerror(errno));
		goto fail_close;
	}

fail_close:
	fclose(fp);
	cfs_free_param_data(&path);

	/* trim trailing newlines */
	ptr = strrchr(buf, '\n');
	if (ptr)
		*ptr = '\0';
fail_print:
	ptr = strrchr(s, '\n');
	if (ptr)
		*ptr = '\0';
	printf("%s%s%s\n", s, buf[0] ? " " : "", buf);
}

int yaml_get_device_index(char *source)
{
	yaml_emitter_t request;
	yaml_parser_t reply;
	yaml_event_t event;
	struct nl_sock *sk;
	bool done = false;
	int rc;

	sk = nl_socket_alloc();
	if (!sk)
		return -EOPNOTSUPP;

	/* Setup parser to recieve Netlink packets */
	rc = yaml_parser_initialize(&reply);
	if (rc == 0)
		return -EOPNOTSUPP;

	rc = yaml_parser_set_input_netlink(&reply, sk, false);
	if (rc == 0)
		return -EOPNOTSUPP;

	/* Create Netlink emitter to send request to kernel */
	yaml_emitter_initialize(&request);
	rc = yaml_emitter_set_output_netlink(&request, sk, "lustre",
					     LUSTRE_GENL_VERSION,
					     LUSTRE_CMD_DEVICES, NLM_F_DUMP);
	if (rc == 0)
		goto error;

	yaml_emitter_open(&request);

	yaml_document_start_event_initialize(&event, NULL, NULL, NULL, 0);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_mapping_start_event_initialize(&event, NULL,
					    (yaml_char_t *)YAML_MAP_TAG,
					    1, YAML_ANY_MAPPING_STYLE);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_scalar_event_initialize(&event, NULL,
				     (yaml_char_t *)YAML_STR_TAG,
				     (yaml_char_t *)"devices",
				     strlen("devices"), 1, 0,
				     YAML_PLAIN_SCALAR_STYLE);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_sequence_start_event_initialize(&event, NULL,
					     (yaml_char_t *)YAML_SEQ_TAG,
					     1, YAML_ANY_SEQUENCE_STYLE);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_mapping_start_event_initialize(&event, NULL,
					    (yaml_char_t *)YAML_MAP_TAG,
					    1, YAML_ANY_MAPPING_STYLE);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_scalar_event_initialize(&event, NULL,
				     (yaml_char_t *)YAML_STR_TAG,
				     (yaml_char_t *)"name",
				     strlen("name"),
				     1, 0, YAML_PLAIN_SCALAR_STYLE);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_scalar_event_initialize(&event, NULL,
				     (yaml_char_t *)YAML_STR_TAG,
				     (yaml_char_t *)source,
				     strlen(source), 1, 0,
				     YAML_PLAIN_SCALAR_STYLE);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_mapping_end_event_initialize(&event);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_sequence_end_event_initialize(&event);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_mapping_end_event_initialize(&event);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_document_end_event_initialize(&event, 0);
	rc = yaml_emitter_emit(&request, &event);
	if (rc == 0)
		goto error;

	yaml_emitter_close(&request);
error:
	if (rc == 0) {
		yaml_emitter_log_error(&request, stderr);
		rc = -EINVAL;
	}
	yaml_emitter_delete(&request);

	while (!done) {
		rc = yaml_parser_parse(&reply, &event);
		if (rc == 0) {
			yaml_parser_log_error(&reply, stdout, "lctl: ");
			rc = -EINVAL;
			break;
		}

		if (event.type == YAML_SCALAR_EVENT) {
			char *value = (char *)event.data.scalar.value;

			if (strcmp(value, "index") == 0) {
				yaml_event_delete(&event);
				rc = yaml_parser_parse(&reply, &event);
				if (rc == 1) {
					value = (char *)event.data.scalar.value;
					errno = 0;
					rc = strtoul(value, NULL, 10);
					if (errno) {
						yaml_event_delete(&event);
						rc = -errno;
					}
					return rc;
				}
			}
		}
		done = (event.type == YAML_STREAM_END_EVENT);
		yaml_event_delete(&event);
	}

	nl_socket_free(sk);

	return rc;
}

/**
 * Perform a read, write or just a listing of a parameter
 *
 * \param[in] popt		list,set,get parameter options
 * \param[in] pattern		search filter for the path of the parameter
 * \param[in] value		value to set the parameter if write operation
 * \param[in] mode		what operation to perform with the parameter
 *
 * \retval number of bytes written on success.
 * \retval -errno on error and prints error message.
 */
static int
param_display(struct param_opts *popt, char *pattern, char *value,
	      enum parameter_operation mode)
{
	int dup_count = 0;
	char **dup_cache;
	glob_t paths;
	char *opname = parameter_opname[mode];
	int rc, i;

	rc = llapi_param_get_paths(pattern, &paths);
	if (rc != 0) {
		rc = -errno;
		if (!popt->po_recursive && !(rc == -ENOENT && getuid() != 0)) {
			fprintf(stderr, "error: %s: param_path '%s': %s\n",
				opname, pattern, strerror(errno));
		}
		return rc;
	}

	dup_cache = calloc(paths.gl_pathc, sizeof(char *));
	if (!dup_cache) {
		rc = -ENOMEM;
		fprintf(stderr,
			"error: %s: allocating '%s' dup_cache[%zd]: %s\n",
			opname, pattern, paths.gl_pathc, strerror(-rc));
		goto out_param;
	}

	for (i = 0; i < paths.gl_pathc; i++) {
		char *param_name = NULL, *tmp;
		char pathname[PATH_MAX], param_dir[PATH_MAX + 2];
		struct stat st;
		int rc2, j;

		if (stat(paths.gl_pathv[i], &st) == -1) {
			fprintf(stderr, "error: %s: stat '%s': %s\n",
				opname, paths.gl_pathv[i], strerror(errno));
			if (rc == 0)
				rc = -errno;
			continue;
		}

		if (popt->po_only_dir && !S_ISDIR(st.st_mode))
			continue;

		param_name = display_name(paths.gl_pathv[i], &st, popt);
		if (!param_name) {
			fprintf(stderr,
				"error: %s: generating name for '%s': %s\n",
				opname, paths.gl_pathv[i], strerror(ENOMEM));
			if (rc == 0)
				rc = -ENOMEM;
			continue;
		}

		switch (mode) {
		case GET_PARAM:
			/* Read the contents of file to stdout */
			if (S_ISREG(st.st_mode)) {
				rc2 = read_param(paths.gl_pathv[i], param_name,
						 popt);
				if (rc2 < 0 && rc == 0)
					rc = rc2;
			}
			break;
		case SET_PARAM:
			if (S_ISREG(st.st_mode)) {
				rc2 = write_param(paths.gl_pathv[i],
						  param_name, popt, value);
				if (rc2 < 0 && rc == 0)
					rc = rc2;
			}
			break;
		case LIST_PARAM:
			/**
			 * For the upstream client the parameter files locations
			 * are split between under both /sys/kernel/debug/lustre
			 * and /sys/fs/lustre. The parameter files containing
			 * small amounts of data, less than a page in size, are
			 * located under /sys/fs/lustre and in the case of large
			 * parameter data files, think stats for example, are
			 * located in the debugfs tree. Since the files are
			 * split across two trees the directories are often
			 * duplicated which means these directories are listed
			 * twice which leads to duplicate output to the user.
			 * To avoid scanning a directory twice we have to cache
			 * any directory and check if a search has been
			 * requested twice.
			 */
			for (j = 0; j < dup_count; j++) {
				if (!strcmp(dup_cache[j], param_name))
					break;
			}
			if (j != dup_count) {
				free(param_name);
				param_name = NULL;
				continue;
			}
			dup_cache[dup_count++] = strdup(param_name);

			if (popt->po_show_path)
				printf("%s\n", param_name);
			break;
		}

		/*
		 * Only directories are searched recursively if
		 * requested by the user
		 */
		if (!S_ISDIR(st.st_mode) || !popt->po_recursive) {
			free(param_name);
			param_name = NULL;
			continue;
		}

		/* Turn param_name into file path format */
		rc2 = clean_path(popt, param_name);
		if (rc2 < 0) {
			fprintf(stderr, "error: %s: cleaning '%s': %s\n",
				opname, param_name, strerror(-rc2));
			free(param_name);
			param_name = NULL;
			if (rc == 0)
				rc = rc2;
			continue;
		}

		/* Use param_name to grab subdirectory tree from full path */
		snprintf(param_dir, sizeof(param_dir), "/%s", param_name);
		tmp = strstr(paths.gl_pathv[i], param_dir);

		/* cleanup paramname now that we are done with it */
		free(param_name);
		param_name = NULL;
		memset(&param_dir, '\0', sizeof(param_dir));

		/* Shouldn't happen but just in case */
		if (!tmp) {
			if (rc == 0)
				rc = -EINVAL;
			continue;
		}
		tmp++;

		rc2 = snprintf(pathname, sizeof(pathname), "%s/*", tmp);
		if (rc2 < 0) {
			/*
			 * snprintf() should never an error, and if it does
			 * there isn't much point trying to use fprintf()
			 */
			continue;
		}
		if (rc2 >= sizeof(pathname)) {
			fprintf(stderr, "error: %s: overflow processing '%s'\n",
				opname, pathname);
			if (rc == 0)
				rc = -EINVAL;
			continue;
		}

		rc2 = param_display(popt, pathname, value, mode);
		if (rc2 != 0 && rc2 != -ENOENT) {
			/* errors will be printed by param_display() */
			if (rc == 0)
				rc = rc2;
			continue;
		}
	}

	for (i = 0; i < dup_count; i++)
		free(dup_cache[i]);
	free(dup_cache);
out_param:
	llapi_param_paths_free(&paths);
	return rc;
}

static int listparam_cmdline(int argc, char **argv, struct param_opts *popt)
{
	int ch;

	popt->po_show_path = 1;
	popt->po_only_path = 1;

	while ((ch = getopt(argc, argv, "FRD")) != -1) {
		switch (ch) {
		case 'F':
			popt->po_show_type = 1;
			break;
		case 'R':
			popt->po_recursive = 1;
			break;
		case 'D':
			popt->po_only_dir = 1;
			break;
		default:
			return -1;
		}
	}

	return optind;
}

int jt_lcfg_listparam(int argc, char **argv)
{
	int rc = 0, index, i;
	struct param_opts popt;
	char *path;

	memset(&popt, 0, sizeof(popt));
	index = listparam_cmdline(argc, argv, &popt);
	if (index < 0 || index >= argc)
		return CMD_HELP;

	for (i = index; i < argc; i++) {
		int rc2;

		path = argv[i];

		rc2 = clean_path(&popt, path);
		if (rc2 < 0) {
			fprintf(stderr, "error: %s: cleaning '%s': %s\n",
				jt_cmdname(argv[0]), path, strerror(-rc2));
			if (rc == 0)
				rc = rc2;
			continue;
		}

		rc2 = param_display(&popt, path, NULL, LIST_PARAM);
		if (rc2 < 0) {
			if (rc == 0)
				rc = rc2;

			if (rc2 == -ENOENT && getuid() != 0)
				rc2 = llapi_param_display_value(path, 0, 0,
								stdout);
			if (rc2 < 0) {
				fprintf(stderr, "error: %s: listing '%s': %s\n",
					jt_cmdname(argv[0]), path,
					strerror(-rc2));
			}
			continue;
		}
	}

	return rc;
}

static int getparam_cmdline(int argc, char **argv, struct param_opts *popt)
{
	int ch;

	popt->po_show_path = 1;

	while ((ch = getopt(argc, argv, "FnNRy")) != -1) {
		switch (ch) {
		case 'F':
			popt->po_show_type = 1;
			break;
		case 'n':
			popt->po_show_path = 0;
			break;
		case 'N':
			popt->po_only_path = 1;
			break;
		case 'R':
			popt->po_recursive = 1;
			break;
		case 'y':
			popt->po_yaml = 1;
			break;
		default:
			return -1;
		}
	}

	return optind;
}

int jt_lcfg_getparam(int argc, char **argv)
{
	int version = LUSTRE_GENL_VERSION;
	enum parameter_operation mode;
	int rc = 0, index, i;
	struct param_opts popt;
	int flags = 0;
	char *path;

	memset(&popt, 0, sizeof(popt));
	index = getparam_cmdline(argc, argv, &popt);
	if (index < 0 || index >= argc)
		return CMD_HELP;

	mode = popt.po_only_path ? LIST_PARAM : GET_PARAM;
	if (mode == LIST_PARAM)
		version = 0;

	if (popt.po_yaml)
		flags |= PARAM_FLAGS_YAML_FORMAT;
	if (popt.po_show_path)
		flags |= PARAM_FLAGS_SHOW_SOURCE;

	for (i = index; i < argc; i++) {
		int rc2;

		path = argv[i];

		rc2 = clean_path(&popt, path);
		if (rc2 < 0) {
			fprintf(stderr, "error: %s: cleaning '%s': %s\n",
				jt_cmdname(argv[0]), path, strerror(-rc2));
			if (rc == 0)
				rc = rc2;
			continue;
		}

		rc2 = param_display(&popt, path, NULL, mode);
		if (rc2 < 0) {
			if (rc == 0)
				rc = rc2;

			if (rc2 == -ENOENT && getuid() != 0)
				rc2 = llapi_param_display_value(path, version,
								flags, stdout);
			continue;
		}
	}

	return rc;
}

/* get device list by netlink or debugfs */
int jt_device_list(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ .name = "target",	.has_arg = no_argument,	.val = 't' },
		{ .name = "yaml",	.has_arg = no_argument,	.val = 'y' },
		{ .name = NULL }
	};
	struct param_opts opts;
	char buf[MAX_OBD_NAME];
	int flags = 0;
	glob_t path;
	int rc, c;
	FILE *fp;

	if (optind + 1 < argc)
		return CMD_HELP;

	memset(&opts, 0, sizeof(opts));

	while ((c = getopt_long(argc, argv, "ty", long_opts, NULL)) != -1) {
		switch (c) {
		case 't':
			flags |= PARAM_FLAGS_EXTRA_DETAILS;
			opts.po_detail = true;
			break;
		case 'y':
			flags |= PARAM_FLAGS_YAML_FORMAT;
			opts.po_yaml = true;
			break;
		default:
			return CMD_HELP;
		}
	}

	if (optind < argc) {
		optind = 1;
		return CMD_HELP;
	}
	optind = 1;

	/* Use YAML to list all devices */
	rc = llapi_param_display_value("devices", LUSTRE_GENL_VERSION, flags,
				       stdout);
	if (rc == 0)
		return 0;

	rc = llapi_param_get_paths("devices", &path);
	if (rc < 0)
		return rc;

	fp = fopen(path.gl_pathv[0], "r");
	if (!fp) {
		cfs_free_param_data(&path);
		return errno;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL)
		if (opts.po_detail)
			print_obd_line(buf);
		else
			printf("%s", buf);

	cfs_free_param_data(&path);
	fclose(fp);
	return 0;
}

static int do_name2dev(char *func, char *name, int dev_id)
{
	struct obd_ioctl_data data;
	char rawbuf[MAX_IOC_BUFLEN], *buf = rawbuf;
	int rc;

	/* Use YAML to find device index */
	rc = yaml_get_device_index(name);
	if (rc >= 0 || rc != -EOPNOTSUPP)
		return rc;

	memset(&data, 0, sizeof(data));
	data.ioc_dev = dev_id;
	data.ioc_inllen1 = strlen(name) + 1;
	data.ioc_inlbuf1 = name;

	memset(buf, 0, sizeof(rawbuf));
	rc = llapi_ioctl_pack(&data, &buf, sizeof(rawbuf));
	if (rc < 0) {
		fprintf(stderr, "error: %s: invalid ioctl\n", jt_cmdname(func));
		return rc;
	}
	rc = l_ioctl(OBD_DEV_ID, OBD_IOC_NAME2DEV, buf);
	if (rc < 0)
		return -errno;
	rc = llapi_ioctl_unpack(&data, buf, sizeof(rawbuf));
	if (rc < 0) {
		fprintf(stderr, "error: %s: invalid reply\n", jt_cmdname(func));
		return rc;
	}

	return data.ioc_dev;
}

/*
 * resolve a device name to a device number.
 * supports a number, $name or %uuid.
 */
int parse_devname(char *func, char *name, int dev_id)
{
	int rc = 0;
	int ret = -1;

	if (!name)
		return ret;

	/* Test if its a pure number string */
	if (strspn(name, "0123456789") != strlen(name)) {
		if (name[0] == '$' || name[0] == '%')
			name++;

		rc = do_name2dev(func, name, dev_id);
		if (rc >= 0)
			ret = rc;
	} else {
		errno = 0;
		ret = strtoul(name, NULL, 10);
		if (errno)
			rc = errno;
	}

	if (rc < 0)
		fprintf(stderr, "No device found for name %s: %s\n",
			name, strerror(rc));
	return ret;
}

#ifdef HAVE_SERVER_SUPPORT
/**
 * Output information about nodemaps.
 * \param	argc		number of args
 * \param	argv[]		variable string arguments
 *
 * [list|nodemap_name|all]	\a list will list all nodemaps (default).
 *				Specifying a \a nodemap_name will
 *				display info about that specific nodemap.
 *				\a all will display info for all nodemaps.
 * \retval			0 on success
 */
int jt_nodemap_info(int argc, char **argv)
{
	const char usage_str[] = "usage: nodemap_info [list|nodemap_name|all]\n";
	struct param_opts popt;
	int rc = 0;

	memset(&popt, 0, sizeof(popt));
	popt.po_show_path = 1;

	if (argc > 2) {
		fprintf(stderr, usage_str);
		return -1;
	}

	if (argc == 1 || strcmp("list", argv[1]) == 0) {
		popt.po_only_dir = 1;
		rc = param_display(&popt, "nodemap/*", NULL, LIST_PARAM);
	} else if (strcmp("all", argv[1]) == 0) {
		rc = param_display(&popt, "nodemap/*/*", NULL, GET_PARAM);
	} else {
		char	pattern[PATH_MAX];

		snprintf(pattern, sizeof(pattern), "nodemap/%s/*", argv[1]);
		rc = param_display(&popt, pattern, NULL, GET_PARAM);
		if (rc == -ESRCH)
			fprintf(stderr,
				"error: nodemap_info: cannot find nodemap %s\n",
				argv[1]);
	}
	return rc;
}
#endif

static int setparam_cmdline(int argc, char **argv, struct param_opts *popt)
{
	int ch;

	popt->po_show_path = 1;
	popt->po_only_path = 0;
	popt->po_show_type = 0;
	popt->po_recursive = 0;
	popt->po_perm = 0;
	popt->po_delete = 0;
	popt->po_file = 0;

	while ((ch = getopt(argc, argv, "nPdF")) != -1) {
		switch (ch) {
		case 'n':
			popt->po_show_path = 0;
			break;
		case 'P':
			popt->po_perm = 1;
			break;
		case 'd':
			popt->po_delete = 1;
			break;
		case 'F':
			popt->po_file = 1;
			break;
		default:
			return -1;
		}
	}
	return optind;
}

enum paramtype {
	PT_NONE = 0,
	PT_SETPARAM,
	PT_CONFPARAM
};

#define PS_NONE 0
#define PS_PARAM_FOUND 1
#define PS_PARAM_SET 2
#define PS_VAL_FOUND 4
#define PS_VAL_SET 8
#define PS_DEVICE_FOUND 16
#define PS_DEVICE_SET 32

#define PARAM_SZ 256

static struct cfg_type_data {
	enum paramtype ptype;
	char *type_name;
} cfg_type_table[] = {
	{ PT_SETPARAM, "set_param" },
	{ PT_CONFPARAM, "conf_param" },
	{ PT_NONE, "none" }
};

static struct cfg_stage_data {
	int pstage;
	char *stage_name;
} cfg_stage_table[] = {
	{ PS_PARAM_FOUND, "parameter" },
	{ PS_VAL_FOUND, "value" },
	{ PS_DEVICE_FOUND, "device" },
	{ PS_NONE, "none" }
};

void conf_to_set_param(enum paramtype confset, const char *param,
		       const char *device, char *buf,
		       int bufsize)
{
	char *tmp;

	if (confset == PT_SETPARAM) {
		strncpy(buf, param, bufsize);
		return;
	}

	/*
	 * sys.* params are top level, we just need to trim the sys.
	 */
	tmp = strstr(param, "sys.");
	if (tmp) {
		tmp += 4;
		strncpy(buf, tmp, bufsize);
		return;
	}

	/*
	 * parameters look like type.parameter, we need to stick the device
	 * in the middle.  Example combine mdt.identity_upcall with device
	 * lustre-MDT0000 for mdt.lustre-MDT0000.identity_upcall
	 */

	tmp = strchrnul(param, '.');
	snprintf(buf, tmp - param + 1, "%s", param);
	buf += tmp - param;
	bufsize -= tmp - param;
	snprintf(buf, bufsize, ".%s%s", device, tmp);
}

int lcfg_setparam_yaml(char *func, char *filename)
{
	FILE *file;
	yaml_parser_t parser;
	yaml_token_t token;
	int rc = 0;

	enum paramtype confset = PT_NONE;
	int param = PS_NONE;
	char *tmp;
	char parameter[PARAM_SZ + 1];
	char value[PARAM_SZ + 1];
	char device[PARAM_SZ + 1];

	file = fopen(filename, "rb");
	yaml_parser_initialize(&parser);
	yaml_parser_set_input_file(&parser, file);

	/*
	 * Search tokens for conf_param or set_param
	 * The token after "parameter" goes into parameter
	 * The token after "value" goes into value
	 * when we have all 3, create param=val and call the
	 * appropriate function for set/conf param
	 */
	while (token.type != YAML_STREAM_END_TOKEN && rc == 0) {
		int i;

		yaml_token_delete(&token);
		if (!yaml_parser_scan(&parser, &token)) {
			rc = 1;
			break;
		}

		if (token.type != YAML_SCALAR_TOKEN)
			continue;

		for (i = 0; cfg_type_table[i].ptype != PT_NONE; i++) {
			if (!strncmp((char *)token.data.alias.value,
				     cfg_type_table[i].type_name,
				     strlen(cfg_type_table[i].type_name))) {
				confset = cfg_type_table[i].ptype;
				break;
			}
		}

		if (confset == PT_NONE)
			continue;

		for (i = 0; cfg_stage_table[i].pstage != PS_NONE; i++) {
			if (!strncmp((char *)token.data.alias.value,
				     cfg_stage_table[i].stage_name,
				     strlen(cfg_stage_table[i].stage_name))) {
				param |= cfg_stage_table[i].pstage;
				break;
			}
		}

		if (cfg_stage_table[i].pstage != PS_NONE)
			continue;

		if (param & PS_PARAM_FOUND) {
			conf_to_set_param(confset,
					  (char *)token.data.alias.value,
					  device, parameter, PARAM_SZ);
			param |= PS_PARAM_SET;
			param &= ~PS_PARAM_FOUND;

			/*
			 * we're getting parameter: param=val
			 * copy val and mark that we've got it in case
			 * there is no value: tag
			 */
			tmp = strchrnul(parameter, '=');
			if (*tmp == '=') {
				strncpy(value, tmp + 1, sizeof(value) - 1);
				*tmp = '\0';
				param |= PS_VAL_SET;
			} else {
				continue;
			}
		} else if (param & PS_VAL_FOUND) {
			strncpy(value, (char *)token.data.alias.value,
				PARAM_SZ);
			param |= PS_VAL_SET;
			param &= ~PS_VAL_FOUND;
		} else if (param & PS_DEVICE_FOUND) {
			strncpy(device, (char *)token.data.alias.value,
				PARAM_SZ);
			param |= PS_DEVICE_SET;
			param &= ~PS_DEVICE_FOUND;
		}

		if (confset && param & PS_VAL_SET && param & PS_PARAM_SET) {
			int size = strlen(parameter) + strlen(value) + 2;
			char *buf = malloc(size);

			if (!buf) {
				rc = 2;
				break;
			}
			snprintf(buf, size, "%s=%s", parameter, value);

			printf("set_param: %s\n", buf);
			rc = lcfg_setparam_perm(func, buf);

			confset = PT_NONE;
			param = PS_NONE;
			parameter[0] = '\0';
			value[0] = '\0';
			device[0] = '\0';
			free(buf);
		}
	}

	yaml_parser_delete(&parser);
	fclose(file);

	return rc;
}

int jt_lcfg_setparam(int argc, char **argv)
{
	int rc = 0, index, i;
	struct param_opts popt;
	char *path = NULL, *value = NULL;

	memset(&popt, 0, sizeof(popt));
	index = setparam_cmdline(argc, argv, &popt);
	if (index < 0 || index >= argc)
		return CMD_HELP;

	if (popt.po_perm)
		/*
		 * We can't delete parameters that were
		 * set with old conf_param interface
		 */
		return jt_lcfg_setparam_perm(argc, argv, &popt);

	if (popt.po_file)
		return lcfg_setparam_yaml(argv[0], argv[index]);

	for (i = index; i < argc; i++) {
		int rc2;

		path = argv[i];
		value = strchr(path, '=');
		if (value) {
			/* format: set_param a=b */
			*value = '\0';
			value++;
			if (*value == '\0') {
				fprintf(stderr,
					"error: %s: setting %s: no value\n",
					jt_cmdname(argv[0]), path);
				if (rc == 0)
					rc = -EINVAL;
				continue;
			}
		} else {
			/* format: set_param a b */
			i++;
			if (i >= argc) {
				fprintf(stderr,
					"error: %s: setting %s: no value\n",
					jt_cmdname(argv[0]), path);
				if (rc == 0)
					rc = -EINVAL;
				break;
			}
			value = argv[i];
		}

		rc2 = clean_path(&popt, path);
		if (rc2 < 0) {
			fprintf(stderr, "error: %s: cleaning %s: %s\n",
				jt_cmdname(argv[0]), path, strerror(-rc2));
			if (rc == 0)
				rc = rc2;
			continue;
		}

		rc2 = param_display(&popt, path, value, SET_PARAM);
		if (rc == 0)
			rc = rc2;
	}

	return rc;
}
