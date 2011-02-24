/* -*-pgsql-c-*- */
/*
 * $Header: /cvsroot/pgpool/pgpool/main.c,v 1.22 2007/08/10 03:57:44 y-asaba Exp $
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
 */
#include "pool.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <netdb.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <sys/stat.h>
#include <fcntl.h>

#include <sys/wait.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "version.h"

#include "pqc.h"

#define CHECK_REQUEST \
	do \
	{ \
		if (failover_request) \
		{ \
			failover(failover_signo); \
			failover_request = 0; \
		} \
		if (sigchld_request) \
		{ \
			reaper(); \
		} \
	} while (0)

#define PGPOOLMAXLITSENQUEUELENGTH 10000
static void daemonize(void);
static int read_pid_file(void);
static void write_pid_file(void);
static pid_t fork_a_child(int unix_fd, int inet_fd);
static int create_unix_domain_socket(void);
static int create_inet_domain_socket(const char *hostname);
static void myexit(int code);
static void failover(int sig);
static void reaper(void);
static int pool_pause(struct timeval *timeout);
static void pool_sleep(unsigned int second);

static RETSIGTYPE exit_handler(int sig);
static RETSIGTYPE reap_handler(int sig);
static RETSIGTYPE failover_handler(int sig);
static RETSIGTYPE health_check_timer_handler(int sig);

static void usage(void);
static void stop_me(void);
static void switch_me(void);

static struct sockaddr_un un_addr;		/* unix domain socket path */

static pid_t *pids;	/* child pid table */

static int unix_fd;	/* unix domain socket fd */
static int inet_fd;	/* inet domain socket fd */

static int exiting = 0;		/* non 0 if I'm exiting */
static int switching = 0;		/* non 0 if I'm fail overing or degenerating */
static int degenerated = 0;	/* set non 0 if already degerated */

static int not_detach = 0;		/* non 0 if non detach option (-n) is given */
int debug = 0;	/* non 0 if debug option is given (-d) */

static pid_t mypid;	/* pgpool parent process id */

long int weight_master;	/* normalized weight of master (0-RAND_MAX range) */

static int stop_sig = SIGTERM;	/* stopping signal default value */
static int switch_over_sig = SIGUSR1;	/* switch over signal default value */

static int health_check_timer_expired;		/* non 0 if health check timer expired */
static volatile sig_atomic_t failover_request = 0;
static volatile sig_atomic_t sigchld_request = 0;
static int pipe_fds[2]; /* for delivering signals */
static volatile int failover_signo = 0;

int myargc;
char **myargv;

