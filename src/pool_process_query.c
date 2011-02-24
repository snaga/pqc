/* -*-pgsql-c-*- */
/*
 * $Header: /cvsroot/pgpool/pgpool/pool_process_query.c,v 1.60 2007/08/29 05:44:50 y-asaba Exp $
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
 * pool_process_query.c: query processing stuff
 *
*/
#include "config.h"
#include <errno.h>

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <ctype.h>

#include "pool.h"

#include "pqc.h"

#define INIT_STATEMENT_LIST_SIZE 8

#define DEADLOCK_ERROR_CODE "40P01"
#define POOL_ERROR_QUERY "send invalid query from pqcd to abort transaction"

typedef struct {
	char *statement_name;
	char *portal_name;
	char *prepared_string;
	char *bind_parameter_string;
} PreparedStatement;

/*
 * prepared statement list
 */
typedef struct {
	int size;
	int cnt;
	PreparedStatement **stmt_list;
} PreparedStatementList;

static POOL_STATUS NotificationResponse(POOL_CONNECTION *frontend, 
										POOL_CONNECTION_POOL *backend);

static POOL_STATUS Query(POOL_CONNECTION *frontend, 
						 POOL_CONNECTION_POOL *backend, char *query);

static POOL_STATUS Execute(POOL_CONNECTION *frontend, 
						   POOL_CONNECTION_POOL *backend);

static POOL_STATUS Parse(POOL_CONNECTION *frontend, 
						 POOL_CONNECTION_POOL *backend);

#ifdef NOT_USED
static POOL_STATUS Sync(POOL_CONNECTION *frontend, 
						   POOL_CONNECTION_POOL *backend);
#endif

static POOL_STATUS ReadyForQuery(POOL_CONNECTION *frontend, 
								 POOL_CONNECTION_POOL *backend, int send_ready);

static POOL_STATUS CompleteCommandResponse(POOL_CONNECTION *frontend, 
										   POOL_CONNECTION_POOL *backend);

static POOL_STATUS CopyInResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend);

static POOL_STATUS CopyOutResponse(POOL_CONNECTION *frontend, 
								   POOL_CONNECTION_POOL *backend);

static POOL_STATUS CopyDataRows(POOL_CONNECTION *frontend,
								POOL_CONNECTION_POOL *backend, int copyin);

static POOL_STATUS CursorResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend);

static POOL_STATUS EmptyQueryResponse(POOL_CONNECTION *frontend,
									  POOL_CONNECTION_POOL *backend);

static int RowDescription(POOL_CONNECTION *frontend, 
						  POOL_CONNECTION_POOL *backend);

static POOL_STATUS AsciiRow(POOL_CONNECTION *frontend, 
							POOL_CONNECTION_POOL *backend,
							short num_fields);

static POOL_STATUS BinaryRow(POOL_CONNECTION *frontend, 
							 POOL_CONNECTION_POOL *backend,
							 short num_fields);

static POOL_STATUS FunctionCall(POOL_CONNECTION *frontend, 
								POOL_CONNECTION_POOL *backend);

static POOL_STATUS FunctionResultResponse(POOL_CONNECTION *frontend, 
										  POOL_CONNECTION_POOL *backend);

static POOL_STATUS ProcessFrontendResponse(POOL_CONNECTION *frontend, 
										   POOL_CONNECTION_POOL *backend);

static POOL_STATUS send_extended_protocol_message(POOL_CONNECTION *cp,
												  char *kind, int len,
												  char *string);
static POOL_STATUS send_execute_message(POOL_CONNECTION *cp,
										int len, char *string);
static int synchronize(POOL_CONNECTION *cp);
static void process_reporting(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
static int reset_backend(POOL_CONNECTION_POOL *backend, int qcnt);

static int is_select_query(char *sql);
static int is_sequence_query(char *sql);
static int load_balance_enabled(POOL_CONNECTION_POOL *backend, char *sql);
static void start_load_balance(POOL_CONNECTION_POOL *backend);
static void end_load_balance(POOL_CONNECTION_POOL *backend);
static POOL_STATUS do_command(POOL_CONNECTION *backend, char *query, int protoMajor, int no_ready_for_query);
static POOL_STATUS do_error_command(POOL_CONNECTION *backend, int protoMajor);
static int need_insert_lock(POOL_CONNECTION_POOL *backend, char *query);
static POOL_STATUS insert_lock(POOL_CONNECTION_POOL *backend, char *query);
static char *get_insert_command_table_name(char *query);
static char *get_execute_command_portal_name(char *query);
static PreparedStatement *get_prepared_command_portal_and_statement(char *query);
static char *skip_comment(char *query);

static void add_prepared_list(PreparedStatementList *p, PreparedStatement *stmt);
static void add_unnamed_portal(PreparedStatementList *p, PreparedStatement *stmt);
static void del_prepared_list(PreparedStatementList *p, PreparedStatement *stmt);
static void reset_prepared_list(PreparedStatementList *p);
static PreparedStatement *lookup_prepared_statement_by_statement(PreparedStatementList *p, const char *name);
static PreparedStatement *lookup_prepared_statement_by_portal(PreparedStatementList *p, const char *name);
static int send_deallocate(POOL_CONNECTION_POOL *backend, PreparedStatementList *p, int n);
static char *normalize_prepared_stmt_name(const char *name);
static POOL_STATUS error_kind_mismatch(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend, int kind, int kind1);

static POOL_CONNECTION_POOL_SLOT *slots[MAX_CONNECTION_SLOTS];

static int in_load_balance;		/* non 0 if in load balance mode */
static int internal_transaction_started;		/* to issue table lock command a transaction
												   has been started internally */
static int select_in_transaction = 0; /* non 0 if select query is in transaction */
static void (*pending_function)(PreparedStatementList *p, PreparedStatement *statement) = NULL;
static PreparedStatement *pending_prepared_stmt = NULL;

static PreparedStatementList prepared_list; /* prepared statement name list */
static PreparedStatement *unnamed_statement = NULL;
static PreparedStatement *unnamed_portal = NULL;
static int prepare_in_session = 0;

static int is_drop_database(char *query);		/* returns non 0 if this is a DROP DATABASE command */
static void query_ps_status(char *query, POOL_CONNECTION_POOL *backend);		/* show ps status */
static int detect_deadlock_error(POOL_CONNECTION *master, int major);

POOL_STATUS pool_process_query(POOL_CONNECTION *frontend, 
							   POOL_CONNECTION_POOL *backend,
							   int connection_reuse,
							   int first_ready_for_query_received)
{
	char kind, kind1;	/* packet kind (backend) */
	char fkind;	/* packet kind (frontend) */
	short num_fields = 0;
	fd_set	readmask;
	fd_set	writemask;
	fd_set	exceptmask;
	int fds;
	POOL_STATUS status;
	int state;	/* 0: ok to issue commands 1: waiting for "ready for query" response */
	int qcnt;

	frontend->no_forward = connection_reuse;
	qcnt = 0;
	status = POOL_END;
	state = 0;

	pqc_buf_init();

	for (;;)
	{
		kind = kind1 = 0;
		fkind = 0;

		/* reset connections before entering query processing. */
		if (state == 0 && connection_reuse)
		{
			int st;

			/* send query for resetting connection such as "ROLLBACK" "RESET ALL"... */
			st = reset_backend(backend, qcnt);

			if (st < 0)		/* error? */
			{
				/* probably we don't need this, since caller will
				 * close the connection to frontend after returning with POOL_END. But I
				 * guess I would like to be a paranoid...
				 */
				frontend->no_forward = 0;
				return POOL_END;
			}

			else if (st == 0)	/* no query issued? */
			{
				qcnt++;
				continue;
			}

			else if (st == 1)	/* more query remains */
			{
				state = 1;
				qcnt++;
				continue;
			}

			else	/* no more query(st == 2) */
			{
				frontend->no_forward = 0;
				prepare_in_session = 0;
				return POOL_CONTINUE;
			}

		}

		/* no data left for reading all of nodes (master,slave,front). */
		if ((!DUAL_MODE && MASTER(backend)->len == 0 && frontend->len == 0) ||
			(DUAL_MODE && MASTER(backend)->len == 0 &&
			SECONDARY(backend)->len == 0
			 && frontend->len == 0))
		{

			struct timeval timeout;

			timeout.tv_sec = 1;
			timeout.tv_usec = 0;

			FD_ZERO(&readmask);
			FD_ZERO(&writemask);
			FD_ZERO(&exceptmask);
			if (!connection_reuse)
				FD_SET(frontend->fd, &readmask);
			FD_SET(MASTER(backend)->fd, &readmask);
			if (DUAL_MODE)
				FD_SET(SECONDARY(backend)->fd, &readmask);
			if (!connection_reuse)
				FD_SET(frontend->fd, &exceptmask);
			FD_SET(MASTER(backend)->fd, &exceptmask);

			if (connection_reuse)
			{
				if (DUAL_MODE)
					fds = select(Max(SECONDARY(backend)->fd, MASTER(backend)->fd) + 1,
								 &readmask, &writemask, &exceptmask, NULL);
				else
					fds = select(MASTER(backend)->fd+1, &readmask, &writemask, &exceptmask, NULL);
			}
			else
			{
				if (DUAL_MODE)
					fds = select(Max(SECONDARY(backend)->fd,
									 Max(frontend->fd, MASTER(backend)->fd)) + 1,
								 &readmask, &writemask, &exceptmask, NULL);
				else
					fds = select(Max(frontend->fd, MASTER(backend)->fd)+1,
								 &readmask, &writemask, &exceptmask, NULL);
			}

			if (fds == -1)
			{
				if (errno == EINTR)
					continue;

				pool_error("select() failed. reason: %s", strerror(errno));
				return POOL_ERROR;
			}

			if (fds == 0)
			{
				return POOL_CONTINUE;
			}

			/* if available, read ONLY a response command from the master at first. */
			if (FD_ISSET(MASTER(backend)->fd, &readmask))
			{
				pool_read(MASTER(backend), &kind, 1);
				pool_debug("read kind from backend %c", kind);
			}

			if (DUAL_MODE && FD_ISSET(SECONDARY(backend)->fd, &readmask))
			{
				pool_read(SECONDARY(backend), &kind1, 1);
				pool_debug("read kind from secondary backend %c", kind1);
			}

			if (!connection_reuse && FD_ISSET(frontend->fd, &exceptmask))
			{
				return POOL_END;
			}
			if (FD_ISSET(MASTER(backend)->fd, &exceptmask))
			{
				return POOL_ERROR;
			}

			/* if need to read from the frontend, go ahead. */
			if (!connection_reuse && FD_ISSET(frontend->fd, &readmask))
			{
				/*
				 * Set the flag back to `query_cache_mode', which is specified
				 * in pqcd.conf (or default value).
				 */
				IsQueryCacheEnabled = pool_config.query_cache_mode;

				/*
				 * Read a command from the frontend, check type of the command,
				 * forward it in an appropriate way.
				 */
				status = ProcessFrontendResponse(frontend, backend);
				if (status != POOL_CONTINUE)
					return status;

				if (kind != 0 || kind1 != 0)
				{
					pool_debug("kind(%02x) or kind1(%02x) != 0", kind, kind1);
				}
				else
				{
					continue;
				}
			}
		}
		else
		{
			/* if something left to read, read it as a command at first. */
			if (MASTER(backend)->len > 0)
			{
				pool_read(MASTER(backend), &kind, 1);
				pool_debug("read kind from backend pending data %c len: %d po: %d", kind, MASTER(backend)->len, MASTER(backend)->po);
			}
			if (frontend->len > 0)
			{
				status = ProcessFrontendResponse(frontend, backend);
				if (status != POOL_CONTINUE)
					return status;

				if (kind != 0 || kind1 != 0)
				{
					pool_debug("cached kind(%02x) or kind1(%02x) != 0", kind, kind1);
				}
				else
				{
					continue;
				}
			}
		}

		/* this is the synchronous point */
		if (first_ready_for_query_received)
		{
			if (kind == 0)
			{
				pool_read(MASTER(backend), &kind, 1);
			}
			if (kind1 == 0)
			{
				if (SECONDARY(backend)->len <= 0)
				{
					/* at this point the query should have completed and it's safe to set timeout here */
					pool_debug("pool_process_query: waiting for secondary for data ready");

					/* temporary enable timeout */
					pool_enable_timeout();

					if (pool_check_fd(SECONDARY(backend), 0))
					{
						pool_error("pool_process_query: secondary data is not ready at synchronous point. abort this session");

					}
					else
					{
						pool_read(SECONDARY(backend), &kind1, 1);
					}

					pool_disable_timeout();
				}
				else
				{
						pool_read(SECONDARY(backend), &kind1, 1);
				}
			}

			first_ready_for_query_received = 0;

			if (kind == '\0' || kind != kind1)
			{
				return error_kind_mismatch(frontend, backend, kind, kind1);
			}
		}

		/*
		 * FIXME: if a query hit the cache, return the result to frontend
		 *        from the cache.
		 */
		/*
		{
			char *ptr;
			int len;

			pqc_get_cache( pqc_pop_current_query(), &ptr, &len );
			pool_write(frontend, ptr, len);

			continue;
		}
		*/

		/*
		 * Prrocess backend Response
		 */

		if (kind == 0)
		{
			pool_error("kind is 0!");
			return POOL_ERROR;
		}

		pool_debug("pool_process_query: kind from backend: %c", kind);

		pool_debug("pool_process_query: database name = %s", frontend->database);

		/*
		 * Check a command type, and process result in an appropriate way.
		 */
		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			switch (kind)
			{
				case 'G':
					/* CopyIn response */
					status = CopyInResponse(frontend, backend);
					break;
				case 'S':
					/* Paramter Status */
					status = ParameterStatus(frontend, backend);
					break;
				case 'Z':
					/*
					 * FIXME: Stop buffering a resultset, put it into the cache,
					 *        and clear a result buffer.
					 *
					 * FIXME: do something here.
					 */

					/*
					 * Store the result messages into the query cache
					 * if the query result was retreived from the backend.
					 *
					 * No need to update the query cache when it was fetched
					 * from the query cache itself.
					 */
					if (IsQueryCacheEnabled && !FoundInQueryCache)
					{
						char *current_query;
						current_query = pqc_pop_current_query();

						if ( current_query )
							pqc_set_cache(frontend, current_query, pqc_buf_get(), pqc_buf_len());
					}


					/* Ready for query */
					status = ReadyForQuery(frontend, backend, 1);

					/* FIXME: load the default again. */
					pool_debug("Query cache enabled back (by default) after the ReadyForQuery response.");
					IsQueryCacheEnabled = UseQueryCache = true;
					FoundInQueryCache = 0;

					pqc_buf_init();

					pool_debug("---------------------- Ready For Query -----------------------");

					break;
				default:
					if (IsQueryCacheEnabled && UseQueryCache && FoundInQueryCache)
					{
						pool_debug("pool_process_query: found in cache. skip reading from the backend.");
						break;
					}

					status = SimpleForwardToFrontend(kind, frontend, backend);

					pool_debug("pool_process_query: len=%d", pqc_buf_len());

					break;
			}
		}
		else
		{
			switch (kind)
			{
				case 'A':
					/* Notification  response */
					status = NotificationResponse(frontend, backend);
					break;

				case 'B':
					/* BinaryRow */
					status = BinaryRow(frontend, backend, num_fields);
					break;

				case 'C':
					/* Complete command response */
					status = CompleteCommandResponse(frontend, backend);
					break;

				case 'D':
					/* AsciiRow */
					status = AsciiRow(frontend, backend, num_fields);
					break;

				case 'E':
					/* Error Response */
					status = ErrorResponse(frontend, backend);
					break;

				case 'G':
					/* CopyIn Response */
					status = CopyInResponse(frontend, backend);
					break;

				case 'H':
					/* CopyOut Response */
					status = CopyOutResponse(frontend, backend);
					break;

				case 'I':
					/* Empty Query Response */
					status = EmptyQueryResponse(frontend, backend);
					break;

				case 'N':
					/* Notice Response */
					status = NoticeResponse(frontend, backend);
					break;

				case 'P':
					/* CursorResponse */
					status = CursorResponse(frontend, backend);
					break;

				case 'T':
					/* RowDescription */
					status = RowDescription(frontend, backend);
					if (status < 0)
						return POOL_ERROR;

					num_fields = status;
					status = POOL_CONTINUE;
					break;

				case 'V':
					/* FunctionResultResponse and FunctionVoidResponse */
					status = FunctionResultResponse(frontend, backend);
					break;
				
				case 'Z':
					/* Ready for query */
					status = ReadyForQuery(frontend, backend, 1);
					break;
				
				default:
					pool_error("Unknown message type %c(%02x)", kind, kind);
					exit(1);
			}
		}

		if (status != POOL_CONTINUE)
			return status;

		if (kind == 'Z' && frontend->no_forward && state == 1)
		{
			state = 0;
		}

	}
	return POOL_CONTINUE;
}

