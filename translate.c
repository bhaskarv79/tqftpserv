// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2019, Linaro Ltd.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "translate.h"
#include "zstd-decompress.h"

#define READONLY_PATH	"/readonly/firmware/image/"
#define READWRITE_PATH	"/readwrite/"
#define UPDATES_DIR	"updates/"

#ifndef ANDROID
#define FIRMWARE_BASE	"/lib/firmware/"
#define TQFTPSERV_TMP	"/tmp/tqftpserv"
#else
#define FIRMWARE_BASE	"/vendor/firmware/"
#define TQFTPSERV_TMP	"/data/vendor/tmp/tqftpserv"
#endif

/* The default mode bits for directories */
#define TFTP_DEFAULT_DIR_MODE 0700

/**
 * struct instance_path_entry - maps a subsystem instance ID to its RFS path
 * @instance: subsystem instance ID
 * @path:     base directory for this instance's files (with trailing slash)
 *
 * Modelled after the downstream tftp_server_folders_path_prefix_map[].
 * Using an explicit struct avoids the fragile "array index == instance - 1"
 * assumption and makes it safe to add/reorder entries.
 */
struct instance_path_entry {
	enum tftp_server_instance_id_type instance;
	const char *path;
};

static const struct instance_path_entry instance_path_map[] = {
	{ TFTP_SERVER_INSTANCE_ID_MSM_MPSS, FIRMWARE_BASE"rfs/msm/mpss/" },
	{ TFTP_SERVER_INSTANCE_ID_MSM_ADSP, FIRMWARE_BASE"rfs/msm/adsp/" },
	{ TFTP_SERVER_INSTANCE_ID_MDM_MPSS, FIRMWARE_BASE"rfs/mdm/mpss/" },
	{ TFTP_SERVER_INSTANCE_ID_MDM_ADSP, FIRMWARE_BASE"rfs/mdm/adsp/" },
	{ TFTP_SERVER_INSTANCE_ID_MDM_TN,   FIRMWARE_BASE"rfs/mdm/tn/"   },
	{ TFTP_SERVER_INSTANCE_ID_APQ_GSS,  FIRMWARE_BASE"rfs/apq/gnss/" },
	{ TFTP_SERVER_INSTANCE_ID_MSM_SLPI, FIRMWARE_BASE"rfs/msm/slpi/" },
	{ TFTP_SERVER_INSTANCE_ID_MDM_SLPI, FIRMWARE_BASE"rfs/mdm/slpi/" },
	{ TFTP_SERVER_INSTANCE_ID_MSM_CDSP, FIRMWARE_BASE"rfs/msm/cdsp/" },
	{ TFTP_SERVER_INSTANCE_ID_MDM_CDSP, FIRMWARE_BASE"rfs/mdm/cdsp/" },
};

#define INSTANCE_PATH_MAP_SIZE \
	(sizeof(instance_path_map) / sizeof(instance_path_map[0]))

/* Subdirectories created under each instance base path */
static const char * const instance_subdirs[] = {
	"readwrite/",
	"readonly/",
};

#define INSTANCE_SUBDIRS_SIZE \
	(sizeof(instance_subdirs) / sizeof(instance_subdirs[0]))

static int open_maybe_compressed(const char *path);
static const char *instance_lookup_path(enum tftp_server_instance_id_type instance);

static void read_fw_path_from_sysfs(char *outbuffer, size_t bufsize)
{
	size_t pathsize;
	FILE *f = fopen("/sys/module/firmware_class/parameters/path", "rt");
	if (!f)
		return;
	pathsize = fread(outbuffer, sizeof(char), bufsize, f);
	fclose(f);
	if (pathsize == 0)
		return;
	/* truncate newline */
	outbuffer[pathsize - 1] = '\0';
}

/**
 * translate_readonly() - open "file" residing with remoteproc firmware
 * @file:	file requested, stripped of "/readonly/image/" prefix
 *
 * It is assumed that the readonly files requested by the client resides under
 * /lib/firmware in the same place as its associated remoteproc firmware.  This
 * function scans through all entries under /sys/class/remoteproc and read the
 * dirname of each "firmware" file in an attempt to find, and open(2), the
 * requested file.
 *
 * As these files are readonly, it's not possible to pass flags to open(2).
 *
 * Return: opened fd on success, -1 otherwise
 */