/*
* pgpool main program
*/
int main(int argc, char **argv)
{
	int opt;
	char conf_file[POOLMAXPATHLEN+1];
	char hba_file[POOLMAXPATHLEN+1];
	int i;
	int pid;

	myargc = argc;
	myargv = argv;

	snprintf(conf_file, sizeof(conf_file), "%s/%s", DEFAULT_CONFIGDIR, POOL_CONF_FILE_NAME);
	snprintf(hba_file, sizeof(hba_file), "%s/%s", DEFAULT_CONFIGDIR, HBA_CONF_FILE_NAME);

	while ((opt = getopt(argc, argv, "a:df:hm:ns:")) != -1)
	{
		switch (opt)
		{
			case 'a':    /* specify hba configuration file */
				if (!optarg)
				{
					usage();
					exit(1);
				}
				strncpy(hba_file, optarg, sizeof(hba_file));
				break;

			case 'd':	/* debug option */
				debug = 1;
				break;

			case 'f':	/* specify configuration file */
				if (!optarg)
				{
					usage();
					exit(1);
				}
				strncpy(conf_file, optarg, sizeof(conf_file));
				break;

			case 'h':
				usage();
				exit(0);
				break;

			case 'm':	/* stop mode */
				if (!optarg)
				{
					usage();
					exit(1);
				}
				if (*optarg == 's' || !strcmp("smart", optarg))
					stop_sig = SIGTERM;		/* smart shutdown */
				else if (*optarg == 'f' || !strcmp("fast", optarg))
					stop_sig = SIGINT;		/* fast shutdown */
				else if (*optarg == 'i' || !strcmp("immediate", optarg))
					stop_sig = SIGQUIT;		/* immediate shutdown */
				else
				{
					usage();
					exit(1);
				}
				break;
				
			case 'n':	/* no detaching control ttys */
				not_detach = 1;
				break;

			case 's':	/* switch over request */
				if (!optarg)
				{
					usage();
					exit(1);
				}
				if (*optarg == 'm' || !strcmp("master", optarg))
					switch_over_sig = SIGUSR1;		/* stopping master */
				else if (*optarg == 's' || !strcmp("secondary", optarg))
					switch_over_sig = SIGUSR2;		/* stopping secondary */
				else
				{
					usage();
					exit(1);
				}
				break;

			default:
				usage();
				exit(1);
		}
	}

	if (pool_get_config(conf_file))
	{
		pool_error("Unable to get configuration. Exiting...");
		exit(1);
	}

	/* set current PostgreSQL backend */
	pool_config.current_backend_host_name = pool_config.backend_host_name;
	pool_config.current_backend_port = pool_config.backend_port;

	/* set load balance weight */
	if (pool_config.weight_master <= 0.0)
		weight_master = 0;
	else
		weight_master = (RAND_MAX) * (pool_config.weight_master /
									  (pool_config.weight_master + pool_config.weight_secondary));

	pool_debug("weight: %ld", weight_master);

	/* read pool_hba.conf */
	if (pool_config.enable_pool_hba)
		load_hba(hba_file);

	/*
	 * if a non-switch argument remains, then it should be either "stop" or "switch"
	 */
	if (optind == (argc - 1))
	{
		if (!strcmp(argv[optind], "stop"))
		{
			stop_me();
			exit(0);
		}
		else if (!strcmp(argv[optind], "switch"))
		{
			switch_me();
			exit(0);
		}
		else
		{
			usage();
			exit(1);
		}
	}
	/*
	 * else if no non-switch argument remains, then it should be a start request
	 */
	else if (optind == argc)
	{
		pid = read_pid_file();
		if (pid > 0)
		{
			if (kill(pid, 0) == 0)
			{
				fprintf(stderr, "pid file found. is another pqcd(%d) is running?\n", pid);
				exit(1);
			}
			else
				fprintf(stderr, "pid file found but it seems bogus. Trying to start pqcd anyway...\n");
		}
	}
	/*
	 * otherwise an error...
	 */
	else
	{
		usage();
		exit(1);
	}

	/* set signal masks */
	poolinitmask();

	if (not_detach)
		write_pid_file();
	else
		daemonize();

	mypid = getpid();

	/* set unix domain socket path */
	snprintf(un_addr.sun_path, sizeof(un_addr.sun_path), "%s/.s.PGSQL.%d",
			 pool_config.socket_dir,
			 pool_config.port);

	/* set up signal handlers */
	pool_signal(SIGPIPE, SIG_IGN);

	/* create unix domain socket */
	unix_fd = create_unix_domain_socket();

	/* create inet domain socket if any */
	if (pool_config.listen_addresses[0])
	{
		inet_fd = create_inet_domain_socket(pool_config.listen_addresses);
	}

	pids = malloc(pool_config.num_init_children * sizeof(pid_t));
	if (pids == NULL)
	{
		pool_error("failed to allocate pids");
		myexit(1);
	}
	memset(pids, 0, pool_config.num_init_children * sizeof(pid_t));

	/*
	 * Initialize the query cache
	 */
	if ( !pqc_init(!not_detach) )
	{
		pool_error("failed to initialize the query cache.");
		myexit(1);
	}

	/* fork the children */
	for (i=0;i<pool_config.num_init_children;i++)
	{
		pids[i] = fork_a_child(unix_fd, inet_fd);
	}

	/* set up signal handlers */
	POOL_SETMASK(&BlockSig);
	pool_signal(SIGTERM, exit_handler);
	pool_signal(SIGINT, exit_handler);
	pool_signal(SIGQUIT, exit_handler);
	pool_signal(SIGCHLD, reap_handler);
	pool_signal(SIGUSR1, failover_handler);
	pool_signal(SIGUSR2, failover_handler);
	pool_signal(SIGHUP, exit_handler);

	/* create pipe for delivering event */
	if (pipe(pipe_fds) < 0)
	{
		pool_error("failed to create pipe");
		myexit(1);
	}

	pool_log("pqcd successfully started");

	/*
	 * This is the main loop
	 */
	for (;;)
	{
		CHECK_REQUEST;

		/* do we need health checking for PostgreSQL? */
		if (!degenerated && pool_config.health_check_period > 0)
		{
			int sts;
			unsigned int sleep_time;

			pool_log("starting health checking");

			if (pool_config.health_check_timeout > 0)
			{
				/*
				 * set health checker timeout. we want to detect
				 * commnuication path failure much earlier before
				 * TCP/IP statck detects it.
				 */
				pool_signal(SIGALRM, health_check_timer_handler);
				alarm(pool_config.health_check_timeout);
			}

			/*
			 * do actual health check. trying to connect to the backend
			 */
			errno = 0;
			health_check_timer_expired = 0;
			sts = health_check();

			if (errno != EINTR || (errno == EINTR && health_check_timer_expired))
			{
				if (sts == -1)
				{
					failover(SIGUSR1);		/* master down */
				}
				else if (sts == -2)
				{
					failover(SIGUSR2);		/* secondary down */
				}
			}

			if (pool_config.health_check_timeout > 0)
			{
				/* seems ok. cancel health check timer */
				pool_signal(SIGALRM, SIG_IGN);
			}

			sleep_time = pool_config.health_check_period;
			pool_sleep(sleep_time);
		}
		else
		{
			for (;;)
			{
				int r;

				POOL_SETMASK(&UnBlockSig);
				r = pool_pause(NULL);
				POOL_SETMASK(&BlockSig);
				if (r > 0)
					break;
			}
		}
	}

	return 0;
}

