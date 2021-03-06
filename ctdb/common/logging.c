/*
   Logging utilities

   Copyright (C) Andrew Tridgell  2008
   Copyright (C) Martin Schwenke  2014
   Copyright (C) Amitay Isaacs  2015

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/network.h"
#include "system/locale.h"
#include "system/time.h"
#include "system/filesys.h"
#include "system/syslog.h"

#include "lib/util/time_basic.h"
#include "lib/util/sys_rw.h"
#include "lib/util/debug.h"
#include "lib/util/blocking.h"
#include "lib/util/samba_util.h" /* get_myname() */

#include "common/logging.h"

struct {
	int log_level;
	const char *log_string;
} log_string_map[] = {
	{ DEBUG_ERR,     "ERROR" },
	{ DEBUG_WARNING, "WARNING" },
	{ 2,             "WARNING" },
	{ DEBUG_NOTICE,  "NOTICE" },
	{ 4,             "NOTICE" },
	{ DEBUG_INFO,    "INFO" },
	{ 6,             "INFO" },
	{ 7,             "INFO" },
	{ 8,             "INFO" },
	{ 9,             "INFO" },
	{ DEBUG_DEBUG,   "DEBUG" },
};

bool debug_level_parse(const char *log_string, int *log_level)
{
	int i;

	if (log_string == NULL) {
		return false;
	}

	if (isdigit(log_string[0])) {
		int level = atoi(log_string);

		if (level >= 0 && level < ARRAY_SIZE(log_string_map)) {
			*log_level = level;
			return true;
		}
		return false;
	}

	for (i=0; i<ARRAY_SIZE(log_string_map); i++) {
		if (strncasecmp(log_string_map[i].log_string,
				log_string, strlen(log_string)) == 0) {
			*log_level = log_string_map[i].log_level;
			return true;
		}
	}

	return false;
}

const char *debug_level_to_string(int log_level)
{
	int i;

	for (i=0; ARRAY_SIZE(log_string_map); i++) {
		if (log_string_map[i].log_level == log_level) {
			return log_string_map[i].log_string;
		}
	}
	return "UNKNOWN";
}

int debug_level_from_string(const char *log_string)
{
	bool found;
	int log_level;

	found = debug_level_parse(log_string, &log_level);
	if (found) {
		return log_level;
	}

	/* Default debug level */
	return DEBUG_ERR;
}

/*
 * file logging backend
 */

struct file_log_state {
	const char *app_name;
	int fd;
	char buffer[1024];
};

static void file_log(void *private_data, int level, const char *msg)
{
	struct file_log_state *state = talloc_get_type_abort(
		private_data, struct file_log_state);
	struct timeval tv;
	struct timeval_buf tvbuf;
	int ret;

	if (state->fd == STDERR_FILENO) {
		ret = snprintf(state->buffer, sizeof(state->buffer),
			       "%s[%u]: %s\n",
			       state->app_name, (unsigned)getpid(), msg);
	} else {
		GetTimeOfDay(&tv);
		timeval_str_buf(&tv, false, true, &tvbuf);

		ret = snprintf(state->buffer, sizeof(state->buffer),
			       "%s %s[%u]: %s\n", tvbuf.buf,
			       state->app_name, (unsigned)getpid(), msg);
	}
	if (ret < 0) {
		return;
	}

	state->buffer[sizeof(state->buffer)-1] = '\0';

	sys_write_v(state->fd, state->buffer, strlen(state->buffer));
}

static int file_log_state_destructor(struct file_log_state *state)
{
	if (state->fd != -1 && state->fd != STDERR_FILENO) {
		close(state->fd);
		state->fd = -1;
	}
	return 0;
}

static int file_log_setup(TALLOC_CTX *mem_ctx, const char *option,
			  const char *app_name)
{
	struct file_log_state *state;

	state = talloc_zero(mem_ctx, struct file_log_state);
	if (state == NULL) {
		return ENOMEM;
	}

	state->app_name = app_name;

	if (option == NULL || strcmp(option, "-") == 0) {
		int ret;

		state->fd = STDERR_FILENO;
		ret = dup2(STDERR_FILENO, STDOUT_FILENO);
		if (ret == -1) {
			int save_errno = errno;
			talloc_free(state);
			return save_errno;
		}

	} else {
		state->fd = open(option, O_WRONLY|O_APPEND|O_CREAT, 0644);
		if (state->fd == -1) {
			int save_errno = errno;
			talloc_free(state);
			return save_errno;
		}

		if (! set_close_on_exec(state->fd)) {
			int save_errno = errno;
			talloc_free(state);
			return save_errno;
		}
	}

	talloc_set_destructor(state, file_log_state_destructor);
	debug_set_callback(state, file_log);

	return 0;
}