static POOL_STATUS Query(POOL_CONNECTION *frontend, 
						 POOL_CONNECTION_POOL *backend, char *query)
{
	char *string, *string1;
	int len;
	static char *sq = "show pool_status";
	POOL_STATUS status;
	int deadlock_detected = 0;
	int hint_offset = 0;

	/*
	 * Read a query from the frontend.
	 */
	if (query == NULL)	/* need to read query from frontend? */
	{
		/* read actual query */
		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			if (pool_read(frontend, &len, sizeof(len)) < 0)
				return POOL_END;
			len = ntohl(len) - 4;
			string = pool_read2(frontend, len);
		}
		else
			string = pool_read_string(frontend, &len, 0);

		if (string == NULL)
			return POOL_END;
	}
	else
	{
		len = strlen(query)+1;
		string = query;
	}

	/* show ps status */
	query_ps_status(string, backend);

	/* log query to log file if neccessary */
	if (pool_config.log_statement)
	{
		pool_log("statement: %s", string);
	}
	else
	{
		pool_debug("statement: %s", string);
	}

	/*
	 * if this is DROP DATABASE command, send HUP signal to parent and
	 * ask it to close all idle connections.
	 * XXX This is overkill. It would be better to close the idle
	 * connection for the database which DROP DATABASE command tries
	 * to drop. This is impossible at this point, since we have no way
	 * to pass such info to other processes.
	 */
	if (is_drop_database(string))
	{
		int stime = 5;	/* XXX give arbitary time to allow closing idle connections */

		pool_debug("Query: sending HUP signal to parent");

		kill(getppid(), SIGHUP);		/* send HUP signal to parent */

		/* we need to loop over here since we will get HUP signal while sleeping */
		while (stime > 0)
		{
			stime = sleep(stime);
		}
	}

	/* process status reporting? */
	if (strncasecmp(sq, string, strlen(sq)) == 0)
	{
		StartupPacket *sp;
		char psbuf[1024];

		pool_debug("process reporting");
		process_reporting(frontend, backend);

		/* show ps status */
		sp = MASTER_CONNECTION(backend)->sp;
		snprintf(psbuf, sizeof(psbuf), "%s %s %s idle",
				 sp->user, sp->database, remote_ps_data);
		set_ps_display(psbuf, false);
		return POOL_CONTINUE;
	}

	/*
	 * Need to check some hint comments, like `cache:refresh',
	 * beginning with the statement, and toggle the cache flags.
	 */
	pqc_check_cache_hint(string, &IsQueryCacheEnabled, &UseQueryCache, &hint_offset);

	if (frontend &&
		(strncasecmp("prepare", string, 7) == 0))
	{
		PreparedStatement *stmt;

		stmt = get_prepared_command_portal_and_statement(string);

		if (stmt != NULL)
		{
			pending_function = add_prepared_list;
			pending_prepared_stmt = stmt;
			prepare_in_session = 1;
		}
	}
	else if (frontend &&
			 strncasecmp("deallocate", string, 10) == 0)
	{
		char *query = string;
		char *buf, *name;
		query = skip_comment(query);

		/* skip "prepare" or "deallocate" */
		while (*query && !isspace(*query))
			query++;

		/* skip spaces */
		while (*query && isspace(*query))
			query++;

		buf = strdup(query);
		name = strtok(buf, "\t\r\n (;");

		pending_function = del_prepared_list;
		pending_prepared_stmt = malloc(sizeof(PreparedStatement));
		if (pending_prepared_stmt == NULL)
		{
			pool_error("SimpleForwardToBackend: malloc failed: %s", strerror(errno));
			return POOL_END;
		}

		pending_prepared_stmt->statement_name = normalize_prepared_stmt_name(name);
		pending_prepared_stmt->portal_name = NULL;
		pending_prepared_stmt->bind_parameter_string = NULL;
		if (pending_prepared_stmt->statement_name == NULL)
		{
			pool_error("SimpleForwardToBackend: strdup failed: %s", strerror(errno));
			return POOL_END;
		}
		free(buf);
	}

	if (frontend &&
		(strncasecmp("execute", string, 7) == 0))
	{
		char *portal_name = get_execute_command_portal_name(string);
		PreparedStatement *stmt;

		if (portal_name != NULL)
		{
			stmt = lookup_prepared_statement_by_statement(&prepared_list,
														  portal_name);
		
			if (!stmt)
				string1 = string;
			else
				string1 = stmt->prepared_string;
		}
		else
		{
			string1 = string;
		}
	}
	else
	{
		string1 = string;
	}

	/* load balance trick */
	if (load_balance_enabled(backend, string1))
		start_load_balance(backend);

	/*
	 * FIXME: Check cache availability for the query here,
	 *        and skip to forward the query to the backend
	 *        if it's available.
	 */
	pool_debug("query cache key = <%s>", string + hint_offset);

	pqc_push_current_query(string + hint_offset);

	FoundInQueryCache = pqc_check_cache_avail(frontend, string + hint_offset);
	pool_debug("Query: IsQueryCacheEnabled = %d, UseQueryCache = %d, FoundInQueryCache = %d",
			   IsQueryCacheEnabled, UseQueryCache, FoundInQueryCache);

	/*
	 * If the response message (memory block) is found in the query cache,
	 * fetch it, and split it into each (multiple) messages to send them back.
	 */
	if (IsQueryCacheEnabled && UseQueryCache && FoundInQueryCache)
	{
		pool_debug("Query: a query result found in the query cache, %s", string);
		{
			char qcache[PQC_MAX_VALUE];
			size_t qcachelen;

			if ( !pqc_get_cache(frontend,  string + hint_offset, (char **)&(qcache[0]), &qcachelen) )
			{
				pool_debug("Query: pqc_get_cache() failed. %s", string + hint_offset);
				return POOL_END;
			}

			pool_debug("Query: a cache fetched. len=%d", qcachelen);
			
			/*
			 * FIXME: This should not be reached, but now it sometimes happens.
			 */
			if (qcachelen == 0)
				goto do_exec;

			/*
			 * Split a memory block (which comes from the query cache)
			 * into several response messages, and send them back each
			 * to the frontend.
			 */
			pqc_send_cached_messages(frontend, qcache, qcachelen);

			/*
			 * Send a "READY FOR QUERY" response.
			 */
			if (MAJOR(backend) == PROTO_MAJOR_V3)
			{
				signed char state;
				
				/* FIXME: need to fix the state variable. */
				state = 'I';
				
				/* set transaction state */
				MASTER(backend)->tstate = state;
				
				pqc_send_message(frontend, 'Z', 5, (char *)&state);
			}
			else
				pool_write(frontend, "Z", 1);
			
			if (pool_flush(frontend))
				return POOL_END;
		}
		pool_debug("Query: sent a query result to the frontend from the query cache, `%s'", string + hint_offset);
		pool_debug("---------------------- Ready For Query -----------------------");

		return POOL_CONTINUE;
	}
	else
	{
		pool_debug("Query: A query result _NOT_ found in the query cache, `%s'", string);
	}

do_exec:

	/* forward the query to the backend */
	pool_write(MASTER(backend), "Q", 1);

	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		int sendlen = htonl(len + 4 - hint_offset);
		pool_write(MASTER(backend), &sendlen, sizeof(sendlen));
	}

	if (pool_write_and_flush(MASTER(backend), string + hint_offset, len - hint_offset) < 0)
	{
		return POOL_END;
	}

	return POOL_CONTINUE;
}

/*
 * process EXECUTE (V3 only)
 */