static void usage(void)
{
	fprintf(stderr, "pqcd version %s(%s),\n",	VERSION, PGPOOLVERSION);
	fprintf(stderr, "  a generic connection pool/replication/load balance server for PostgreSQL\n\n");
	fprintf(stderr, "usage: pqcd [-f config_file][-a hba_file][-n][-d]\n");
	fprintf(stderr, "usage: pqcd [-f config_file][-a hba_file] [-m {s[mart]|f[ast]|i[mmediate]}] stop\n");
	fprintf(stderr, "usage: pqcd [-f config_file][-a hba_file] [-s {m[aster]|s[econdary]] switch\n");
	fprintf(stderr, "usage: pqcd -h\n");
	fprintf(stderr, "  config_file default path: %s/%s\n",DEFAULT_CONFIGDIR, POOL_CONF_FILE_NAME);
	fprintf(stderr, "  hba_file default path:    %s/%s\n",DEFAULT_CONFIGDIR, HBA_CONF_FILE_NAME);
	fprintf(stderr, "  -n: don't run in daemon mode. does not detatch control tty\n");
	fprintf(stderr, "  -d: debug mode. lots of debug information will be printed\n");
	fprintf(stderr, "  stop: stop pqcd\n");
	fprintf(stderr, "  switch: send switch over request to pqcd\n");
	fprintf(stderr, "  -h: print this help\n");
}

/*
* detatch control ttys
*/
static void daemonize(void)
{
	int			i;
	pid_t		pid;

	pid = fork();
	if (pid == (pid_t) -1)
	{
		pool_error("fork() failed. reason: %s", strerror(errno));
		exit(1);
		return;					/* not reached */
	}
	else if (pid > 0)
	{			/* parent */
		exit(0);
	}

#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		pool_error("setsid() failed. reason:%s", strerror(errno));
		exit(1);
	}
#endif

	i = open("/dev/null", O_RDWR);
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);
	close(i);

	write_pid_file();
}


/*
* stop myself
*/
static void stop_me(void)
{
	pid_t pid;

	pqc_destroy();

	pid = read_pid_file();
	if (pid < 0)
	{
		pool_error("could not read pid file");
		exit(1);
	}

	if (kill(pid, stop_sig) == -1)
	{
		pool_error("could not stop pid: %d. reason: %s", pid, strerror(errno));
		exit(1);
	}

	fprintf(stderr, "stop request sent to pqcd. waiting for termination...");

	while (kill(pid, 0) == 0)
	{
		fprintf(stderr, ".");
		sleep(1);
	}
	fprintf(stderr, "done.\n");
}

/*
* switch over request
*/
static void switch_me(void)
{
	pid_t pid;

	pid = read_pid_file();

	if (pid < 0)
	{
		pool_error("could read pid file");
		exit(1);
	}

	if (kill(pid, switch_over_sig) == -1)
	{
		pool_error("could not send switch over request to pid: %d. reason: %s", pid, strerror(errno));
		exit(1);
	}

	pool_log("switch over request sent");
}

