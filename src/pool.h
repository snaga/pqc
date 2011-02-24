/* -*-pgsql-c-*- */
/*
 *
 * $Header: /cvsroot/pgpool/pgpool/pool.h,v 1.21 2007/07/26 08:45:42 y-asaba Exp $
 *
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2007	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * pool.h.: master definition header file
 *
 */

#ifndef POOL_H
#define POOL_H

#include "config.h"
#include "pool_signal.h"
#include "pool_type.h"
#include "pool_list.h"

#include <stdio.h>
#include <time.h>
#include <netinet/in.h>

/* undef this if you have problems with non blocking accept() */
#define NONE_BLOCK

#define POOLMAXPATHLEN 8192

/* configuration file name */
#define POOL_CONF_FILE_NAME "pqcd.conf"
#define HBA_CONF_FILE_NAME "pqcd_hba.conf"

/* pid file directory */
#define DEFAULT_LOGDIR "/tmp"

/* Unix domain socket directory */
#define DEFAULT_SOCKET_DIR "/tmp"

/* memcached executable */
#define DEFAULT_MEMCACHED_BIN "/usr/local/bin/memcached"

/* pid file name */
#define PID_FILE_NAME "pqcd.pid"

/* strict mode comment in SQL */
#define STRICT_MODE_STR "/*STRICT*/"
#define STRICT_MODE(s) (strncasecmp((s), STRICT_MODE_STR, strlen(STRICT_MODE_STR)) == 0)
#define NO_STRICT_MODE_STR "/*NO STRICT*/"
#define NO_STRICT_MODE(s) (strncasecmp((s), NO_STRICT_MODE_STR, strlen(NO_STRICT_MODE_STR)) == 0)

typedef enum {
	POOL_CONTINUE = 0,
	POOL_IDLE,
	POOL_END,
	POOL_ERROR,
	POOL_FATAL,
	POOL_DEADLOCK
} POOL_STATUS;

/* protocol major version numbers */
#define PROTO_MAJOR_V2	2
#define PROTO_MAJOR_V3	3

/*
 * startup packet definitions (v2) stolen from PostgreSQL
 */
#define SM_DATABASE		64
#define SM_USER			32
#define SM_OPTIONS		64
#define SM_UNUSED		64
#define SM_TTY			64

typedef struct StartupPacket_v2
{
	int			protoVersion;		/* Protocol version */
	char		database[SM_DATABASE];	/* Database name */
	char		user[SM_USER];	/* User name */
	char		options[SM_OPTIONS];	/* Optional additional args */
	char		unused[SM_UNUSED];		/* Unused */
	char		tty[SM_TTY];	/* Tty for debug output */
} StartupPacket_v2;

/* startup packet info */
typedef struct
{
	char *startup_packet;		/* raw startup packet without packet length (malloced area) */
	int len;					/* raw startup packet length */
	int major;	/* protocol major version */
	int minor;	/* protocol minor version */
	char *database;	/* database name in startup_packet (malloced area) */
	char *user;	/* user name in startup_packet (malloced area) */
} StartupPacket;

typedef struct CancelPacket
{
	int			protoVersion;		/* Protocol version */
	int			pid;	/* bcckend process id */
	int			key;	/* cancel key */
} CancelPacket;

#define MAX_CONNECTION_SLOTS 2

/*
 * configuration paramters
 */