static POOL_STATUS Execute(POOL_CONNECTION *frontend, 
						   POOL_CONNECTION_POOL *backend)
{
	char *string;		/* portal name + null terminate + max_tobe_returned_rows */
	int len;
	int sendlen;
	int i;
	char kind;
	int status;
	PreparedStatement *stmt;
	int deadlock_detected = 0;
	int checked = 0;
	char cachekey[PQC_MAX_KEY];
	int hint_offset = 0;

	/* read Execute packet */
	if (pool_read(frontend, &len, sizeof(len)) < 0)
		return POOL_END;

	len = ntohl(len) - 4;
	string = pool_read2(frontend, len);

	pool_debug("Execute: portal name <%s>", string);

	stmt = lookup_prepared_statement_by_portal(&prepared_list, string);

	pool_debug("Execute:");
	pool_debug("  statement_name        <%s>", stmt->statement_name);
	pool_debug("  portal_name           <%s>", stmt->portal_name);
	pool_debug("  prepared_string       <%s>", stmt->prepared_string);
	pool_debug("  bind_parameter_string <%s>", stmt->bind_parameter_string);

	if (stmt)
	{
		/* load balance trick */
		if (stmt && load_balance_enabled(backend, stmt->prepared_string))
			start_load_balance(backend);
	}

	/*
	 * Need to check some hint comments, like `cache:refresh',
	 * beginning with the statement, and toggle the cache flags.
	 */
	pqc_check_cache_hint(stmt->prepared_string, &IsQueryCacheEnabled, &UseQueryCache, &hint_offset);

	/*
	 * FIXME: Check cache availability for the query here,
	 *        and skip to forward the query to the backend
	 *        if it's available.
	 */
	snprintf(cachekey, sizeof(cachekey), "%s%s", stmt->prepared_string + hint_offset, stmt->bind_parameter_string);
	
	pool_debug("query cache key = <%s>", cachekey);

	pqc_push_current_query(cachekey);
	FoundInQueryCache = pqc_check_cache_avail(frontend, cachekey);

	pool_debug("Execute: IsQueryCacheEnabled = %d, UseQueryCache = %d, FoundInQueryCache = %d",
			   IsQueryCacheEnabled, UseQueryCache, FoundInQueryCache);

	/*
	 * If the response message (memory block) is found in the query cache,
	 * fetch it, and split it into each (multiple) messages to send them back.
	 */
	if (IsQueryCacheEnabled && UseQueryCache && FoundInQueryCache)
	{
		pool_debug("Execute: a query result found in the query cache, %s", stmt->prepared_string);
		{
			char qcache[PQC_MAX_VALUE];
			size_t qcachelen;

			if ( !pqc_get_cache(frontend, cachekey, (char **)&(qcache[0]), &qcachelen) )
			{
				pool_debug("Execute: pqc_get_cache() failed. %s", stmt->prepared_string + hint_offset);
				return POOL_END;
			}

			pool_debug("Execute: a cache fetched. len=%d", qcachelen);

			/*
			 * Split a memory block (which comes from the query cache)
			 * into several response messages, and send them back each
			 * to the frontend.
			 */
			pqc_send_cached_messages(frontend, qcache, qcachelen);

			/*
			 * Send a "READY FOR QUERY" response.
			 */
			if (MAJOR(backend) == PROTO_MAJOR_V3)
			{
				signed char state;
				
				/* FIXME: need to fix the state variable. */
				state = 'I';
				
				/* set transaction state */
				MASTER(backend)->tstate = state;
				
				pqc_send_message(frontend, 'Z', 5, (char *)&state);
			}
			else
				pool_write(frontend, "Z", 1);
			
			if (pool_flush(frontend))
				return POOL_END;
		}
		pool_debug("Execute: sent a query result to the frontend from the query cache, `%s'",
				   stmt->prepared_string + hint_offset);

		return POOL_CONTINUE;
	}

	for (i = 0;i < backend->num;i++)
	{
		POOL_CONNECTION *cp = backend->slots[i]->con;

		if (deadlock_detected)
		{
			pool_write(cp, "Q", 1);
			len = strlen(POOL_ERROR_QUERY) + 1;
			sendlen = htonl(len + 4);
			pool_write(cp, &sendlen, sizeof(sendlen));
			pool_write_and_flush(cp, POOL_ERROR_QUERY, len);
		}
		else
		{
			/* forward the query to the backend */
			if (send_execute_message(cp, len, string))
				return POOL_ERROR;
		}

		break;
	}

	while ((kind = pool_read_kind(backend)),
		   (kind != 'C' && kind != 'E' && kind != 'I' && kind != 's'))
	{
		if (kind == -2) /* kind mismatch */
		{
			return error_kind_mismatch(frontend, backend, 0, 0);
		}
		else if (kind < 0)
		{
			pool_error("Execute: pool_read_kind error");
			return POOL_ERROR;
		}

		status = SimpleForwardToFrontend(kind, frontend, backend);
		if (status != POOL_CONTINUE)
			return status;
		pool_flush(frontend);
	}
	status = SimpleForwardToFrontend(kind, frontend, backend);
	if (status != POOL_CONTINUE)
		return status;
	pool_flush(frontend);

	/* end load balance mode */
	if (in_load_balance)
		end_load_balance(backend);

	return POOL_CONTINUE;
}

/*
 * Extended query protocol has to send Flush message.
 */
static POOL_STATUS send_extended_protocol_message(POOL_CONNECTION *cp,
												  char *kind, int len,
												  char *string)
{
	int sendlen;

	/* forward the query to the backend */
	pool_write(cp, kind, 1);
	sendlen = htonl(len + 4);
	pool_write(cp, &sendlen, sizeof(sendlen));
	pool_write(cp, string, len);

	/*
	 * send "Flush" message so that backend notices us
	 * the completion of the command
	 */
	pool_write(cp, "H", 1);
	sendlen = htonl(4);
	if (pool_write_and_flush(cp, &sendlen, sizeof(sendlen)) < 0)
	{
		return POOL_ERROR;
	}

	return POOL_CONTINUE;
}

static POOL_STATUS send_execute_message(POOL_CONNECTION *cp,
										int len, char *string)
{
	return send_extended_protocol_message(cp, "E", len, string);
}

/*
 * process PARSE (V3 only)
 */
static POOL_STATUS Parse(POOL_CONNECTION *frontend, 
						 POOL_CONNECTION_POOL *backend)
{
	char kind;
	int i;
	int len;
	char *string;
	char *name, *stmt;
	int deadlock_detected = 0;
	int checked = 0;

	/* read Parse packet */
	if (pool_read(frontend, &len, sizeof(len)) < 0)
		return POOL_END;

	len = ntohl(len) - 4;
	string = pool_read2(frontend, len);

	pool_debug("Parse: portal name <%s>, len=%d", string, len);

	name = strdup(string);
	if (name == NULL)
	{
		pool_error("Parse: malloc failed: %s", strerror(errno));
		return POOL_END;
	}

	pending_prepared_stmt = malloc(sizeof(PreparedStatement));
	if (pending_prepared_stmt == NULL)
	{
		pool_error("Parse: malloc failed: %s", strerror(errno));
		return POOL_END;
	}
	pending_prepared_stmt->portal_name = NULL;
	pending_prepared_stmt->bind_parameter_string = NULL;

	if (*string)
	{
		pending_function = add_prepared_list;
		pending_prepared_stmt->statement_name = name;
	}
	else
	{
		pending_function = add_unnamed_portal;
		pending_prepared_stmt->statement_name = NULL;
		free(name);
	}

	/* copy prepared statement string */
	stmt = string;
	stmt += strlen(string) + 1;
	pending_prepared_stmt->prepared_string = strdup(stmt);
	if (pending_prepared_stmt->prepared_string == NULL)
	{
		pool_error("SimpleForwardToBackend: strdup failed: %s", strerror(errno));
		return POOL_END;
	}

	pool_debug("Parse: stmt <%s>, len=%d", stmt, strlen(stmt));

	/* forward Parse message to backends */
	for (i = 0; i < backend->num; i++)
	{
		POOL_CONNECTION *cp = backend->slots[i]->con;
		int sendlen;

		if (deadlock_detected)
		{
			pool_write(cp, "Q", 1);
			len = strlen(POOL_ERROR_QUERY) + 1;
			sendlen = htonl(len + 4);
			pool_write(cp, &sendlen, sizeof(sendlen));
			pool_write_and_flush(cp, POOL_ERROR_QUERY, len);
		}
		else if (send_extended_protocol_message(cp, "P", len, string))
			return POOL_END;

		break;
	}

	for (;;)
	{
		kind = pool_read_kind(backend);
		if (kind < 0)
		{
			pool_error("SimpleForwardToBackend: pool_read_kind error");
			return POOL_ERROR;
		}
		SimpleForwardToFrontend(kind, frontend, backend);
		if (pool_flush(frontend) < 0)
			return POOL_ERROR;

		/*
		 * If warning or log messages are received, we must read
		 * one message from backend.
		 */
		if (kind != 'N') /* Notice Message */
			break;
	}

	return POOL_CONTINUE;
}

#ifdef NOT_USED
/*
 * process Sync (V3 only)
 */
static POOL_STATUS Sync(POOL_CONNECTION *frontend, 
						   POOL_CONNECTION_POOL *backend)
{
	char *string;		/* portal name + null terminate + max_tobe_returned_rows */
	int len;
	int sendlen;

	/* read Sync packet */
	if (pool_read(frontend, &len, sizeof(len)) < 0)
		return POOL_END;

	len = ntohl(len) - 4;
	string = pool_read2(frontend, len);

	/* forward the query to the backend */
	pool_write(MASTER(backend), "S", 1);

	sendlen = htonl(len + 4);
	pool_write(MASTER(backend), &sendlen, sizeof(sendlen));
	if (pool_write_and_flush(MASTER(backend), string, len) < 0)
	{
		return POOL_END;
	}

	return POOL_CONTINUE;
}
#endif

static POOL_STATUS ReadyForQuery(POOL_CONNECTION *frontend, 
								 POOL_CONNECTION_POOL *backend, int send_ready)
{
	StartupPacket *sp;
	char psbuf[1024];

	/* if a transaction is started for insert lock, we need to close it. */
	if (internal_transaction_started)
	{
		int i;
		int len;
		signed char state;

		if ((len = pool_read_message_length(backend)) < 0)
			return POOL_END;

		pool_debug("ReadyForQuery: message length: %d", len);

		len = htonl(len);

		state = pool_read_kind(backend);
		if (state < 0)
			return POOL_END;

		/* set transaction state */
		pool_debug("ReadyForQuery: transaction state: %c", state);
		MASTER(backend)->tstate = state;

		for (i = 0;i < backend->num;i++)
		{
			if (do_command(backend->slots[i]->con, "COMMIT", PROTO_MAJOR_V3, 1) != POOL_CONTINUE)
				return POOL_ERROR;
		}
		internal_transaction_started = 0;
	}

	pool_flush(frontend);

	if (send_ready)
	{
		pool_write(frontend, "Z", 1);

		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			int len;
			signed char state;

			if ((len = pool_read_message_length(backend)) < 0)
				return POOL_END;

			pool_debug("ReadyForQuery: message length: %d", len);

			len = htonl(len);
			pool_write(frontend, &len, sizeof(len));

			state = pool_read_kind(backend);
			if (state < 0)
				return POOL_END;

			/* set transaction state */
			pool_debug("ReadyForQuery: transaction state: %c", state);
			MASTER(backend)->tstate = state;

			pool_write(frontend, &state, 1);
		}

		if (pool_flush(frontend))
			return POOL_END;
	}

	/* end load balance mode */
	if (in_load_balance)
		end_load_balance(backend);

#ifdef NOT_USED
	return ProcessFrontendResponse(frontend, backend);
#endif

	sp = MASTER_CONNECTION(backend)->sp;
	if (MASTER(backend)->tstate == 'T')
		snprintf(psbuf, sizeof(psbuf), "%s %s %s idle in transaction", 
				 sp->user, sp->database, remote_ps_data);
	else
		snprintf(psbuf, sizeof(psbuf), "%s %s %s idle", 
				 sp->user, sp->database, remote_ps_data);
	set_ps_display(psbuf, false);

	return POOL_CONTINUE;
}

static POOL_STATUS CompleteCommandResponse(POOL_CONNECTION *frontend, 
										   POOL_CONNECTION_POOL *backend)
{
	char *string, *string1;
	int len, len1;

	/* read command tag */
	string = pool_read_string(MASTER(backend), &len, 0);
	if (string == NULL)
		return POOL_END;

	/* forward to the frontend */
	pool_write(frontend, "C", 1);
	pool_debug("Complete Command Response: string: \"%s\"", string);
	if (pool_write(frontend, string, len) < 0)
	{
		return POOL_END;
	}
	return POOL_CONTINUE;
}

static int RowDescription(POOL_CONNECTION *frontend, 
						  POOL_CONNECTION_POOL *backend)
{
	short num_fields, num_fields1;
	int oid, mod;
	int oid1, mod1;
	short size, size1;
	char *string, *string1;
	int len, len1;
	int i;

	/* # of fields (could be 0) */
	pool_read(MASTER(backend), &num_fields, sizeof(short));

	/* forward it to the frontend */
	pool_write(frontend, "T", 1);
	pool_write(frontend, &num_fields, sizeof(short));

	num_fields = ntohs(num_fields);
	for (i = 0;i<num_fields;i++)
	{
		/* field name */
		string = pool_read_string(MASTER(backend), &len, 0);
		if (string == NULL)
			return POOL_END;

		pool_write(frontend, string, len);

		/* type oid */
		pool_read(MASTER(backend), &oid, sizeof(int));

		pool_debug("RowDescription: type oid: %d", ntohl(oid));

		pool_write(frontend, &oid, sizeof(int));

		/* size */
		pool_read(MASTER(backend), &size, sizeof(short));
		pool_debug("RowDescription: field size: %d", ntohs(size));
		pool_write(frontend, &size, sizeof(short));

		/* modifier */
		pool_read(MASTER(backend), &mod, sizeof(int));

		pool_debug("RowDescription: modifier: %d", ntohs(mod));

		pool_write(frontend, &mod, sizeof(int));
	}

	return num_fields;
}