/*
* read the pid file
*/
static int read_pid_file(void)
{
	FILE *fd;
	char path[POOLMAXPATHLEN];
	char pidbuf[128];

	snprintf(path, sizeof(path), "%s/%s", pool_config.logdir, PID_FILE_NAME);
	fd = fopen(path, "r");
	if (!fd)
	{
		return -1;
	}
	if (fread(pidbuf, 1, sizeof(pidbuf), fd) <= 0)
	{
		pool_error("could not read pid file as %s. reason: %s",
				   path, strerror(errno));
		fclose(fd);
		return -1;
	}
	fclose(fd);
	return(atoi(pidbuf));
}

/*
* write the pid file
*/
static void write_pid_file(void)
{
	FILE *fd;
	char path[POOLMAXPATHLEN];
	char pidbuf[128];

	snprintf(path, sizeof(path), "%s/%s", pool_config.logdir, PID_FILE_NAME);
	fd = fopen(path, "w");
	if (!fd)
	{
		pool_error("could not open pid file as %s. reason: %s",
				   path, strerror(errno));
		exit(1);
	}
	snprintf(pidbuf, sizeof(pidbuf), "%d", (int)getpid());
	fwrite(pidbuf, strlen(pidbuf), 1, fd);
	if (fclose(fd))
	{
		pool_error("could not write pid file as %s. reason: %s",
				   path, strerror(errno));
		exit(1);
	}
}

/*
* fork a child
*/
static pid_t fork_a_child(int unix_fd, int inet_fd)
{
	pid_t pid;

	pid = fork();

	if (pid == 0)
	{
		myargv = save_ps_display_args(myargc, myargv);

		/* call child main */
		POOL_SETMASK(&UnBlockSig);
		do_child(unix_fd, inet_fd);
	}
	else if (pid == -1)
	{
		pool_error("fork() failed. reason: %s", strerror(errno));
		myexit(1);
	}
	return pid;
}

/*
* create inet domain socket
*/
static int create_inet_domain_socket(const char *hostname)
{
	struct sockaddr_in addr;
	int fd;
	int status;
	int one = 1;
	int len;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
	{
		pool_error("Failed to create INET domain socket. reason: %s", strerror(errno));
		myexit(1);
	}
	if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one,
					sizeof(one))) == -1)
	{
		pool_error("setsockopt() failed. reason: %s", strerror(errno));
		myexit(1);
	}

	memset((char *) &addr, 0, sizeof(addr));
	((struct sockaddr *)&addr)->sa_family = AF_INET;

	if (strcmp(hostname, "*")==0)
	{
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	}
	else
	{
		struct hostent *hostinfo;

		hostinfo = gethostbyname(hostname);
		if (!hostinfo)
		{
			pool_error("could not resolve host name \"%s\": %s", hostname, hstrerror(h_errno));
			myexit(1);
		}
		addr.sin_addr = *(struct in_addr *) hostinfo->h_addr;
	}

	addr.sin_port = htons(pool_config.port);
	len = sizeof(struct sockaddr_in);
	status = bind(fd, (struct sockaddr *)&addr, len);
	if (status == -1)
	{
		pool_error("bind() failed. reason: %s", strerror(errno));
		myexit(1);
	}

	status = listen(fd, PGPOOLMAXLITSENQUEUELENGTH);
	if (status < 0)
	{
		pool_error("listen() failed. reason: %s", strerror(errno));
		myexit(1);
	}
	return fd;
}

/*
* create UNIX domain socket
*/
static int create_unix_domain_socket(void)
{
	struct sockaddr_un addr;
	int fd;
	int status;
	int len;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
	{
		pool_error("Failed to create UNIX domain socket. reason: %s", strerror(errno));
		myexit(1);
	}
	memset((char *) &addr, 0, sizeof(addr));
	((struct sockaddr *)&addr)->sa_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), un_addr.sun_path);
	len = sizeof(struct sockaddr_un);
	status = bind(fd, (struct sockaddr *)&addr, len);
	if (status == -1)
	{
		pool_error("bind() failed. reason: %s", strerror(errno));
		myexit(1);
	}

	if (chmod(un_addr.sun_path, 0777) == -1)
	{
		pool_error("chmod() failed. reason: %s", strerror(errno));
		myexit(1);
	}

	status = listen(fd, PGPOOLMAXLITSENQUEUELENGTH);
	if (status < 0)
	{
		pool_error("listen() failed. reason: %s", strerror(errno));
		myexit(1);
	}
	return fd;
}

static void myexit(int code)
{
	char path[POOLMAXPATHLEN];

	if (getpid() != mypid)
		return;

	unlink(un_addr.sun_path);
	snprintf(path, sizeof(path), "%s/%s", pool_config.logdir, PID_FILE_NAME);
	unlink(path);

	exit(code);
}

