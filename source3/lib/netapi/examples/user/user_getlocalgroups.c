/*
 *  Unix SMB/CIFS implementation.
 *  NetUserGetLocalGroups query
 *  Copyright (C) Guenther Deschner 2008
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netapi.h>

#include "common.h"

int main(int argc, const char **argv)
{
	NET_API_STATUS status;
	struct libnetapi_ctx *ctx = NULL;
	const char *hostname = NULL;
	const char *username = NULL;
	uint32_t level = 0;
	uint8_t *buffer = NULL;
	uint32_t entries_read = 0;
	uint32_t total_entries = 0;
	uint32_t flags = 0;
	int i;

	struct LOCALGROUP_USERS_INFO_0 *info0 = NULL;

	poptContext pc;
	int opt;

	struct poptOption long_options[] = {
		POPT_AUTOHELP
		POPT_COMMON_LIBNETAPI_EXAMPLES
		POPT_TABLEEND
	};

	status = libnetapi_init(&ctx);
	if (status != 0) {
		return status;
	}

	pc = poptGetContext("user_getlocalgroups", argc, argv, long_options, 0);

	poptSetOtherOptionHelp(pc, "hostname username");
	while((opt = poptGetNextOpt(pc)) != -1) {
	}

	if (!poptPeekArg(pc)) {
		poptPrintHelp(pc, stderr, 0);
		goto out;
	}
	hostname = poptGetArg(pc);

	if (!poptPeekArg(pc)) {
		poptPrintHelp(pc, stderr, 0);
		goto out;
	}
	username = poptGetArg(pc);

	/* NetUserGetLocalGroups */

	do {
		status = NetUserGetLocalGroups(hostname,
					       username,
					       level,
					       flags,
					       &buffer,
					       (uint32_t)-1,
					       &entries_read,
					       &total_entries);
		if (status == 0 || status == ERROR_MORE_DATA) {

			switch (level) {
				case 0:
					info0 = (struct LOCALGROUP_USERS_INFO_0 *)buffer;
					break;
				default:
					break;
			}

			for (i=0; i<entries_read; i++) {
				switch (level) {
					case 0:
						printf("#%d group: %s\n", i, info0->lgrui0_name);
						info0++;
						break;
					default:
						break;
				}
			}
			NetApiBufferFree(buffer);
		}
	} while (status == ERROR_MORE_DATA);

	if (status != 0) {
		printf("NetUserGetLocalGroups failed with: %s\n",
			libnetapi_get_error_string(ctx, status));
	}

 out:
	libnetapi_free(ctx);
	poptFreeContext(pc);

	return status;
}