static POOL_STATUS AsciiRow(POOL_CONNECTION *frontend, 
							POOL_CONNECTION_POOL *backend,
							short num_fields)
{
	static char nullmap[8192], nullmap1[8192];
	int nbytes;
	int i;
	unsigned char mask;
	int size, size1;
	char *buf;
	char msgbuf[1024];

	pool_write(frontend, "D", 1);

	nbytes = (num_fields + 7)/8;

	if (nbytes <= 0)
		return POOL_CONTINUE;

	/* NULL map */
	pool_read(MASTER(backend), nullmap, nbytes);
	if (pool_write(frontend, nullmap, nbytes) < 0)
		return POOL_END;

	mask = 0;

	for (i = 0;i<num_fields;i++)
	{
		if (mask == 0)
			mask = 0x80;

		/* NOT NULL? */
		if (mask & nullmap[i/8])
		{
			/* field size */
			if (pool_read(MASTER(backend), &size, sizeof(int)) < 0)
				return POOL_END;
		}

		buf = NULL;

		if (mask & nullmap[i/8])
		{
			/* forward to frontend */
			pool_write(frontend, &size, sizeof(int));
			size = ntohl(size) - 4;

			/* read and send actual data only when size > 0 */
			if (size > 0)
			{
				buf = pool_read2(MASTER(backend), size);
				if (buf == NULL)
					return POOL_END;
			}
		}

		if (buf)
		{
			pool_write(frontend, buf, size);
			snprintf(msgbuf, Min(sizeof(msgbuf), size+1), "%s", buf);
			pool_debug("AsciiRow: len: %d data: %s", size, msgbuf);
		}

		mask >>= 1;
	}

	return POOL_CONTINUE;
}

static POOL_STATUS BinaryRow(POOL_CONNECTION *frontend, 
							 POOL_CONNECTION_POOL *backend,
							 short num_fields)
{
	static char nullmap[8192], nullmap1[8192];
	int nbytes;
	int i;
	unsigned char mask;
	int size, size1;
	char *buf;

	pool_write(frontend, "B", 1);

	nbytes = (num_fields + 7)/8;

	if (nbytes <= 0)
		return POOL_CONTINUE;

	/* NULL map */
	pool_read(MASTER(backend), nullmap, nbytes);
	if (pool_write(frontend, nullmap, nbytes) < 0)
		return POOL_END;

	mask = 0;

	for (i = 0;i<num_fields;i++)
	{
		if (mask == 0)
			mask = 0x80;

		/* NOT NULL? */
		if (mask & nullmap[i/8])
		{
			/* field size */
			if (pool_read(MASTER(backend), &size, sizeof(int)) < 0)
				return POOL_END;
		}

		buf = NULL;

		if (mask & nullmap[i/8])
		{
			/* forward to frontend */
			pool_write(frontend, &size, sizeof(int));
			size = ntohl(size) - 4;

			/* read and send actual data only when size > 0 */
			if (size > 0)
			{
				buf = pool_read2(MASTER(backend), size);
				if (buf == NULL)
					return POOL_END;
			}
		}

		if (buf)
			pool_write(frontend, buf, size);

		mask >>= 1;
	}
	return POOL_CONTINUE;
}

static POOL_STATUS CursorResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend)
{
	char *string, *string1;
	int len, len1;

	/* read cursor name */
	string = pool_read_string(MASTER(backend), &len, 0);
	if (string == NULL)
		return POOL_END;

	/* forward to the frontend */
	pool_write(frontend, "P", 1);
	if (pool_write(frontend, string, len) < 0)
	{
		return POOL_END;
	}
	return POOL_CONTINUE;
}

POOL_STATUS ErrorResponse(POOL_CONNECTION *frontend, 
						  POOL_CONNECTION_POOL *backend)
{
	char *string;
	int len;

	/* read error message */
	string = pool_read_string(MASTER(backend), &len, 0);
	if (string == NULL)
		return POOL_END;

	/* forward to the frontend */
	pool_write(frontend, "E", 1);
	if (pool_write_and_flush(frontend, string, len) < 0)
		return POOL_END;
			
	return POOL_CONTINUE;
}

POOL_STATUS ErrorResponse2(POOL_CONNECTION *frontend,
						   POOL_CONNECTION_POOL *backend)
{
	char *buf;
	int len;

	/* forward to the frontend */
	pool_write(frontend, "E", 1);

	/* read error message length */
	if ((buf = pool_read2(MASTER(backend), sizeof(len))) == NULL)
		return POOL_END;

	/* forward to the frontend */
	if (pool_write_and_flush(frontend, buf, sizeof(len)) < 0)
		return POOL_END;

	len = ntohl(*(int *)buf) - sizeof(len);
	if(len < 8 || len > 30000)
	{
		/* Handle error from a pre-3.0 server */
		/* read error message */
		if ((buf = pool_read_string(MASTER(backend), &len, 0)) == NULL)
			return POOL_END;

		/* forward to the frontend */
		if (pool_write_and_flush(frontend, buf, len) < 0)
			return POOL_END;
	}
	else
	{
		/* read rest of error message */
		if ((buf = pool_read2(MASTER(backend), len)) == NULL)
			return POOL_END;

		/* forward to the frontend */
		if (pool_write_and_flush(frontend, buf, len) < 0)
			return POOL_END;
	}

	return POOL_CONTINUE;
}

POOL_STATUS NoticeResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend)
{
	char *string, *string1;
	int len, len1;

	/* read notice message */
	string = pool_read_string(MASTER(backend), &len, 0);
	if (string == NULL)
		return POOL_END;

	/* forward to the frontend */
	pool_write(frontend, "N", 1);
	if (pool_write_and_flush(frontend, string, len) < 0)
	{
		return POOL_END;
	}
	return POOL_CONTINUE;
}

static POOL_STATUS CopyInResponse(POOL_CONNECTION *frontend, 
								  POOL_CONNECTION_POOL *backend)
{
	POOL_STATUS status;

	/* forward to the frontend */
	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		if (SimpleForwardToFrontend('G', frontend, backend) != POOL_CONTINUE)
			return POOL_END;
		if (pool_flush(frontend) != POOL_CONTINUE)
			return POOL_END;
	}
	else
		if (pool_write_and_flush(frontend, "G", 1) < 0)
			return POOL_END;

	status = CopyDataRows(frontend, backend, 1);
	return status;
}

static POOL_STATUS CopyOutResponse(POOL_CONNECTION *frontend, 
								   POOL_CONNECTION_POOL *backend)
{
	POOL_STATUS status;

	/* forward to the frontend */
	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		if (SimpleForwardToFrontend('H', frontend, backend) != POOL_CONTINUE)
			return POOL_END;
		if (pool_flush(frontend) != POOL_CONTINUE)
			return POOL_END;
	}
	else
		if (pool_write_and_flush(frontend, "H", 1) < 0)
			return POOL_END;

	status = CopyDataRows(frontend, backend, 0);
	return status;
}

static POOL_STATUS CopyDataRows(POOL_CONNECTION *frontend,
								POOL_CONNECTION_POOL *backend, int copyin)
{
	char *string;
	int len;

#ifdef DEBUG
	int i = 0;
	char buf[1024];
#endif

	for (;;)
	{
		if (copyin)
		{
			if (MAJOR(backend) == PROTO_MAJOR_V3)
			{
				char kind;

				if (pool_read(frontend, &kind, 1) < 0)
					return POOL_END;
				
				SimpleForwardToBackend(kind, frontend, backend);

				/* CopyData? */
				if (kind == 'd')
					continue;
				else
					break;
			}
			else
				string = pool_read_string(frontend, &len, 1);
		}
		else
		{
			/* CopyOut */
			if (MAJOR(backend) == PROTO_MAJOR_V3)
			{
				signed char kind;

				if ((kind = pool_read_kind(backend)) < 0)
					return POOL_END;
				
				SimpleForwardToFrontend(kind, frontend, backend);

				/* CopyData? */
				if (kind == 'd')
					continue;
				else
					break;
			}
			else
			{
				string = pool_read_string(MASTER(backend), &len, 1);
			}
		}

		if (string == NULL)
			return POOL_END;

#ifdef DEBUG
		strncpy(buf, string, len);
		pool_debug("copy line %d %d bytes :%s:", i++, len, buf);
#endif

		if (copyin)
		{
			pool_write(MASTER(backend), string, len);
		}
		else
			pool_write(frontend, string, len);			

		if (len == PROTO_MAJOR_V3)
		{
			/* end of copy? */
			if (string[0] == '\\' &&
				string[1] == '.' &&
				string[2] == '\n')
			{
				break;
			}
		}
	}

	if (copyin)
	{
		if (pool_flush(MASTER(backend)) <0)
			return POOL_END;
	}
	else
		if (pool_flush(frontend) <0)
			return POOL_END;

	return POOL_CONTINUE;
}

static POOL_STATUS EmptyQueryResponse(POOL_CONNECTION *frontend,
									  POOL_CONNECTION_POOL *backend)
{
	char c;

	if (pool_read(MASTER(backend), &c, sizeof(c)) < 0)
		return POOL_END;

	pool_write(frontend, "I", 1);
	return pool_write_and_flush(frontend, "", 1);
}

static POOL_STATUS NotificationResponse(POOL_CONNECTION *frontend, 
										POOL_CONNECTION_POOL *backend)
{
	int pid, pid1;
	char *condition, *condition1;
	int len, len1;

	pool_write(frontend, "A", 1);

	if (pool_read(MASTER(backend), &pid, sizeof(pid)) < 0)
		return POOL_ERROR;

	condition = pool_read_string(MASTER(backend), &len, 0);
	if (condition == NULL)
		return POOL_END;

	pool_write(frontend, &pid, sizeof(pid));

	return pool_write_and_flush(frontend, condition, len);
}

static POOL_STATUS FunctionCall(POOL_CONNECTION *frontend, 
								POOL_CONNECTION_POOL *backend)
{
	char dummy[2];
	int oid;
	int argn;
	int i;

	pool_write(MASTER(backend), "F", 1);

	/* dummy */
	if (pool_read(frontend, dummy, sizeof(dummy)) < 0)
		return POOL_ERROR;
	pool_write(MASTER(backend), dummy, sizeof(dummy));

	/* function object id */
	if (pool_read(frontend, &oid, sizeof(oid)) < 0)
		return POOL_ERROR;

	pool_write(MASTER(backend), &oid, sizeof(oid));

	/* number of arguments */
	if (pool_read(frontend, &argn, sizeof(argn)) < 0)
		return POOL_ERROR;
	pool_write(MASTER(backend), &argn, sizeof(argn));

	argn = ntohl(argn);

	for (i=0;i<argn;i++)
	{
		int len;
		char *arg;

		/* length of each argument in bytes */
		if (pool_read(frontend, &len, sizeof(len)) < 0)
			return POOL_ERROR;

		pool_write(MASTER(backend), &len, sizeof(len));

		len = ntohl(len);

		/* argument value itself */
		if ((arg = pool_read2(frontend, len)) == NULL)
			return POOL_ERROR;
		pool_write(MASTER(backend), arg, len);
	}

	if (pool_flush(MASTER(backend)))
		return POOL_ERROR;

	return POOL_CONTINUE;
}

static POOL_STATUS FunctionResultResponse(POOL_CONNECTION *frontend, 
										  POOL_CONNECTION_POOL *backend)
{
	char dummy;
	int len;
	char *result;

	pool_write(frontend, "V", 1);

	if (pool_read(MASTER(backend), &dummy, 1) < 0)
		return POOL_ERROR;

	pool_write(frontend, &dummy, 1);

	/* non empty result? */
	if (dummy == 'G')
	{
		/* length of result in bytes */
		if (pool_read(MASTER(backend), &len, sizeof(len)) < 0)
			return POOL_ERROR;

		pool_write(frontend, &len, sizeof(len));

		len = ntohl(len);

		/* result value itself */
		if ((result = pool_read2(MASTER(backend), len)) == NULL)
			return POOL_ERROR;

		pool_write(frontend, result, len);
	}

	/* unused ('0') */
	if (pool_read(MASTER(backend), &dummy, 1) < 0)
		return POOL_ERROR;

	pool_write(frontend, "0", 1);

	return pool_flush(frontend);
}

static POOL_STATUS ProcessFrontendResponse(POOL_CONNECTION *frontend, 
										   POOL_CONNECTION_POOL *backend)
{
	char fkind;
	POOL_STATUS status;

	if (frontend->len <= 0 && frontend->no_forward != 0)
		return POOL_CONTINUE;

	if (pool_read(frontend, &fkind, 1) < 0)
	{
		pool_error("ProcessFrontendResponse: failed to read kind from frontend. frontend abnormally exited");
		return POOL_ERROR;
	}

	pool_debug("read kind from frontend %c(%02x)", fkind, fkind);

	switch (fkind)
	{
		case 'X':
			if (MAJOR(backend) == PROTO_MAJOR_V3)
			{
				int len;
				pool_read(frontend, &len, sizeof(len));
			}
			return POOL_END;

		case 'Q':
			/*
			 * Read a query from the frontend, and forward it
			 * to the backend if needed.
			 */
			status = Query(frontend, backend, NULL);
			break;

/*
		case 'S':
			status = Sync(frontend, backend);
			break;
*/
		case 'E':
			status = Execute(frontend, backend);
			break;

		case 'P':
			status = Parse(frontend, backend);
			break;

		default:
			if (MAJOR(backend) == PROTO_MAJOR_V3)
			{
				status = SimpleForwardToBackend(fkind, frontend, backend);
				if (pool_flush(MASTER(backend)))
					status = POOL_ERROR;
			}
			else if (MAJOR(backend) == PROTO_MAJOR_V2 && fkind == 'F')
				status = FunctionCall(frontend, backend);
			else
			{
				pool_error("ProcessFrontendResponse: unknown message type %c(%02x)", fkind, fkind);
				status = POOL_ERROR;
			}
			break;
	}

	if (status != POOL_CONTINUE)
		status = POOL_ERROR;
	return status;
}