/* notice backend connection error using SIGUSR1 or SIGUSR2 */
void notice_backend_error(int master)
{
	pid_t parent = getppid();

	pool_log("notice_backend_error: master: %d fail over request from pid %d", master, getpid());

	if (master)
		kill(parent, SIGUSR1);
	else
		kill(parent, SIGUSR2);

	/* avoid race conditon with SIGCHLD */
#ifdef NOT_USED
	sleep(1);
#endif
}

static RETSIGTYPE exit_handler(int sig)
{
	int i;

	POOL_SETMASK(&AuthBlockSig);

	/*
	 * this could happend in a child process if a signal has been sent
	 * before resetting signal handler
	 */
	if (getpid() != mypid)
	{
		pool_debug("exit_handler: I am not parent");
		POOL_SETMASK(&UnBlockSig);
		exit(0);
	}

	if (sig == SIGTERM)
		pool_log("received smart shutdown request");
	else if (sig == SIGINT)
		pool_log("received fast shutdown request");
	else if (sig == SIGQUIT)
		pool_log("received immediate shutdown request");
	else if (sig == SIGHUP)
		pool_log("received idle connection close request");
	else
	{
		pool_error("exit_handler: unknown signal received %d", sig);
		POOL_SETMASK(&UnBlockSig);
		return;
	}

	exiting = 1;

	for (i = 0; i < pool_config.num_init_children; i++)
	{
		pid_t pid = pids[i];
		if (pid)
		{
			kill(pid, sig);
		}
	}

	if (sig == SIGHUP)
	{
		exiting = 0;
		POOL_SETMASK(&UnBlockSig);
		return;
	}

	POOL_SETMASK(&UnBlockSig);

	while (wait(NULL) > 0)
		;

	if (errno != ECHILD)
		pool_error("wait() failed. reason:%s", strerror(errno));

	/*
	 * Destroy the query cache
	 */
	if ( !pqc_destroy() )
	{
		pool_error("failed to finish the query cache.");
		myexit(1);
	}

	myexit(0);
}

/*
 * handle SIGUSR1/SIGUSR2 (backend connection error, fail over request, if possible)
 *
 * if sig == SIGUSR1, we assume that the master has been down.
 * if sig == SIGUSR2, we assume that the secondary has been down.
 */
static RETSIGTYPE failover_handler(int sig)
{
	POOL_SETMASK(&BlockSig);
	failover_request = 1;
	failover_signo = sig;
	write(pipe_fds[1], "\0", 1);
	POOL_SETMASK(&UnBlockSig);
}

/*
 * Process failover request
 * failover() must be called under protecting signals.
 */
static void failover(int sig)
{
	int i;
	int replication = 0;

	pool_debug("failover_handler called");

	/*
	 * this could happen in a child process if a signal has been sent
	 * before resetting signal handler
	 */
	if (getpid() != mypid)
	{
		pool_debug("failover_handler: I am not parent");
		return;
	}

	/*
	 * processing SIGTERM, SIGINT or SIGQUIT
	 */
	if (exiting)
	{
		return;
	}

	/*
	 * processing fail over or switch over
	 */
	if (switching)
	{
		return;
	}

#ifdef NOT_USED
	/* secondary backend exists? */
	if (pool_config.secondary_backend_port == 0)
		return;
#endif

	/* 
	 * if not in replication mode/master slave mode, we treat this a restart request.
	 * otherwise we need to check if we have already failovered.
	 */
	if (
		strcmp(pool_config.current_backend_host_name, pool_config.secondary_backend_host_name) ||
		pool_config.current_backend_port != pool_config.secondary_backend_port)
	{
		switching = 1;
		
		if (!degenerated && pool_config.secondary_backend_port != 0)
		{
			pool_log("starting failover from %s(%d) to %s(%d)",
					   pool_config.current_backend_host_name,
					   pool_config.current_backend_port,
					   pool_config.secondary_backend_host_name,
					   pool_config.secondary_backend_port);
			pool_config.server_status[0] = 2;		/* mark this down */
		}
		else
		{
			pool_log("restarting pqcd");
		}

		/* kill all children */
		for (i = 0; i < pool_config.num_init_children; i++)
		{
			pid_t pid = pids[i];
			if (pid)
			{
				kill(pid, SIGQUIT);
				pool_debug("kill %d", pid);
			}
		}

		while (wait(NULL) > 0)
			;

		if (errno != ECHILD)
			pool_error("wait() failed. reason:%s", strerror(errno));

		if (!degenerated && pool_config.secondary_backend_port != 0)
		{
			/* fail over to secondary */
			pool_config.current_backend_host_name = pool_config.secondary_backend_host_name;
			pool_config.current_backend_port = pool_config.secondary_backend_port;
		}

		/* fork the children */
		for (i=0;i<pool_config.num_init_children;i++)
		{
			pids[i] = fork_a_child(unix_fd, inet_fd);
		}

		/*
		 * do not close unix_fd and inet_fd here. if a child dies we
		 * need to fork a new child which should inherit these fds.
		 */

		if (replication)
		{
			if (sig == SIGUSR2)
			{
				pool_log("degeneration done. shutdown secondary host %s(%d)",
						 pool_config.secondary_backend_host_name,
						 pool_config.secondary_backend_port);
			}
			else
			{
				pool_log("degeneration done. shutdown master host %s(%d)",
						 pool_config.backend_host_name,
						 pool_config.backend_port);
			}
		}
		else if (!degenerated && pool_config.secondary_backend_port != 0)
		{
			pool_log("failover from %s(%d) to %s(%d) done.",
					   pool_config.backend_host_name,
					   pool_config.backend_port,
					   pool_config.secondary_backend_host_name,
					   pool_config.secondary_backend_port);
		}
		else
		{
			pool_log("restarting pqcd done.");
		}

		switching = 0;
	}
}