/*
 * syslog logging backend
 */

/* Copied from lib/util/debug.c */
static int debug_level_to_priority(int level)
{
        /*
         * map debug levels to syslog() priorities
         */
        static const int priority_map[] = {
                LOG_ERR,     /* 0 */
                LOG_WARNING, /* 1 */
                LOG_NOTICE,  /* 2 */
                LOG_NOTICE,  /* 3 */
                LOG_NOTICE,  /* 4 */
                LOG_NOTICE,  /* 5 */
                LOG_INFO,    /* 6 */
                LOG_INFO,    /* 7 */
                LOG_INFO,    /* 8 */
                LOG_INFO,    /* 9 */
        };
        int priority;

        if( level >= ARRAY_SIZE(priority_map) || level < 0)
                priority = LOG_DEBUG;
        else
                priority = priority_map[level];

        return priority;
}

struct syslog_log_state {
	int fd;
	const char *app_name;
	const char *hostname;
	int (*format)(int dbglevel, struct syslog_log_state *state,
		      const char *str, char *buf, int bsize);
	/* RFC3164 says: The total length of the packet MUST be 1024
	   bytes or less. */
	char buffer[1024];
};

/* Format messages as per RFC3164
 *
 * It appears that some syslog daemon implementations do not allow a
 * hostname when messages are sent via a Unix domain socket, so omit
 * it.  Similarly, syslogd on FreeBSD does not understand the hostname
 * part of the header, even when logging via UDP.  Note that most
 * implementations will log messages against "localhost" when logging
 * via UDP.  A timestamp could be sent but rsyslogd on Linux limits
 * the timestamp logged to the precision that was received on
 * /dev/log.  It seems sane to send degenerate RFC3164 messages
 * without a header at all, so that the daemon will generate high
 * resolution timestamps if configured.
 */
static int format_rfc3164(int dbglevel, struct syslog_log_state *state,
			  const char *str, char *buf, int bsize)
{
	int pri;
	int len;

	pri = LOG_DAEMON | debug_level_to_priority(dbglevel);
	len = snprintf(buf, bsize, "<%d>%s[%u]: %s",
		       pri, state->app_name, getpid(), str);
	buf[bsize-1] = '\0';
	len = MIN(len, bsize - 1);

	return len;
}

/* Format messages as per RFC5424
 *
 * <165>1 2003-08-24T05:14:15.000003-07:00 192.0.2.1
 *         myproc 8710 - - %% It's time to make the do-nuts.
 */
static int format_rfc5424(int dbglevel, struct syslog_log_state *state,
			  const char *str, char *buf, int bsize)
{
	int pri;
	struct timeval tv;
	struct timeval_buf tvbuf;
	int len, s;

	/* Header */
	pri = LOG_DAEMON | debug_level_to_priority(dbglevel);
	GetTimeOfDay(&tv);
	len = snprintf(buf, bsize,
		       "<%d>1 %s %s %s %u - - ",
		       pri, timeval_str_buf(&tv, true, true, &tvbuf),
		       state->hostname, state->app_name, getpid());
	/* A truncated header is not useful... */
	if (len >= bsize) {
		return -1;
	}

	/* Message */
	s = snprintf(&buf[len], bsize - len, "%s", str);
	buf[bsize-1] = '\0';
	len = MIN(len + s, bsize - 1);

	return len;
}

static void syslog_log(void *private_data, int level, const char *msg)
{
	syslog(debug_level_to_priority(level), "%s", msg);
}

static void syslog_log_sock(void *private_data, int level, const char *msg)
{
	struct syslog_log_state *state = talloc_get_type_abort(
		private_data, struct syslog_log_state);
	int n;

	n = state->format(level, state, msg, state->buffer,
			  sizeof(state->buffer));
	if (n == -1) {
		return;
	}

	sys_write_v(state->fd, state->buffer, n);
}

static int syslog_log_setup_syslog(TALLOC_CTX *mem_ctx, const char *app_name)
{
	openlog(app_name, LOG_PID, LOG_DAEMON);

	debug_set_callback(NULL, syslog_log);

	return 0;
}

static int syslog_log_state_destructor(struct syslog_log_state *state)
{
	if (state->fd != -1) {
		close(state->fd);
		state->fd = -1;
	}
	return 0;
}

static int syslog_log_setup_common(TALLOC_CTX *mem_ctx, const char *app_name,
				   struct syslog_log_state **result)
{
	struct syslog_log_state *state;

	state = talloc_zero(mem_ctx, struct syslog_log_state);
	if (state == NULL) {
		return ENOMEM;
	}

	state->fd = -1;
	state->app_name = app_name;
	talloc_set_destructor(state, syslog_log_state_destructor);