static int timeoutmsec;

/*
 * enable read timeout
 */
void pool_enable_timeout()
{
	timeoutmsec = pool_config.replication_timeout;
}

/*
 * disable read timeout
 */
void pool_disable_timeout()
{
	timeoutmsec = 0;
}

/*
 * wait until read data is ready
 */
static int synchronize(POOL_CONNECTION *cp)
{
	return pool_check_fd(cp, 1);
}

/*
 * wait until read data is ready
 * if notimeout is non 0, wait forever.
 */
int pool_check_fd(POOL_CONNECTION *cp, int notimeout)
{
	fd_set readmask;
	fd_set exceptmask;
	int fd;
	int fds;
	struct timeval timeout;
	struct timeval *tp;

	fd = cp->fd;

	for (;;)
	{
		FD_ZERO(&readmask);
		FD_ZERO(&exceptmask);
		FD_SET(fd, &readmask);
		FD_SET(fd, &exceptmask);

		if (notimeout || timeoutmsec == 0)
			tp = NULL;
		else
		{
			timeout.tv_sec = pool_config.replication_timeout / 1000;
			timeout.tv_usec = (pool_config.replication_timeout - (timeout.tv_sec * 1000))*1000;
			tp = &timeout;
		}

		fds = select(fd+1, &readmask, NULL, &exceptmask, tp);

		if (fds == -1)
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;

			pool_error("pool_check_fd: select() failed. reason %s", strerror(errno));
			break;
		}

		if (FD_ISSET(fd, &exceptmask))
		{
			pool_error("pool_check_fd: exception occurred");
			break;
		}

		if (fds == 0)
		{
			pool_error("pool_check_fd: data is not ready tp->tv_sec %d tp->tp_usec %d", 
					   pool_config.replication_timeout / 1000,
					   (pool_config.replication_timeout - (timeout.tv_sec * 1000))*1000);
			break;
		}
		return 0;
	}
	return -1;
}

static void process_reporting(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend)
{
	static char *cursorname = "blank";
	static short num_fields = 3;
	static char *field_names[] = {"item", "value", "description"};
	static int oid = 0;
	static short fsize = -1;
	static int mod = 0;
	short n;
	int i, j;
	short s;
	int len;
	short colnum;

	static char nullmap[2] = {0xff, 0xff};
	int nbytes = (num_fields + 7)/8;

#define MAXVALLEN 512

	typedef struct {
		char *name;
		char value[MAXVALLEN+1];
		char *desc;
	} POOL_REPORT_STATUS;

#define MAXITEMS 128

	POOL_REPORT_STATUS status[MAXITEMS];

	short nrows;
	int size;
	int hsize;
	int slen;

	i = 0;

	status[i].name = "listen_addresses";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.listen_addresses);
	status[i].desc = "host name(s) or IP address(es) to listen to";
	i++;

	status[i].name = "port";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.port);
	status[i].desc = "pqcd accepting port number";
	i++;

	status[i].name = "socket_dir";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.socket_dir);
	status[i].desc = "pqcd socket directory";
	i++;

	status[i].name = "backend_host_name";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.backend_host_name);
	status[i].desc = "master backend host name";
	i++;

	status[i].name = "backend_port";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.backend_port);
	status[i].desc = "master backend port number";
	i++;

	status[i].name = "secondary_backend_host_name";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.secondary_backend_host_name);
	status[i].desc = "secondary backend host name";
	i++;

	status[i].name = "secondary_backend_port";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.secondary_backend_port);
	status[i].desc = "secondary backend port number";
	i++;

	status[i].name = "num_init_children";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.num_init_children);
	status[i].desc = "# of children initially pre-forked";
	i++;

	status[i].name = "child_life_time";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.child_life_time);
	status[i].desc = "if idle for this seconds, child exits";
	i++;

	status[i].name = "connection_life_time";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.connection_life_time);
	status[i].desc = "if idle for this seconds, connection closes";
	i++;

	status[i].name = "child_max_connections";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.child_max_connections);
	status[i].desc = "if max_connections received, chile exits";
	i++;

	status[i].name = "max_pool";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.max_pool);
	status[i].desc = "max # of connection pool per child";
	i++;

	status[i].name = "logdir";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.logdir);
	status[i].desc = "logging directory";
	i++;

	status[i].name = "backend_socket_dir";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.backend_socket_dir);
	status[i].desc = "Unix domain socket directory for the PostgreSQL server";
	i++;

	status[i].name = "replication_timeout";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.replication_timeout);
	status[i].desc = "if secondary does not respond in this milli seconds, abort the session";
	i++;

	status[i].name = "load_balance_mode";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.load_balance_mode);
	status[i].desc = "non 0 if operating in load balancing mode";
	i++;

	status[i].name = "weight_master";
	snprintf(status[i].value, MAXVALLEN, "%f", pool_config.weight_master);
	status[i].desc = "weight of master";
	i++;

	status[i].name = "weight_secondary";
	snprintf(status[i].value, MAXVALLEN, "%f", pool_config.weight_secondary);
	status[i].desc = "weight of secondary";
	i++;

	status[i].name = "replication_stop_on_mismatch";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.replication_stop_on_mismatch);
	status[i].desc = "stop replication mode on fatal error";
	i++;

	status[i].name = "replicate_select";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.replicate_select);
	status[i].desc = "non 0 if SELECT statement is replicated";
	i++;

	status[i].name = "reset_query_list";
	*(status[i].value) = '\0';
	for (j=0;j<pool_config.num_reset_queries;j++)
	{
		int len;
		len = MAXVALLEN - strlen(status[i].value);
		strncat(status[i].value, pool_config.reset_query_list[j], len);
		len = MAXVALLEN - strlen(status[i].value);
		strncat(status[i].value, ";", len);
	}
	status[i]. desc = "queries issued at the end of session";
	i++;

	status[i].name = "print_timestamp";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.print_timestamp);
	status[i].desc = "if true print time stamp to each log line";
	i++;

	status[i].name = "master_slave_mode";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.master_slave_mode);
	status[i].desc = "if true, operate in master/slave mode";
	i++;
		 
	status[i].name = "connection_cache";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.connection_cache);
	status[i].desc = "if true, cache connection pool";
	i++;

	status[i].name = "health_check_timeout";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.health_check_timeout);
	status[i].desc = "health check timeout";
	i++;

	status[i].name = "health_check_period";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.health_check_period);
	status[i].desc = "health check period";
	i++;

	status[i].name = "health_check_user";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.health_check_user);
	status[i].desc = "health check user";
	i++;

	status[i].name = "insert_lock";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.insert_lock);
	status[i].desc = "insert lock";
	i++;

	status[i].name = "ignore_leading_white_space";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.ignore_leading_white_space);
	status[i].desc = "ignore leading white spaces";
	i++;

	status[i].name = "current_backend_host_name";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.current_backend_host_name);
	status[i].desc = "current master host name";
	i++;

	status[i].name = "current_backend_port";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.current_backend_port);
	status[i].desc = "current master port #";
	i++;

	status[i].name = "num_reset_queries";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.num_reset_queries);
	status[i].desc = "number of queries in reset_query_list";
	i++;

	status[i].name = "log_statement";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.log_statement);
	status[i].desc = "if true, print all statements to the log";
	i++;

	status[i].name = "log_connections";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.log_connections);
	status[i].desc = "if true, print incoming connections to the log";
	i++;

	status[i].name = "log_hostname";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.log_hostname);
	status[i].desc = "if true, resolve hostname for ps and log print";
	i++;

	status[i].name = "enable_pool_hba";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.enable_pool_hba);
	status[i].desc = "if true, use pool_hba.conf for client authentication";
	i++;

	status[i].name = "memcached_bin";
	snprintf(status[i].value, MAXVALLEN, "%s", pool_config.memcached_bin);
	status[i].desc = "a path name to the memcached executable.";
	i++;

	status[i].name = "query_cache_mode";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.query_cache_mode);
	status[i].desc = "a query cache mode. if 1, Active-cache mode. if 0, Passive-cache mode.";
	i++;

	status[i].name = "query_cache_expiration";
	snprintf(status[i].value, MAXVALLEN, "%d", pool_config.query_cache_expiration);
	status[i].desc = "a query cache expiration time in seconds. if 0, expiration disabled.";
	i++;

	status[i].name = "server_status";

	if (pool_config.server_status[0] == 0)
	{
		snprintf(status[i].value, MAXVALLEN, "master(%s on %d) unused ",
		  pool_config.backend_host_name, pool_config.backend_port);
	}
	else if (pool_config.server_status[0] == 1)
	{
		snprintf(status[i].value, MAXVALLEN, "master(%s on %d) up ",
		  pool_config.backend_host_name, pool_config.backend_port);
	}
	else if (pool_config.server_status[0] == 2)
	{
		snprintf(status[i].value, MAXVALLEN, "master(%s on %d) down ",
		  pool_config.backend_host_name, pool_config.backend_port);
	}

	slen = strlen(status[i].value);

	if (pool_config.server_status[1] == 0)
	{
		snprintf(status[i].value+slen, MAXVALLEN-slen, "secondary(%s on %d) unused",
		  pool_config.secondary_backend_host_name, pool_config.secondary_backend_port);
	}
	else if (pool_config.server_status[1] == 1)
	{
		snprintf(status[i].value+slen, MAXVALLEN-slen, "secondary(%s on %d) up",
		  pool_config.secondary_backend_host_name, pool_config.secondary_backend_port);
	}
	else if (pool_config.server_status[1] == 2)
	{
		snprintf(status[i].value+slen, MAXVALLEN-slen, "secondary(%s on %d) down",
		  pool_config.secondary_backend_host_name, pool_config.secondary_backend_port);
	}
	status[i].desc = "server status";
	i++;

	nrows = i;

	if (MAJOR(backend) == PROTO_MAJOR_V2)
	{
		/* cursor response */
		pool_write(frontend, "P", 1);
		pool_write(frontend, cursorname, strlen(cursorname)+1);
	}

	/* row description */
	pool_write(frontend, "T", 1);

	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		len = sizeof(num_fields) + sizeof(len);

		for (i=0;i<num_fields;i++)
		{
			char *f = field_names[i];
			len += strlen(f)+1;
			len += sizeof(oid);
			len += sizeof(colnum);
			len += sizeof(oid);
			len += sizeof(s);
			len += sizeof(mod);
			len += sizeof(s);
		}

		len = htonl(len);
		pool_write(frontend, &len, sizeof(len));
	}

	n = htons(num_fields);
	pool_write(frontend, &n, sizeof(short));

	for (i=0;i<num_fields;i++)
	{
		char *f = field_names[i];

		pool_write(frontend, f, strlen(f)+1);		/* field name */

		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			pool_write(frontend, &oid, sizeof(oid));	/* table oid */
			colnum = htons(i);
			pool_write(frontend, &colnum, sizeof(colnum));	/* column number */
		}

		pool_write(frontend, &oid, sizeof(oid));		/* data type oid */
		s = htons(fsize);
		pool_write(frontend, &s, sizeof(fsize));		/* field size */
		pool_write(frontend, &mod, sizeof(mod));		/* modifier */

		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			s = htons(0);
			pool_write(frontend, &s, sizeof(fsize));	/* field format (text) */
		}
	}
	pool_flush(frontend);

	if (MAJOR(backend) == PROTO_MAJOR_V2)
	{
		/* ascii row */
		for (i=0;i<nrows;i++)
		{
			pool_write(frontend, "D", 1);
			pool_write_and_flush(frontend, nullmap, nbytes);

			size = strlen(status[i].name);
			hsize = htonl(size+4);
			pool_write(frontend, &hsize, sizeof(hsize));
			pool_write(frontend, status[i].name, size);

			size = strlen(status[i].value);
			hsize = htonl(size+4);
			pool_write(frontend, &hsize, sizeof(hsize));
			pool_write(frontend, status[i].value, size);

			size = strlen(status[i].desc);
			hsize = htonl(size+4);
			pool_write(frontend, &hsize, sizeof(hsize));
			pool_write(frontend, status[i].desc, size);
		}
	}
	else
	{
		/* data row */
		for (i=0;i<nrows;i++)
		{
			pool_write(frontend, "D", 1);
			len = sizeof(len) + sizeof(nrows);
			len += sizeof(int) + strlen(status[i].name);
			len += sizeof(int) + strlen(status[i].value);
			len += sizeof(int) + strlen(status[i].desc);
			len = htonl(len);
			pool_write(frontend, &len, sizeof(len));
			s = htons(3);
			pool_write(frontend, &s, sizeof(s));

			len = htonl(strlen(status[i].name));
			pool_write(frontend, &len, sizeof(len));
			pool_write(frontend, status[i].name, strlen(status[i].name));

			len = htonl(strlen(status[i].value));
			pool_write(frontend, &len, sizeof(len));
			pool_write(frontend, status[i].value, strlen(status[i].value));
			
			len = htonl(strlen(status[i].desc));
			pool_write(frontend, &len, sizeof(len));
			pool_write(frontend, status[i].desc, strlen(status[i].desc));
		}
	}

	/* complete command response */
	pool_write(frontend, "C", 1);
	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		len = htonl(sizeof(len) + strlen("SELECT")+1);
		pool_write(frontend, &len, sizeof(len));
	}
	pool_write(frontend, "SELECT", strlen("SELECT")+1);

	/* ready for query */
	pool_write(frontend, "Z", 1);
	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		len = htonl(sizeof(len) + 1);
		pool_write(frontend, &len, sizeof(len));
		pool_write(frontend, "I", 1);
	}

	pool_flush(frontend);
}