typedef struct {
	char *listen_addresses; /* hostnames/IP addresses to listen on */
    int	port;	/* port # to bind */
	char *socket_dir;		/* pgpool socket directory */
    char	*backend_host_name;	/* backend host name */
    int	backend_port;	/* backend port # */
    char	*secondary_backend_host_name;	/* secondary backend host name */
    int	secondary_backend_port;	/* secondary backend port # */
    int	num_init_children;	/* # of children initially pre-forked */
    int	child_life_time;	/* if idle for this seconds, child exits */
    int	connection_life_time;	/* if idle for this seconds, connection closes */
    int	child_max_connections;	/* if max_connections received, child exits */
    int	max_pool;	/* max # of connection pool per child */
    char *logdir;		/* logging directory */
    char *backend_socket_dir;	/* Unix domain socket directory for the PostgreSQL server */
	double weight_master;		/* master weight for load balancing */
	double weight_secondary;		/* secondary weight for load balancing */
	/*
	 * if secondary does not respond in this milli seconds, abort this session.
	 */
	int replication_timeout;

	int load_balance_mode;		/* load balance mode */

	int replication_stop_on_mismatch;		/* if there's a data mismatch between master and secondary
											 * start degenration to stop replication mode
											 */
	int replicate_select; /* if non 0, replicate SELECT statement when load balancing is disabled. */
	char **reset_query_list;		/* comma separated list of quries to be issued at the end of session */

	int print_timestamp;		/* if non 0, print time stamp to each log line */
	int master_slave_mode;		/* if non 0, operate in master/slave mode */
	int connection_cache;		/* if non 0, cache connection pool */
	int health_check_timeout;	/* health check timeout */
	int health_check_period;	/* health check period */
	char *health_check_user;		/* PostgreSQL user name for health check */
	int insert_lock;	/* if non 0, automatically lock table with INSERT to keep SERIAL
						   data consistency */
	int ignore_leading_white_space;		/* ignore leading white spaces of each query */
	/* followings do not exist in the configuration file */
    char *current_backend_host_name;	/* current backend host name */
    int	current_backend_port;	/* current backend port # */
	int num_reset_queries;		/* number of queries in reset_query_list */
	int num_servers;			/* number of PostgreSQL servers */
	int server_status[MAX_CONNECTION_SLOTS];	/* server status 0:unused, 1:up, 2:down */
	int log_statement; /* 0:false, 1: true - logs all SQL statements */
	int log_connections;		/* 0:false, 1:true - logs incoming connections */
	int log_hostname;		/* 0:false, 1:true - resolve hostname */
	int enable_pool_hba;		/* 0:false, 1:true - enables pool_hba.conf file authentication */
	char *memcached_bin;		/* a path name to the memcached executable. */
	int query_cache_mode;		/* 1:Active-cache mode 0:Passive-cache mode */
	int query_cache_expiration;	/* timeout value (in seconds) to expire a query cache */
} POOL_CONFIG;

#define MAX_PASSWORD_SIZE		1024

typedef struct {
	int num;	/* number of entries */
	char **names;		/* parameter names */
	char **values;		/* values */
} ParamStatus;

/*
 * stream connection structure
 */
typedef struct {
	int fd;		/* fd for connection */

	char *wbuf;	/* write buffer for the connection */
	int wbufsz;	/* write buffer size */
	int wbufpo;	/* buffer offset */

	char *hp;	/* pending data buffer head address */
	int po;		/* pending data offset */
	int bufsz;	/* pending data buffer size */
	int len;	/* pending data length */

	char *sbuf;	/* buffer for pool_read_string */
	int sbufsz;	/* its size in bytes */

	char *buf2;	/* buffer for pool_read2 */
	int bufsz2;	/* its size in bytes */

	int isbackend;		/* this connection is for backend if non 0 */
	int issecondary_backend;		/* this connection is for secondary backend if non 0 */

	char tstate;		/* transaction state (V3 only) */

	/*
	 * following are used to remember when re-use the authenticated connection
	 */
	int auth_kind;		/* 3: clear text password, 4: crypt password, 5: md5 password */
	int pwd_size;		/* password (sent back from frontend) size in host order */
	char password[MAX_PASSWORD_SIZE];		/* password (sent back from frontend) */
	char salt[4];		/* password salt */

	/*
	 * following are used to remember current session paramter status.
	 * re-used connection will need them (V3 only)
	 */
	ParamStatus params;

	int no_forward;		/* if non 0, do not write to frontend */

	/*
	 * frontend info needed for hba
	 */
	int protoVersion;
	SockAddr raddr;
	UserAuth auth_method;
	char *auth_arg;
	char *database;
	char *username;
#ifdef USE_SSL
	bool ssl;
#endif

} POOL_CONNECTION;

/*
 * connection pool structure
 */
typedef struct {
	StartupPacket *sp;	/* startup packet info */
    int pid;	/* backend pid */
    int key;	/* cancel key */
    POOL_CONNECTION	*con;
	time_t closetime;	/* absolute time in second when the connection closed
						 * if 0, that means the connection is under use.
						 */
} POOL_CONNECTION_POOL_SLOT;

typedef struct {
    int num;	/* number of slots */
    POOL_CONNECTION_POOL_SLOT	*slots[MAX_CONNECTION_SLOTS];
} POOL_CONNECTION_POOL;

#define MASTER_CONNECTION(p) ((p)->slots[0])
#define SECONDARY_CONNECTION(p) ((p)->slots[1])
#define DUAL_MODE (0)
#define MASTER(p) MASTER_CONNECTION(p)->con
#define SECONDARY(p) SECONDARY_CONNECTION(p)->con
#define MAJOR(p) MASTER_CONNECTION(p)->sp->major
#define TSTATE(p) MASTER(p)->tstate

#define Max(x, y)		((x) > (y) ? (x) : (y))
#define Min(x, y)		((x) < (y) ? (x) : (y))

#define LOCK_COMMENT "/*INSERT LOCK*/"
#define LOCK_COMMENT_SZ (sizeof(LOCK_COMMENT)-1)
#define NO_LOCK_COMMENT "/*NO INSERT LOCK*/"
#define NO_LOCK_COMMENT_SZ (sizeof(NO_LOCK_COMMENT)-1)