	return 0;
}

#ifdef _PATH_LOG
static int syslog_log_setup_nonblocking(TALLOC_CTX *mem_ctx,
					const char *app_name)
{
	struct syslog_log_state *state = NULL;
	struct sockaddr_un dest;
	int ret;

	ret = syslog_log_setup_common(mem_ctx, app_name, &state);
	if (ret != 0) {
		return ret;
	}

	state->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (state->fd == -1) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	dest.sun_family = AF_UNIX;
	strncpy(dest.sun_path, _PATH_LOG, sizeof(dest.sun_path)-1);
	ret = connect(state->fd,
		      (struct sockaddr *)&dest, sizeof(dest));
	if (ret == -1) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	ret = set_blocking(state->fd, false);
	if (ret != 0) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	if (! set_close_on_exec(state->fd)) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	state->hostname = NULL; /* Make this explicit */
	state->format = format_rfc3164;

	debug_set_callback(state, syslog_log_sock);

	return 0;
}
#endif /* _PATH_LOG */

static int syslog_log_setup_udp(TALLOC_CTX *mem_ctx, const char *app_name,
				bool rfc5424)
{
	struct syslog_log_state *state = NULL;
	struct sockaddr_in dest;
	int ret;

	ret = syslog_log_setup_common(mem_ctx, app_name, &state);
	if (ret != 0) {
		return ret;
	}

	state->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (state->fd == -1) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	dest.sin_family = AF_INET;
	dest.sin_port   = htons(514);
	dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ret = connect(state->fd,
		      (struct sockaddr *)&dest, sizeof(dest));
	if (ret == -1) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	if (! set_close_on_exec(state->fd)) {
		int save_errno = errno;
		talloc_free(state);
		return save_errno;
	}

	state->hostname = get_myname(state);
	if (state->hostname == NULL) {
		/* Use a fallback instead of failing initialisation */
		state->hostname = "localhost";
	}
	if (rfc5424) {
		state->format = format_rfc5424;
	} else {
		state->format = format_rfc3164;
	}

	debug_set_callback(state, syslog_log_sock);

	return 0;
}

static int syslog_log_setup(TALLOC_CTX *mem_ctx, const char *option,
			    const char *app_name)
{
	if (option == NULL) {
		return syslog_log_setup_syslog(mem_ctx, app_name);
#ifdef _PATH_LOG
	} else if (strcmp(option, "nonblocking") == 0) {
		return syslog_log_setup_nonblocking(mem_ctx, app_name);
#endif
	} else if (strcmp(option, "udp") == 0) {
		return syslog_log_setup_udp(mem_ctx, app_name, false);
	} else if (strcmp(option, "udp-rfc5424") == 0) {
		return syslog_log_setup_udp(mem_ctx, app_name, true);
	}

	return EINVAL;
}

/* Initialise logging */
int logging_init(TALLOC_CTX *mem_ctx, const char *logging,
		 const char *debug_level, const char *app_name)
{
	struct {
		const char *name;
		int (*setup)(TALLOC_CTX *mem_ctx, const char *option,
			     const char *app_name);
	} log_backend[] = {
		{
			.name = "file",
			.setup = file_log_setup,
		},
		{
			.name = "syslog",
			.setup = syslog_log_setup,
		},
	};
	int (*setup)(TALLOC_CTX *, const char *, const char *) = NULL;
	char *str, *name, *option;
	int ret, i;

	setup_logging(app_name, DEBUG_STDERR);

	if (debug_level == NULL) {
		debug_level = getenv("CTDB_DEBUGLEVEL");
	}
	if (! debug_level_parse(debug_level, &DEBUGLEVEL)) {
		return EINVAL;
	}

	if (logging == NULL) {
		logging = getenv("CTDB_LOGGING");
	}
	if (logging == NULL || logging[0] == '\0') {
		return EINVAL;
	}

	str = talloc_strdup(mem_ctx, logging);
	if (str == NULL) {
		return ENOMEM;
	}

	name = strtok(str, ":");
	if (name == NULL) {
		talloc_free(str);
		return EINVAL;
	}
	option = strtok(NULL, ":");
	/*
	 * option can be NULL here, both setup()
	 * backends handle this.
	 */

	for (i=0; i<ARRAY_SIZE(log_backend); i++) {
		if (strcmp(log_backend[i].name, name) == 0) {
			setup = log_backend[i].setup;
		}
	}

	if (setup == NULL) {
		talloc_free(str);
		fprintf(stderr, "Invalid logging option \'%s\'\n", logging);
		return EINVAL;
	}

	ret = setup(mem_ctx, option, app_name);
	talloc_free(str);
	return ret;
}