void pool_send_frontend_exits(POOL_CONNECTION_POOL *backend)
{
	int len;

	pool_write(MASTER(backend), "X", 1);

	if (MAJOR(backend) == PROTO_MAJOR_V3)
	{
		len = htonl(4);
		pool_write(MASTER(backend), &len, sizeof(len));
	}

	/*
	 * XXX we cannot call pool_flush() here since backend may already
	 * close the socket and pool_flush() automatically invokes fail
	 * over handler. This could happen in copy command (remember the
	 * famouse "lost synchronization with server, resettin g
	 * connection" message)
	 */
	pool_flush_it(MASTER(backend));

	if (DUAL_MODE)
	{
		pool_write(SECONDARY(backend), "X", 1);
		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			len = htonl(4);
			pool_write(SECONDARY(backend), &len, sizeof(len));
		}
		pool_flush_it(SECONDARY(backend));
	}
}

/*
 * -------------------------------------------------------
 * V3 functions
 * -------------------------------------------------------
 */
POOL_STATUS SimpleForwardToFrontend(char kind, POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend)
{
	int len, len1;
	char *p;
	int status;

	pool_write(frontend, &kind, 1);
	pqc_buf_add(&kind, 1);

	/*
	 * Check if packet kind == 'C'(Command complete), '1'(Parse
	 * complete), '3'(Close complete). If so, then register or
	 * unregister pending prepared statement.
	 */
	if ((kind == 'C' || kind == '1' || kind == '3') &&
		pending_function &&	pending_prepared_stmt)
	{
		pending_function(&prepared_list, pending_prepared_stmt);
	}
	else if (kind == 'C' && select_in_transaction)
		select_in_transaction = 0;

	/* 
	 * Remove a pending function if a received message is not
	 * NoticeResponse.
	 */
	if (kind != 'N')
	{
		pending_function = NULL;
		pending_prepared_stmt = NULL;
	}

	status = pool_read(MASTER(backend), &len, sizeof(len));
	if (status < 0)
	{
		pool_error("SimpleForwardToFrontend: error while reading message length");
		return POOL_END;
	}

	pool_write(frontend, &len, sizeof(len));
	pqc_buf_add((char *)&len, sizeof(len));

	len = ntohl(len) - 4 ;

	if (len <= 0)
		return POOL_CONTINUE;

	/*
	 * Read a data payload from the backend.
	 */
	p = pool_read2(MASTER(backend), len);
	if (p == NULL)
		return POOL_END;

	/*
	 * FIXME: Check a result command here to determine need to push
	 *        result into the cache (or not).
	 */
	if (kind == 'C')
	{
		/*
		 * Query cache is enabled only on SELECT command.
		 */
		if ( strncmp(p, "SELECT", 6)==0 )
		{
			/* FIXME: The result can be cached. */
			pool_debug("SimpleForwardToFrontend: kind='C' string=\"%s\"", p);
			pool_debug("SimpleForwardToFrontend: query=\"%s\"", pqc_pop_current_query());
		}
		else
		{
			/*
			 * Expire a result buffer to disable the query cache
			 * on non-SELECT statement.
			 */
			pool_debug("SimpleForwardToFrontend: Non-SELECT statement. Query cache disabled.");
			pqc_buf_init();
			IsQueryCacheEnabled = false;
		}
	}

	/*
	 * Forward the data payload to the frontend.
	 */
	if (pool_write(frontend, p, len))
	{
		return POOL_END;
	}

	pqc_buf_add(p, len);

	pool_debug("SimpleForwardToFrontend: kind=%c, len=%d, data=%p", kind, len+4, p);

	if (kind == 'A')	/* notification response */
	{
		pool_flush(frontend);	/* we need to immediately notice to frontend */
	}
	else if (kind == 'E')		/* error response? */
	{
		int i, k;
		int res1, res2;
		char *p1;

		/* don't use the result buffer on an error response. */
		IsQueryCacheEnabled = false;
		pqc_buf_init();

		/*
		 * check if the error was PANIC or FATAL. If so, we just flush
		 * the message and exit since the backend will close the
		 * channel immediately.
		 */
		for (;;)
		{
			char e;

			e = *p++;
			if (e == '\0')
				break;

			if (e == 'S' && (strcasecmp("PANIC", p) == 0 || strcasecmp("FATAL", p) == 0))
			{
				pool_flush(frontend);
				return POOL_END;
			}
			else
			{
				while (*p++)
					;
				continue;
			}
		}

		if (select_in_transaction)
		{
			if (TSTATE(backend) != 'E')
			{
				in_load_balance = 0;
				do_error_command(SECONDARY(backend), PROTO_MAJOR_V3);
			}
			select_in_transaction = 0;
		}

		for (i = 0;i < backend->num;i++)
		{
			POOL_CONNECTION *cp = backend->slots[i]->con;

			/* We need to send "sync" message to backend in extend mode
			 * so that it accepts next command.
			 * Note that this may be overkill since client may send
			 * it by itself. Moreover we do not need it in non-extend mode.
			 * At this point we regard it is not harmfull since error resonse
			 * will not be sent too frequently.
			 */
			pool_write(cp, "S", 1);
			res1 = htonl(4);
			if (pool_write_and_flush(cp, &res1, sizeof(res1)) < 0)
			{
				return POOL_END;
			}

			break;
		}

		while ((k = pool_read_kind(backend)) != 'Z')
		{
			POOL_STATUS ret;
			if (k < 0)
			{
				pool_error("SimpleForwardToBackend: pool_read_kind error");
				return POOL_ERROR;
			}

			ret = SimpleForwardToFrontend(k, frontend, backend);
			if (ret != POOL_CONTINUE)
				return ret;
			pool_flush(frontend);
		}

		status = pool_read(MASTER(backend), &res1, sizeof(res1));
		if (status < 0)
		{
			pool_error("SimpleForwardToFrontend: error while reading message length");
			return POOL_END;
		}
		res1 = ntohl(res1) - sizeof(res1);
		p1 = pool_read2(MASTER(backend), res1);
		if (p1 == NULL)
			return POOL_END;
	}

	return POOL_CONTINUE;
}

POOL_STATUS SimpleForwardToBackend(char kind, POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend)
{
	int len;
	int sendlen;
	char *p;
	char *name = NULL;

	if (pool_write(MASTER(backend), &kind, 1))
		return POOL_END;

	if (pool_read(frontend, &sendlen, sizeof(sendlen)))
	{
		return POOL_END;
	}

	if (pool_write(MASTER(backend), &sendlen, sizeof(sendlen)))
		return POOL_END;

	len = ntohl(sendlen) - 4;

	if (len == 0)
		return POOL_CONTINUE;
	else if (len < 0)
	{
		pool_error("SimpleFowardToBackend: invalid message length");
		return POOL_END;
	}

	p = pool_read2(frontend, len);
	if (p == NULL)
		return POOL_END;

	if (pool_write(MASTER(backend), p, len))
		return POOL_END;

	if (kind == 'B') /* Bind message? */
	{
		char *stmt_name, *portal_name;
		PreparedStatement *stmt;

		portal_name = p;
		stmt_name = p + strlen(portal_name) + 1;

		pool_debug("bind message: portal_name %s stmt_name %s", portal_name, stmt_name);

		if (*stmt_name == '\0')
			stmt = unnamed_statement;
		else
		{
			name = strdup(stmt_name);
			if (name == NULL)
			{
				pool_error("SimpleForwardToBackend: strdup failed: %s", strerror(errno));
				return POOL_END;
			}

			stmt = lookup_prepared_statement_by_statement(&prepared_list, name);
			free(name);
		}

		if (*portal_name == '\0')
			unnamed_portal = stmt;
		else
		{
			if (stmt->portal_name)
				free(stmt->portal_name);
			stmt->portal_name = strdup(portal_name);
		}

		/*
		 * Build a paramter string in the ascii-hex form with statement name,
		 * like 'stmt00 00010203', from a BIND command payload to use it
		 * as a part of the query cache key.
		 */
		{
			char *param = p + ( strlen(portal_name) + 1 + strlen(stmt_name) + 1);
			int paramlen = htonl(sendlen) - 4 - ( strlen(portal_name) + 1 + strlen(stmt_name) + 1);

			char key[PQC_MAX_KEY];
			int i;

			memset(key, 0, sizeof(key));

			/* NOTE: a statement name could be an empty. */
			strncat(key, stmt_name, sizeof(key));
			strncat(key, " ", sizeof(key));

			for (i = 0 ; i < paramlen ; i++)
			{
				char tmp[3];

				snprintf(tmp, sizeof(tmp), "%02x", *(param+i));
				strncat(key, tmp, sizeof(key));
			}
			pool_debug("  a built key for the prep-stmt cache: %s", key);

			if ( stmt->bind_parameter_string )
			{
				free(stmt->bind_parameter_string);
				stmt->bind_parameter_string = NULL;
			}

			stmt->bind_parameter_string = strdup(key);
		}
	}
	else if (kind == 'C' && *p == 'S' && *(p + 1))
	{
		name = strdup(p+1);
		if (name == NULL)
		{
			pool_error("SimpleForwardToBackend: strdup failed: %s", strerror(errno));
			return POOL_END;
		}
		pending_function = del_prepared_list;
		pending_prepared_stmt = malloc(sizeof(PreparedStatement));
		if (pending_prepared_stmt == NULL)
		{
			pool_error("SimpleForwardToBackend: malloc failed: %s", strerror(errno));
			return POOL_END;
		}

		pending_prepared_stmt->statement_name = normalize_prepared_stmt_name(name);
		pending_prepared_stmt->bind_parameter_string = NULL;
		if (pending_prepared_stmt->statement_name == NULL)
		{
			pool_error("SimpleForwardToBackend: malloc failed: %s", strerror(errno));
			return POOL_END;
		}
		pending_prepared_stmt->prepared_string = NULL;
	}

	if (kind == 'B' || kind == 'D' || kind == 'C')
	{
		int i;
		int kind1;

		for (i = 0;i < backend->num;i++)
		{
			POOL_CONNECTION *cp = backend->slots[i]->con;

			/*
			 * send "Flush" message so that backend notices us
			 * the completion of the command
			 */
			pool_write(cp, "H", 1);
			sendlen = htonl(4);
			if (pool_write_and_flush(cp, &sendlen, sizeof(sendlen)) < 0)
			{
				return POOL_END;
			}

			break;
		}


		/*
		 * Describe message with a portal name receive two messages.
		 * 1. ParameterDescription
		 * 2. RowDescriptions or NoData
		 */
		if (kind == 'D' && *p == 'S')
		{
			kind1 = pool_read_kind(backend);
			if (kind1 < 0)
			{
				pool_error("SimpleForwardToBackend: pool_read_kind error");
				return POOL_ERROR;
			}
			SimpleForwardToFrontend(kind1, frontend, backend);
			pool_flush(frontend);
		}

		for (;;)
		{
			kind1 = pool_read_kind(backend);
			if (kind1 < 0)
			{
				pool_error("SimpleForwardToBackend: pool_read_kind error");
				return POOL_ERROR;
			}
			SimpleForwardToFrontend(kind1, frontend, backend);
			if (pool_flush(frontend) < 0)
				return POOL_ERROR;

			/*
			 * If warning or log messages are received, we must read
			 * one message from backend.
			 */
			if (kind1 != 'N') /* Notice Message */
				break;
		}
	}
	else
	{
		if (pool_flush(MASTER(backend)))
			return POOL_END;

	}

	return POOL_CONTINUE;
}