static int translate_readonly(const char *file)
{
	char firmware_value[PATH_MAX];
	char *firmware_value_copy = NULL;
	char *firmware_path;
	char firmware_attr[32];
	char path[PATH_MAX];
	char fw_sysfs_path[PATH_MAX];
	struct dirent *de;
	int firmware_fd;
	DIR *class_dir;
	int class_fd;
	ssize_t n;
	int fd = -1;

	read_fw_path_from_sysfs(fw_sysfs_path, sizeof(fw_sysfs_path));

	class_fd = open("/sys/class/remoteproc", O_RDONLY | O_DIRECTORY);
	if (class_fd < 0) {
		warn("failed to open remoteproc class");
		return -1;
	}

	class_dir = fdopendir(class_fd);
	if (!class_dir) {
		warn("failed to opendir");
		close(class_fd);
		return -1;
	}

	while ((de = readdir(class_dir)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		if (strlen(de->d_name) + sizeof("/firmware") > sizeof(firmware_attr))
			continue;
		strcpy(firmware_attr, de->d_name);
		strcat(firmware_attr, "/firmware");

		firmware_fd = openat(class_fd, firmware_attr, O_RDONLY);
		if (firmware_fd < 0)
			continue;

		n = read(firmware_fd, firmware_value, sizeof(firmware_value));
		close(firmware_fd);
		if (n < 0) {
			continue;
		}
		firmware_value[n] = '\0';

		firmware_value_copy = strdup(firmware_value);
		firmware_path = dirname(firmware_value_copy);

		/* first try path from sysfs */
		if ((strlen(fw_sysfs_path) > 0) &&
		    (strlen(fw_sysfs_path) + 1 + strlen(firmware_value) + 1 + strlen(file) + 1 < sizeof(path))) {
			strcpy(path, fw_sysfs_path);
			strcat(path, "/");
			strcat(path, firmware_path);
			strcat(path, "/");
			strcat(path, file);

			fd = open_maybe_compressed(path);
			if (fd >= 0)
				break;
			if (errno != ENOENT)
				warn("failed to open %s", path);
		}

		/* now try with base path */
		if (strlen(FIRMWARE_BASE) + strlen(UPDATES_DIR) + strlen(firmware_value) + 1 +
		    strlen(file) + 1 > sizeof(path))
			continue;

		strcpy(path, FIRMWARE_BASE);
		strcat(path, UPDATES_DIR);
		strcat(path, firmware_path);
		strcat(path, "/");
		strcat(path, file);

		fd = open_maybe_compressed(path);
		if (fd < 0) {
			strcpy(path, FIRMWARE_BASE);
			strcat(path, firmware_path);
			strcat(path, "/");
			strcat(path, file);

			fd = open_maybe_compressed(path);
		}
		if (fd >= 0)
			break;

		if (errno != ENOENT)
			warn("failed to open %s", path);
	}

	free(firmware_value_copy);
	closedir(class_dir);

	return fd;
}

/**
 * translate_readwrite() - open "file" from a temporary directory
 * @file:	relative path of the requested file, with /readwrite/ stripped
 * @flags:	flags to be passed to open(2)
 *
 * Return: opened fd on success, -1 otherwise
 */
static int translate_readwrite(const char *file, int flags)
{
	int base;
	int ret;
	int fd;

	ret = mkdir(TQFTPSERV_TMP, 0700);
	if (ret < 0 && errno != EEXIST) {
		warn("failed to create temporary tqftpserv directory");
		return -1;
	}

	base = open(TQFTPSERV_TMP, O_RDONLY | O_DIRECTORY);
	if (base < 0) {
		warn("failed top open temporary tqftpserv directory");
		return -1;
	}

	fd = openat(base, file, flags, 0600);
	close(base);
	if (fd < 0)
		warn("failed to open %s", file);

	return fd;
}

/**
 * translate_open() - open file after translating path with instance awareness
 * @path: requested file path from the TFTP client (no leading slash)
 * @flags: flags to be passed to open(2)
 * @target_instance: subsystem instance ID identifying the RFS client
 *
 * Looks up the per-instance base directory from instance_path_map[] and
 * opens the file relative to that directory.
 *
 * Return: opened fd on success, -1 otherwise
 */
int translate_open(const char *path, int flags,
		   enum tftp_server_instance_id_type target_instance)
{
	char full_path[PATH_MAX];
	const char *instance_folder;
	int fd;

	instance_folder = instance_lookup_path(target_instance);
	if (!instance_folder) {
		warn("No path mapping for instance %d, file %s",
		     target_instance, path);
		errno = EINVAL;
		return -1;
	}

	if (strlen(instance_folder) + strlen(path) + 1 > sizeof(full_path)) {
		warn("Path too long for instance %d, file %s",
		     target_instance, path);
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(full_path, sizeof(full_path), "%s%s", instance_folder, path);

	/* Collapse any accidental double slashes (e.g. if path starts with /) */
	{
		char *src = full_path, *dst = full_path;
		while (*src) {
			if (*src == '/' && *(src + 1) == '/')
				src++;
			else
				*dst++ = *src++;
		}
		*dst = '\0';
	}

	fd = open(full_path, flags, 0600);
	if (fd < 0)
		warn("failed to open %s", full_path);

	return fd;
}

/* linux-firmware uses .zst as file extension */
#define ZSTD_EXTENSION ".zst"

/**
 * open_maybe_compressed() - open a file and maybe decompress it if necessary
 * @filename:	path to a file that may be compressed (should not include compression format extension)
 *
 * Return: opened fd on success, -1 on error
 */
static int open_maybe_compressed(const char *path)
{
	char *path_with_zstd_extension = NULL;
	int fd = -1;

	if (access(path, F_OK) == 0)
		return open(path, O_RDONLY);

	asprintf(&path_with_zstd_extension, "%s%s", path, ZSTD_EXTENSION);

	if (access(path_with_zstd_extension, F_OK) == 0)
		fd = zstd_decompress_file(path_with_zstd_extension);

	free(path_with_zstd_extension);

	return fd;
}

/**
 * instance_lookup_path() - look up the RFS path for a subsystem instance
 * @instance: subsystem instance ID
 *
 * Return: path string on success, NULL if instance is not in the map
 */
static const char *instance_lookup_path(enum tftp_server_instance_id_type instance)
{
	size_t i;

	for (i = 0; i < INSTANCE_PATH_MAP_SIZE; i++) {
		if (instance_path_map[i].instance == instance)
			return instance_path_map[i].path;
	}
	return NULL;
}

/**
 * mkdir_p() - create a directory and all missing parent directories
 * @path: full directory path to create
 * @mode: permission bits for newly created directories
 *
 * Walks each component of @path and creates any missing directories,
 * equivalent to 'mkdir -p'. Existing directories are silently skipped.
 *
 * Return: 0 on success, -1 on error (errno set)
 */
static int mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	char *p;
	size_t len;

	len = (size_t)snprintf(tmp, sizeof(tmp), "%s", path);
	if (len >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* Strip trailing slash so the loop handles the leaf component too */
	if (len > 1 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	/* Create each intermediate component */
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, mode) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}

	/* Create the leaf directory */
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return -1;

	return 0;
}

/**
 * translate_folders_create_data_folders() - create data folders for subsystems
 *
 * Creates the base directory and readwrite/readonly subdirectories for each
 * subsystem instance, equivalent to the downstream tftp_server behaviour.
 *
 * Return: 0 on success, -1 on error
 */
static int translate_folders_create_data_folders(void)
{
	char dir_path[PATH_MAX];
	size_t i, j;
	int result, error_count = 0;
	const char *base;

	for (i = 0; i < INSTANCE_PATH_MAP_SIZE; i++) {
		base = instance_path_map[i].path;

		/* Create the base directory */
		result = mkdir_p(base, TFTP_DEFAULT_DIR_MODE);
		if (result != 0 && errno != EEXIST) {
			error_count++;
			warn("Failed to create directory %s", base);
		}

		/* Create readwrite/ and readonly/ subdirectories */
		for (j = 0; j < INSTANCE_SUBDIRS_SIZE; j++) {
			if (snprintf(dir_path, sizeof(dir_path), "%s%s",
				     base, instance_subdirs[j]) >= (int)sizeof(dir_path)) {
				error_count++;
				warn("Path too long: %s%s", base, instance_subdirs[j]);
				continue;
			}

			result = mkdir_p(dir_path, TFTP_DEFAULT_DIR_MODE);
			if (result != 0 && errno != EEXIST) {
				error_count++;
				warn("Failed to create directory %s", dir_path);
			}
		}
	}

	return (error_count > 0) ? -1 : 0;
}

/**
 * translate_folders_init() - initialize folder structure at server startup
 *
 * Creates the per-subsystem RFS directory tree under FIRMWARE_BASE so that
 * the TFTP server can serve files to each remote subsystem instance.
 *
 * Return: 0 on success, -1 on error
 */
int translate_folders_init(void)
{
	return translate_folders_create_data_folders();
}
