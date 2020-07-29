/****************************************************************************
 *
 * Copyright 2020 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <tinyara/config.h>
#include <debug.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <tinyara/binary_manager.h>

#include "binary_manager.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/
/****************************************************************************
 * Name: binary_manager_clear_binfile
 *
 * Description:
 *	 This function clears binary files with old version.
 *
 ****************************************************************************/
static int binary_manager_clear_binfile(int bin_idx)
{
	DIR *dirp;
	int name_len;
	char *bin_name;
	char filepath[CONFIG_PATH_MAX];
	char running_file[NAME_MAX];

	snprintf(running_file, NAME_MAX, "%s_%d", BIN_NAME(bin_idx), BIN_VER(bin_idx));
	bin_name = BIN_NAME(bin_idx);
	name_len = strlen(bin_name);

	/* Open a directory for user binaries, BINARY_DIR_PATH */
	dirp = opendir(BINARY_DIR_PATH);
	if (dirp) {
		/* Read each directory entry */
		for (;;) {
			struct dirent *entryp = readdir(dirp);
			if (!entryp) {
				/* Finished with this directory */
				break;
			}
			/* Remove binary file which is not running */
			if (DIRENT_ISFILE(entryp->d_type) && !strncmp(entryp->d_name, bin_name, name_len) \
				&& entryp->d_name[name_len] == '_' && strncmp(entryp->d_name, running_file, strlen(running_file))) { 
				snprintf(filepath, NAME_MAX, "%s/%s", BINARY_DIR_PATH, entryp->d_name);
				bmvdbg("unlink %s\n", entryp->d_name);
				unlink(filepath);
			}
		}
		closedir(dirp);
	} else if (errno != ENOENT) {
		bmdbg("Failed to open a directory, %s\n", BINARY_DIR_PATH);
		return BINMGR_OPERATION_FAIL;
	}

	return BINMGR_OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/
/****************************************************************************
 * Name: binary_manager_scan_ubin
 *
 * Description:
 *	 This function scans user binary files.
 *
 ****************************************************************************/
void binary_manager_scan_ubin(void)
{
	int ret;
	DIR *dirp;
	binary_header_t header_data;
	char filepath[CONFIG_PATH_MAX];

	/* Open a directory for user binaries, BINARY_DIR_PATH */
	dirp = opendir(BINARY_DIR_PATH);
	if (dirp) {
		/* Read each directory entry */
		for (;;) {
			struct dirent *entryp = readdir(dirp);
			if (!entryp) {
				/* Finished with this directory */
				break;
			}
			/* Remove binary file which is not running */
			if (DIRENT_ISFILE(entryp->d_type)) {
				snprintf(filepath, CONFIG_PATH_MAX, "%s/%s", BINARY_DIR_PATH, entryp->d_name);
				ret = binary_manager_read_header(filepath, &header_data, false);
				if (ret < 0) {
					continue;
				}
				/* If binary is not registered, register it */
				(void)binary_manager_register_ubin(header_data.bin_name);
			}
		}
		closedir(dirp);
	} else if (errno != ENOENT) {
		bmdbg("Failed to open a directory, %s\n", BINARY_DIR_PATH);
	}
}

/****************************************************************************
 * Name: binary_manager_create_entry
 *
 * Description:
 *	 This function creates new empty file for requested binary name.
 *
 ****************************************************************************/
int binary_manager_create_entry(int requester_pid, char *bin_name, int version)
{
	int fd;
	int ret;
	int bin_idx;
	binmgr_kinfo_t *kerinfo;
	char q_name[BIN_PRIVMQ_LEN];
	char filepath[CONFIG_PATH_MAX];
	binmgr_createbin_response_t response_msg;

	if (requester_pid < 0 || bin_name == NULL || version < 0) {
		bmdbg("Invalid data : pid %d name %s version %d\n", requester_pid, bin_name, version);
		response_msg.result = BINMGR_INVALID_PARAM;
		goto send_result;
	}

	/* If it is kernel, Return the devname of inacive kernel partition */
	if (!strncmp("kernel", bin_name, BIN_NAME_MAX)) {
		kerinfo = binary_manager_get_kdata();
		if (kerinfo->part_count > 1) {
			response_msg.result = BINMGR_OK;
			snprintf(response_msg.binpath, CONFIG_PATH_MAX, BINMGR_DEVNAME_FMT, kerinfo->part_info[kerinfo->inuse_idx ^ 1].part_num, CONFIG_PATH_MAX);
		} else {
			response_msg.result = BINMGR_NOT_FOUND;
		}
		goto send_result;
	}

	/* Else, Create a new file for user binary and Return filepath */
	bin_idx = binary_manager_get_index_with_name(bin_name);
	if (bin_idx >= 0) {
		/* Check version */
		if (BIN_VER(bin_idx) == version) {
			bmvdbg("Already existing version %d\n", version);
			response_msg.result = BINMGR_ALREADY_UPDATED;
			goto send_result;
		}
		/* Remove old binary files to get space in fs */
		ret = binary_manager_clear_binfile(bin_idx);
		if (ret < 0) {
			response_msg.result = ret;
			goto send_result;
		}
	} else {
		/* If it it not registered, register it */
		ret = binary_manager_register_ubin(bin_name);
		if (ret < 0) {
			response_msg.result = BINMGR_OPERATION_FAIL;
			goto send_result;
		}
	}
	response_msg.result = BINMGR_OPERATION_FAIL;

	/* Create a new file */
	snprintf(filepath, CONFIG_PATH_MAX, "%s/%s_%d", BINARY_DIR_PATH, bin_name, version);
	fd = open(filepath, O_RDWR | O_CREAT, 0666);
	if (fd > 0) {
		bmvdbg("Created file '%s' for binary %s\n", filepath, bin_name);
		strncpy(response_msg.binpath, filepath, CONFIG_PATH_MAX);
		response_msg.result = BINMGR_OK;
		close(fd);
	} else if (errno == ENOENT) {
		/* A directory for binary is not existing, then Create it */
		ret = mkdir(BINARY_DIR_PATH, 0777);
		if (ret == OK) {
			fd = open(filepath, O_RDWR | O_CREAT, 0666);
			if (fd > 0) {
				bmvdbg("Created file '%s' for binary %s\n", filepath, bin_name);
				strncpy(response_msg.binpath, filepath, CONFIG_PATH_MAX);
				response_msg.result = BINMGR_OK;
				close(fd);
			}
		}
	}
send_result:
	snprintf(q_name, BIN_PRIVMQ_LEN, "%s%d", BINMGR_RESPONSE_MQ_PREFIX, requester_pid);
	binary_manager_send_response(q_name, &response_msg, sizeof(binmgr_createbin_response_t));

	return response_msg.result;
}