POOL_STATUS ParameterStatus(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend)
{
	int len;
	int *len_array;
	int sendlen;
	char *p;
	char *name;
	char *value;

	pool_write(frontend, "S", 1);

	len_array = pool_read_message_length2(backend);

	if (len_array == NULL)
	{
		return POOL_END;
	}

	len = len_array[0];
	sendlen = htonl(len);
	pool_write(frontend, &sendlen, sizeof(sendlen));

	len -= 4;

	p = pool_read2(MASTER(backend), len);
	if (p == NULL)
		return POOL_END;

	name = p;
	value = p + strlen(name) + 1;

	pool_debug("name: %s value: %s", name, value);

	pool_add_param(&MASTER(backend)->params, name, value);

#ifdef DEBUG
	pool_param_debug_print(&MASTER(backend)->params);
#endif

	if (DUAL_MODE)
	{
		char *sp;

		if ((sp = pool_read2(SECONDARY(backend), len_array[1]-4)) == NULL)
			return POOL_END;

		name = sp;
		value = sp + strlen(name) + 1;

		pool_debug("secondary name: %s value: %s", name, value);
	}

	return pool_write(frontend, p, len);

}

/*
 * reset backend status. return values are:
 * 0: no query was issued 1: a query was issued 2: no more queries remain -1: error
 */
static int reset_backend(POOL_CONNECTION_POOL *backend, int qcnt)
{
	char *query;
	int qn;

	qn = pool_config.num_reset_queries;

	if (qcnt >= qn)
	{
		if (qcnt >= qn + prepared_list.cnt)
		{
			reset_prepared_list(&prepared_list);
			return 2;
		}

		send_deallocate(backend, &prepared_list, qcnt - qn);
		return 1;
	}

	query = pool_config.reset_query_list[qcnt];

	/* if transaction state is idle, we don't need to issue ABORT */
	if (TSTATE(backend) == 'I' && !strcmp("ABORT", query))
		return 0;

	if (Query(NULL, backend, query) != POOL_CONTINUE)
		return -1;

	return 1;
}

/*
 * return non 0 if SQL is SELECT statement.
 */
static int is_select_query(char *sql)
{
	if (pool_config.ignore_leading_white_space)
	{
		/* ignore leading white spaces */
		while (*sql && isspace(*sql))
			sql++;
	}

	return (!strncasecmp(sql, "SELECT", 6));

}

/*
 * return non 0 if SQL is SELECT statement.
 */
static int is_sequence_query(char *sql)
{
	if (pool_config.ignore_leading_white_space)
	{
		/* ignore leading white spaces */
		while (*sql && isspace(*sql))
			sql++;
	}

	if (strncasecmp(sql, "SELECT", 6))
		return 0;

	sql += 6;
	while (*sql && isspace(*sql))
		sql++;

	/* SELECT NEXTVAL('xxx') */
	if (*sql && !strncasecmp(sql, "NEXTVAL", 7))
		return 1;

	/* SELECT SETVAL('xxx') */
	if (*sql && !strncasecmp(sql, "SETVAL", 6))
		return 1;

	return 0;
}

/*
 * return non 0 if load balance is possible
 */
static int load_balance_enabled(POOL_CONNECTION_POOL *backend, char *sql)
{
	return (pool_config.load_balance_mode &&
			DUAL_MODE &&
			MAJOR(backend) == PROTO_MAJOR_V3 &&
			TSTATE(backend) == 'I' &&
			is_select_query(sql) &&
			!is_sequence_query(sql));
}

/*
 * start load balance mode
 */
static void start_load_balance(POOL_CONNECTION_POOL *backend)
{
	int i;
	int master;

	/* save backend connection slots */
	for (i=0;i<backend->num;i++)
	{
		slots[i] = backend->slots[i];
	}

	/* choose a master in random manner with weight */
	master = (random() <= weight_master)?0:1;
	backend->slots[0] = slots[master];
	pool_debug("start_load_balance: selected master is %d", master);

	/* start load balancing */
	in_load_balance = 1;
}

/*
 * finish load balance mode
 */
static void end_load_balance(POOL_CONNECTION_POOL *backend)
{
	int i;

	/* restore backend connection slots */
	for (i=0;i<backend->num;i++)
	{
		backend->slots[i] = slots[i];
	}

	in_load_balance = 0;

	pool_debug("end_load_balance: end load balance mode");
}

/*
 * send error message to frontend
 */
void pool_send_error_message(POOL_CONNECTION *frontend, int protoMajor,
							 char *code,
							 char *message,
							 char *detail,
							 char *hint,
							 char *file,
							 int line)
{
#define MAXDATA	1024
#define MAXMSGBUF 128
	if (protoMajor == PROTO_MAJOR_V2)
	{
		pool_write(frontend, "E", 1);
		pool_write_and_flush(frontend, message, strlen(message)+1);
	}
	else if (protoMajor == PROTO_MAJOR_V3)
	{
		char data[MAXDATA];
		char msgbuf[MAXMSGBUF];
		int len;
		int thislen;
		int sendlen;

		len = 0;

		pool_write(frontend, "E", 1);

		/* error level */
		thislen = snprintf(msgbuf, MAXMSGBUF, "SERROR");
		memcpy(data +len, msgbuf, thislen+1);
		len += thislen + 1;

		/* code */
		thislen = snprintf(msgbuf, MAXMSGBUF, "C%s", code);
		memcpy(data +len, msgbuf, thislen+1);
		len += thislen + 1;

		/* message */
		thislen = snprintf(msgbuf, MAXMSGBUF, "M%s", message);
		memcpy(data +len, msgbuf, thislen+1);
		len += thislen + 1;

		/* detail */
		if (*detail != '\0')
		{
			thislen = snprintf(msgbuf, MAXMSGBUF, "D%s", detail);
			memcpy(data +len, msgbuf, thislen+1);
			len += thislen + 1;
		}

		/* hint */
		if (*hint != '\0')
		{
			thislen = snprintf(msgbuf, MAXMSGBUF, "H%s", hint);
			memcpy(data +len, msgbuf, thislen+1);
			len += thislen + 1;
		}

		/* file */
		thislen = snprintf(msgbuf, MAXMSGBUF, "F%s", file);
		memcpy(data +len, msgbuf, thislen+1);
		len += thislen + 1;

		/* line */
		thislen = snprintf(msgbuf, MAXMSGBUF, "L%d", line);
		memcpy(data +len, msgbuf, thislen+1);
		len += thislen + 1;

		/* stop null */
		len++;
		*(data + len - 1) = '\0';

		sendlen = len;
		len = htonl(len + 4);
		pool_write(frontend, &len, sizeof(len));
		pool_write_and_flush(frontend, data, sendlen);
	}
	else
		pool_error("send_error_message: unknown protocol major %d", protoMajor);
}

/*
 * sends q query in sync manner.
 * this function sends a query and wait for CommandComplete/ReadyForQuery.
 * if an error occured, it returns with POOL_ERROR.
 * this function does NOT handle SELECT/SHOW quries.
 * if no_ready_for_query is non 0, returns without reading the packet
 * length for ReadyForQuery. This mode is necessary when called from ReadyForQuery().
 */
static POOL_STATUS do_command(POOL_CONNECTION *backend, char *query, int protoMajor,
							  int no_ready_for_query)
{
	int len;
	int status;
	char kind;
	char *string;
	int deadlock_detected;

	pool_debug("do_command: Query: %s", query);

	/* send the query to the backend */
	pool_write(backend, "Q", 1);
	len = strlen(query)+1;

	if (protoMajor == PROTO_MAJOR_V3)
	{
		int sendlen = htonl(len + 4);
		pool_write(backend, &sendlen, sizeof(sendlen));
	}

	if (pool_write_and_flush(backend, query, len) < 0)
	{
		return POOL_END;
	}

	/*
	 * We must check deadlock error because a aborted transaction
	 * by detecting deadlock isn't same on all nodes.
	 * If a transaction is aborted on master node, pgpool send a
	 * error query to another nodes.
	 */
	deadlock_detected = detect_deadlock_error(backend, protoMajor);
	if (deadlock_detected < 0)
		return POOL_END;

	/*
	 * Expecting CompleteCommand
	 */
	status = pool_read(backend, &kind, sizeof(kind));
	if (status < 0)
	{
		pool_error("do_command: error while reading message kind");
		return POOL_END;
	}

	if (kind != 'C')
	{
		pool_log("do_command: backend does not successfully complete command %s status %c", query, kind);

	}

	/*
	 * read command tag of CommandComplete response
	 */
	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (pool_read(backend, &len, sizeof(len)) < 0)
			return POOL_END;
		len = ntohl(len) - 4;
		string = pool_read2(backend, len);
		if (string == NULL)
			return POOL_END;
		pool_debug("command tag: %s", string);
	}
	else
	{
		string = pool_read_string(backend, &len, 0);
		if (string == NULL)
			return POOL_END;
	}

	/*
	 * Expecting ReadyForQuery
	 */
	status = pool_read(backend, &kind, sizeof(kind));
	if (status < 0)
	{
		pool_error("do_command: error while reading message kind");
		return POOL_END;
	}

	if (kind != 'Z')
	{
		pool_error("do_command: backend does not return ReadyForQuery");
		return POOL_END;
	}

	if (no_ready_for_query)
		return POOL_CONTINUE;

	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (pool_read(backend, &len, sizeof(len)) < 0)
			return POOL_END;

		status = pool_read(backend, &kind, sizeof(kind));
		if (status < 0)
		{
			pool_error("do_command: error while reading transaction status");
			return POOL_END;
		}

		/* set transaction state */
		pool_debug("ReadyForQuery: transaction state: %c", kind);
		backend->tstate = kind;
	}

	return deadlock_detected ? POOL_DEADLOCK : POOL_CONTINUE;
}

/*
 * Send syntax error query to abort transaction.
 * We need to sync transaction status in transaction block.
 * SELECT query is sended to master only.
 * If SELECT is error, we must abort transaction on other nodes.
 */
static POOL_STATUS do_error_command(POOL_CONNECTION *backend, int protoMajor)
{
	int len;
	int status;
	char kind;
	char *string;
	char *error_query = "send invalid query from pqcd to abort transaction";

	/* send the query to the backend */
	pool_write(backend, "Q", 1);
	len = strlen(error_query)+1;

	if (protoMajor == PROTO_MAJOR_V3)
	{
		int sendlen = htonl(len + 4);
		pool_write(backend, &sendlen, sizeof(sendlen));
	}

	if (pool_write_and_flush(backend, error_query, len) < 0)
	{
		return POOL_END;
	}

	/*
	 * Expecting CompleteCommand
	 */
	status = pool_read(backend, &kind, sizeof(kind));
	if (status < 0)
	{
		pool_error("do_command: error while reading message kind");
		return POOL_END;
	}

	/*
	 * read ErrorResponse message
	 */
	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (pool_read(backend, &len, sizeof(len)) < 0)
			return POOL_END;
		len = ntohl(len) - 4;
		string = pool_read2(backend, len);
		if (string == NULL)
			return POOL_END;
		pool_debug("command tag: %s", string);
	}
	else
	{
		string = pool_read_string(backend, &len, 0);
		if (string == NULL)
			return POOL_END;
	}

	return POOL_CONTINUE;
}

/*
 * judge if we need to lock the table
 * to keep SERIAL consistency among servers
 */
static int need_insert_lock(POOL_CONNECTION_POOL *backend, char *query)
{
	if (MAJOR(backend) != PROTO_MAJOR_V3)
		return 0;
	
	/*
	 * either insert_lock directive specified and without "NO INSERT LOCK" comment
	 * or "INSERT LOCK" comment exists?
	 */
	if ((pool_config.insert_lock && strncasecmp(query, NO_LOCK_COMMENT, NO_LOCK_COMMENT_SZ)) ||
		strncasecmp(query, LOCK_COMMENT, LOCK_COMMENT_SZ) == 0)
	{
		/* INSERT STATEMENT? */
		query = skip_comment(query);
		if (strncasecmp(query, "INSERT", 6) == 0)
			return 1;
	}

	return 0;
}

/*
 * if a transaction has not already started, start a new one.
 * issue LOCK TABLE IN SHARE ROW EXCLUSIVE MODE
 */
static POOL_STATUS insert_lock(POOL_CONNECTION_POOL *backend, char *query)
{
	char *table;
	char qbuf[1024];
	POOL_STATUS status;
	int i, deadlock_detected = 0;

	/* insert_lock can be used in V3 only */
	if (MAJOR(backend) != PROTO_MAJOR_V3)
		return POOL_CONTINUE;

	/* get table name */
	table = get_insert_command_table_name(query);

	/* could not get table name. probably wrong SQL command */
	if (table == NULL)
	{
		return POOL_CONTINUE;
	}

	snprintf(qbuf, sizeof(qbuf), "LOCK TABLE %s IN SHARE ROW EXCLUSIVE MODE", table);

	/* if we are not in a transaction block,
	 * start a new transaction
	 */
	if (TSTATE(backend) == 'I')
	{
		for (i = 0;i < backend->num;i++)
		{
			if (do_command(backend->slots[i]->con, "BEGIN", PROTO_MAJOR_V3, 0) != POOL_CONTINUE)
				return POOL_END;
		}

		/* mark that we started new transaction */
		internal_transaction_started = 1;
	}

	status = POOL_CONTINUE;

	/* issue lock table command */
	for (i = 0;i < backend->num;i++)
	{
		if (deadlock_detected)
			status = do_command(backend->slots[i]->con, POOL_ERROR_QUERY, PROTO_MAJOR_V3, 0);
		else
			status = do_command(backend->slots[i]->con, qbuf, PROTO_MAJOR_V3, 0);

		if (status == POOL_DEADLOCK)
			deadlock_detected = 1;
	}

	return status;
}