/*
 * health check timer handler
 */
static RETSIGTYPE health_check_timer_handler(int sig)
{
	POOL_SETMASK(&BlockSig);
	health_check_timer_expired = 1;
	POOL_SETMASK(&UnBlockSig);
}

/*
 * handle SIGCHLD
 */
static RETSIGTYPE reap_handler(int sig)
{
	POOL_SETMASK(&BlockSig);
	sigchld_request = 1;
	write(pipe_fds[1], "\0", 1);
	POOL_SETMASK(&UnBlockSig);
}

/*
 * Attach zombie processes and restart child processes.
 * reaper() must be called under protecting signals.
 */
static void reaper(void)
{
	pid_t pid;
	int status;
	int i;

	pool_debug("reap_handler called");
	sigchld_request = 0;

	if (exiting)
	{
		return;
	}

	if (switching)
	{
		return;
	}

#ifdef HAVE_WAITPID
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
#else
		while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
		{
#endif

			pool_debug("child %d exits with status %d by signal %d", pid, status, WTERMSIG(status));

			/* look for exiting child's pid */
			for (i=0;i<pool_config.num_init_children;i++)
			{
				if (pid == pids[i])
				{
					/* if found, fork a new child */
					if (!switching && !exiting && status)
					{
						pids[i] = fork_a_child(unix_fd, inet_fd);
						pool_debug("fork a new child pid %d", pids[i]);
						break;
					}
				}
			}
		}

	}

/*
 * pool_pause: A process pauses by select().
 */
static int pool_pause(struct timeval *timeout)
{
	fd_set rfds;
	int n;
	char dummy;

	FD_ZERO(&rfds);
	FD_SET(pipe_fds[0], &rfds);
	n = select(pipe_fds[0]+1, &rfds, NULL, NULL, timeout);
	if (n == 1)
		read(pipe_fds[0], &dummy, 1);
	return n;
}

/*
 * pool_pause: A process sleep using pool_pause().
 *             If a signal event occurs, it raises signal handler.
 */
static void pool_sleep(unsigned int second)
{
	struct timeval current_time, sleep_time;

	gettimeofday(&current_time, NULL);
	sleep_time.tv_sec = second + current_time.tv_sec;
	sleep_time.tv_usec = current_time.tv_usec;

	POOL_SETMASK(&UnBlockSig);
	while (sleep_time.tv_sec > current_time.tv_sec ||
		   sleep_time.tv_usec > current_time.tv_usec)
	{
		struct timeval timeout;
		int r;

		timeout.tv_sec = sleep_time.tv_sec - current_time.tv_sec;
		timeout.tv_usec = sleep_time.tv_usec - current_time.tv_usec;
		if (timeout.tv_usec < 0)
		{
			timeout.tv_sec--;
			timeout.tv_usec += 1000000;
		}

		r = pool_pause(&timeout);
		POOL_SETMASK(&BlockSig);
		if (r > 0)									
			CHECK_REQUEST;
		POOL_SETMASK(&UnBlockSig);
		gettimeofday(&current_time, NULL);
	}
	POOL_SETMASK(&BlockSig);
}