/*
 * global variables
 */
extern POOL_CONFIG pool_config;	/* configuration values */
extern POOL_CONNECTION_POOL *pool_connection_pool;	/* connection pool */
extern volatile sig_atomic_t backend_timer_expired;
extern long int weight_master;	/* normalized weight of master (0-RAND_MAX range) */
extern char remote_ps_data[];		/* used for set_ps_display */

/*
 * public functions
 */
extern void pool_error(const char *fmt,...);
extern void pool_debug(const char *fmt,...);
extern void pool_log(const char *fmt,...);
extern int pool_get_config(char *confpath);
extern void do_child(int unix_fd, int inet_fd);
extern int pool_init_cp(void);
extern POOL_STATUS pool_process_query(POOL_CONNECTION *frontend,
									  POOL_CONNECTION_POOL *backend,
									  int connection_reuse,
									  int first_ready_for_query_received);

extern POOL_CONNECTION *pool_open(int fd);
extern void pool_close(POOL_CONNECTION *cp);
extern int pool_read(POOL_CONNECTION *cp, void *buf, int len);
extern char *pool_read2(POOL_CONNECTION *cp, int len);
extern int pool_write(POOL_CONNECTION *cp, void *buf, int len);
extern int pool_flush(POOL_CONNECTION *cp);
extern int pool_flush_it(POOL_CONNECTION *cp);
extern int pool_write_and_flush(POOL_CONNECTION *cp, void *buf, int len);
extern char *pool_read_string(POOL_CONNECTION *cp, int *len, int line);
extern int pool_unread(POOL_CONNECTION *cp, void *data, int len);

extern int pool_do_auth(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern int pool_do_reauth(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *cp);

extern int pool_init_cp(void);
extern POOL_CONNECTION_POOL *pool_create_cp(void);
extern POOL_CONNECTION_POOL *pool_get_cp(char *user, char *database, int protoMajor, int check_socket);
extern void pool_discard_cp(char *user, char *database, int protoMajor);
extern void pool_backend_timer(void);

extern POOL_STATUS ErrorResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend);
extern POOL_STATUS ErrorResponse2(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend);

extern POOL_STATUS NoticeResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend);

extern void notice_backend_error(int master);

extern void pool_connection_pool_timer(POOL_CONNECTION_POOL *backend);
extern RETSIGTYPE pool_backend_timer_handler(int sig);

extern int connect_inet_domain_socket(int secondary_backend);
extern int connect_unix_domain_socket(int secondary_backend);

extern int pool_check_fd(POOL_CONNECTION *cp, int notimeout);
extern void pool_enable_timeout(void);
extern void pool_disable_timeout(void);

extern void pool_send_frontend_exits(POOL_CONNECTION_POOL *backend);

extern int pool_read_message_length(POOL_CONNECTION_POOL *cp);
extern int *pool_read_message_length2(POOL_CONNECTION_POOL *cp);
extern signed char pool_read_kind(POOL_CONNECTION_POOL *cp);
extern signed char pool_read_kind2(POOL_CONNECTION_POOL *cp);

extern POOL_STATUS SimpleForwardToFrontend(char kind, POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern POOL_STATUS SimpleForwardToBackend(char kind, POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern POOL_STATUS ParameterStatus(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);

extern int pool_init_params(ParamStatus *params);
extern void pool_discard_params(ParamStatus *params);
extern char *pool_find_name(ParamStatus *params, char *name, int *pos);
extern int pool_get_param(ParamStatus *params, int index, char **name, char **value);
extern int pool_add_param(ParamStatus *params, char *name, char *value);
extern void pool_param_debug_print(ParamStatus *params);

extern void pool_send_error_message(POOL_CONNECTION *frontend, int protoMajor,
							 char *code,
							 char *message,
							 char *detail,
							 char *hint,
							 char *file,
							 int line);

extern void pool_free_startup_packet(StartupPacket *sp);
extern int health_check(void);

extern void init_prepared_list(void);

extern size_t strlcpy(char *dst, const char *src, size_t siz);

extern bool update_process_title;

extern char **save_ps_display_args(int argc, char **argv);

extern void init_ps_display(const char *username, const char *dbname,
							const char *host_info, const char *initial_str);

extern void set_ps_display(const char *activity, bool force);

extern const char *get_ps_display(int *displen);

/* pool_hba.c */
extern void load_hba(char *hbapath);
extern void ClientAuthentication(POOL_CONNECTION *frontend);

/* pool_ip.c */
extern void pool_getnameinfo_all(SockAddr *saddr, char *remote_host, char *remote_port);

#endif /* POOL_H */