/*
 * obtain table name in INSERT statement
 */
static char *get_insert_command_table_name(char *query)
{
	static char table[1024];
	char *qbuf;
	char *token;

	table[0] = '\0';

	/* skip comment */
    query = skip_comment(query);

	if (*query == '\0')
		return table;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* skip non spaces(INSERT) */
	while (*query && !isspace(*query))
		query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* skip non spaces(INTO) */
	while (*query && !isspace(*query))
		query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* get table name */
	qbuf = strdup(query);
 	token = strtok(qbuf, "\r\n\t (");

	if (token == NULL)
	{
		pool_error("get_insert_command_table_name: could not get table name");
		return NULL;
	}

	strncpy(table, token, sizeof(table));
	free(qbuf);

	pool_debug("get_insert_command_table_name: extracted table name: %s", table);

	return table;
}

/*
 * obtain portal name in EXECUTE statement
 */
static char *get_execute_command_portal_name(char *query)
{
	static char portal[1024];
	char *qbuf;
	char *token;

	portal[0] = '\0';

	/* skip comment */
    query = skip_comment(query);

	if (*query == '\0')
		return portal;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* skip non spaces(EXECUTE) */
	while (*query && !isspace(*query))
		query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* get portal name */
	qbuf = strdup(query);
 	token = strtok(qbuf, "\r\n\t (");

	if (token == NULL)
	{
		pool_error("get_execute_command_portal_name: could not get portal name");
		return NULL;
	}

	strncpy(portal, token, sizeof(portal));
	free(qbuf);

	pool_debug("get_execute_command_portal_name: extracted portal name: %s", portal);

	return portal;
}

/*
 * obtain portal name and statement in PREPARED statement
 */
static PreparedStatement *get_prepared_command_portal_and_statement(char *query)
{
	PreparedStatement *stmt;
	static char portal[1024];
	char *string = NULL;
	char *qbuf;
	char *token;
	int len;

	portal[0] = '\0';

	/* skip comment */
    query = skip_comment(query);

	if (*query == '\0')
		return NULL;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* skip non spaces(PREPARED) */
	while (*query && !isspace(*query))
		query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* get portal name */
	qbuf = strdup(query);
 	token = strtok(qbuf, "\r\n\t (");

	if (token == NULL)
	{
		pool_debug("get_prepared_command_portal_and_statement: could not get portal name");
		return NULL;
	}

	strncpy(portal, token, sizeof(portal));
	free(qbuf);

	/* skip data type list */
	while (*query && *query != ')')
		query++;

	if (!*query)
	{
		pool_debug("get_prepared_command_portal_and_statement: could not get statement");
		return NULL;
	}
	query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* skip non spaces(AS) */
	while (*query && !isspace(*query))
		query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	if (!*query)
	{
		pool_debug("get_prepared_command_portal_and_statement: could not get statement");
		return NULL;
	}

	len = strlen(query) + 1;
	string = malloc(len);
	if (string == NULL)
	{
		pool_error("get_prepared_command_portal_and_statement: malloc failed: %s", strerror(errno));
		return NULL;
	}
	memcpy(string, query, len);

	stmt = malloc(sizeof(PreparedStatement));
	stmt->statement_name = normalize_prepared_stmt_name(portal);
	stmt->portal_name = NULL;
	stmt->prepared_string = string;

	return stmt;
}


/* judge if this is a DROP DATABASE command */
static int is_drop_database(char *query)
{
	/* skip comment */
    query = skip_comment(query);

	if (*query == '\0')
		return 0;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* DROP? */
	if (strncasecmp("DROP", query, 4))
		return 0;

	/* skip DROP */
	while (*query && !isspace(*query))
		query++;

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	/* DATABASE? */
	if (strncasecmp("DATABASE", query, 8))
		return 0;

	return 1;
}

/* skip SQL comments */
static char *skip_comment(char *query)
{
	if (strncmp(query, "/*", 2) == 0)
	{
		query += 2;
		while (query)
		{
			if (strncmp(query, "*/", 2) == 0)
			{
				query += 2;
				break;
			}
			query++;
		}
	}
	return query;
}

void init_prepared_list(void)
{
	prepared_list.cnt = 0;
	prepared_list.size = INIT_STATEMENT_LIST_SIZE;
	prepared_list.stmt_list = malloc(sizeof(char *) * prepared_list.size);
	if (prepared_list.stmt_list == NULL)
	{
		pool_error("init_prepared_list: malloc failed: %s", strerror(errno));
		exit(1);
	}
}

static void add_prepared_list(PreparedStatementList *p, PreparedStatement *stmt)
{
	if (p->cnt == p->size)
	{
		p->size *= 2;
		p->stmt_list = realloc(p->stmt_list, sizeof(char *) * p->size);
		if (p->stmt_list == NULL)
		{
			pool_error("add_prepared_list: realloc failed: %s", strerror(errno));
			exit(1);
		}
	}

	p->stmt_list[p->cnt++] = stmt;
}

static void add_unnamed_portal(PreparedStatementList *p, PreparedStatement *stmt)
{
	if (unnamed_statement && unnamed_statement->statement_name == NULL)
	{
		free(unnamed_statement->prepared_string);
		free(unnamed_statement);
	}

	unnamed_portal = NULL;
	unnamed_statement = stmt;
}

static void del_prepared_list(PreparedStatementList *p, PreparedStatement *stmt)
{
	int i;

	for (i = 0; i < p->cnt; i++)
	{
		if (strcmp(p->stmt_list[i]->statement_name, stmt->statement_name) == 0)
		break;
	}

	free(stmt->statement_name);
	free(stmt);
	
	if (i == p->cnt)
		return;

	free(p->stmt_list[i]->statement_name);
	free(p->stmt_list[i]->portal_name);
	free(p->stmt_list[i]->prepared_string);
	free(p->stmt_list[i]);
	if (i != p->cnt - 1)
	{
		memmove(&p->stmt_list[i], &p->stmt_list[i+1],
				sizeof(PreparedStatement *) * (p->cnt - i - 1));
	}
	p->cnt--;
}

static void reset_prepared_list(PreparedStatementList *p)
{
	int i;

	for (i = 0; i < p->cnt; i++)
	{
		free(p->stmt_list[i]->statement_name);
		free(p->stmt_list[i]->portal_name);
		free(p->stmt_list[i]->prepared_string);
		free(p->stmt_list[i]);
	}
	p->cnt = 0;
}

static PreparedStatement *lookup_prepared_statement_by_statement(PreparedStatementList *p, const char *name)
{
	int i;

	/* unnamed portal? */
	if (name == NULL || name[0] == '\0' || (name[0] == '\"' && name[1] == '\"'))
		return unnamed_statement;

	for (i = 0; i < p->cnt; i++)
	{
		if (strcmp(p->stmt_list[i]->statement_name, name) == 0)
			return p->stmt_list[i];
	}

	return NULL;
}

static PreparedStatement *lookup_prepared_statement_by_portal(PreparedStatementList *p, const char *name)
{
	int i;

	/* unnamed portal? */
	if (name == NULL || name[0] == '\0' || (name[0] == '\"' && name[1] == '\"'))
		return unnamed_portal;

	for (i = 0; i < p->cnt; i++)
	{
		if (p->stmt_list[i]->portal_name &&
			strcmp(p->stmt_list[i]->portal_name, name) == 0)
			return p->stmt_list[i];
	}

	return NULL;
}

static int send_deallocate(POOL_CONNECTION_POOL *backend, PreparedStatementList *p,
					int n)
{
	char *query;
	int len;

	if (p->cnt <= n)
		return 1;
	
	len = strlen(p->stmt_list[n]->statement_name) + 14; /* "DEALLOCATE \"" + "\"" + '\0' */
	query = malloc(len);
	if (query == NULL)
	{
		pool_error("send_deallocate: malloc failed: %s", strerror(errno));
		exit(1);
	}
	sprintf(query, "DEALLOCATE \"%s\"", p->stmt_list[n]->statement_name);
	if (Query(NULL, backend, query) != POOL_CONTINUE)
	{
		free(query);
		return 1;
	}
	free(query);

	return 0;
}

static char *normalize_prepared_stmt_name(const char *name)
{
	char *result;
	int i, len;

	len = strlen(name);

	if (name[0] != '"' && name[len-1] != '"')
	{
		result = strdup(name);
		if (result == NULL)
			return result;
		for (i = 0; i < len; i++)
		{
			if (isupper(result[i]))
			{
				result[i] += 32; /* convert to lower */
			}
		}
	}
	else
	{
		result = malloc(len - 1);
		if (result == NULL)
			return result;

		result = memcpy(result, name+1, len-2);
		result[len-1] = '\0';
	}

	return result;
}

static void query_ps_status(char *query, POOL_CONNECTION_POOL *backend)
{
	StartupPacket *sp;
	char psbuf[1024];
	int i;

	/* skip comment */
    query = skip_comment(query);

	if (*query == '\0')
		return;

	sp = MASTER_CONNECTION(backend)->sp;
	i = snprintf(psbuf, sizeof(psbuf), "%s %s %s ",
				 sp->user, sp->database, remote_ps_data);

	/* skip spaces */
	while (*query && isspace(*query))
		query++;

	for (; i< sizeof(psbuf); i++)
	{
		if (!*query || isspace(*query))
			break;

		psbuf[i] = toupper(*query++);
	}
	psbuf[i] = '\0';

	set_ps_display(psbuf, false);
}

static POOL_STATUS error_kind_mismatch(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend, int kind, int kind1)
{
	int sts;

	pool_error("pool_process_query: kind does not match between backends master(%c) secondary(%c)",
			   kind, kind1);
	pool_send_error_message(frontend, MAJOR(backend), "XX000", 
							"kind mismatch between backends", "",
							"check data consistency between master and secondary", __FILE__, __LINE__);

	/* health check */
	sts = health_check();
	if (sts == -1)
	{
		notice_backend_error(1);
		exit(1);
	}
	else if (sts == -2)
	{
		notice_backend_error(0);
		exit(1);
	}
	
	if (pool_config.replication_stop_on_mismatch)
		return POOL_FATAL;
	else
		return POOL_ERROR;
}

static int detect_deadlock_error(POOL_CONNECTION *master, int major)
{
	int deadlock = 0;
	char kind;
	int readlen = 0, len;
	char *buf;
	char *p, *str;

	if ((buf = malloc(1024)) == NULL)
	{
		pool_error("detect_deadlock_error: malloc failed");
		return -1;
	}

	if (pool_read(master, &kind, sizeof(kind)))
		return POOL_END;
	readlen += sizeof(kind);
	p = buf;
	memcpy(p, &kind, sizeof(kind));
	p += sizeof(kind);

	if (kind == 'E') /* deadlock error? */
	{
		/* read actual query */
		if (major == PROTO_MAJOR_V3)
		{
			char *error_code;
			
			if (pool_read(master, &len, sizeof(len)) < 0)
				return POOL_END;
			readlen += sizeof(len);
			memcpy(p, &len, sizeof(len));
			p += sizeof(len);
			
			len = ntohl(len) - 4;
			str = malloc(len);
			pool_read(master, str, len);
			readlen += len;
			if (readlen > 1024)
			{
				buf = realloc(buf, readlen);
				if (buf == NULL)
				{
					pool_error("detect_deadlock_error: malloc failed");
					return -1;
				}
			}
			memcpy(p, str, len);

			error_code = str;
			while (*error_code)
			{
				if (*error_code == 'C')
				{
					if (strcmp(error_code+1, DEADLOCK_ERROR_CODE) == 0) /* deadlock error */
					{
						pool_debug("SimpleQuery: receive deadlock error from master node.");
						deadlock = 1;
					}
					break;
				}
				else
					error_code = error_code + strlen(error_code) + 1;
			}
			free(str);
		}
		else
		{
			str = pool_read_string(master, &len, 0);
			readlen += len;
			if (readlen > 1024)
			{
				buf = realloc(buf, readlen);
				if (buf == NULL)
				{
					pool_error("detect_deadlock_error: malloc failed");
					return -1;
				}
			}
			memcpy(p, str, len);
		}
	}
	if (pool_unread(master, buf, readlen) != 0)
		deadlock = -1;
	free(buf);
	return deadlock;
}
