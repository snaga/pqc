/* -*-pgsql-c-*- */
/*
 *
 * $Header: /cvsroot/pgpool/pgpool/pool_config.l,v 1.12 2007/07/18 04:48:04 y-asaba Exp $
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
 * pool_config.l: read configuration file
 *
 */

%{

#include "pool.h"

#include <stdio.h>
#include <string.h>

/* to shut off compiler warnings */
int yylex(void);

POOL_CONFIG pool_config;	/* configuration values */
static unsigned Lineno;

typedef enum {
  POOL_KEY = 1,
  POOL_INTEGER,
  POOL_REAL,
  POOL_STRING,
  POOL_UNQUOTED_STRING,
  POOL_EQUALS,
  POOL_EOL,
  POOL_PARSE_ERROR
} POOL_TOKEN;

static char *extract_string(char *value, POOL_TOKEN token);
static char **extract_string_tokens(char *str, char *delim, int *n);

%}

%option 8bit
%option never-interactive
%option nounput
%option noyywrap

SIGN            ("-"|"+")
DIGIT           [0-9]
HEXDIGIT        [0-9a-fA-F]

INTEGER         {SIGN}?({DIGIT}+|0x{HEXDIGIT}+)

EXPONENT        [Ee]{SIGN}?{DIGIT}+
REAL            {SIGN}?{DIGIT}*"."{DIGIT}*{EXPONENT}?

LETTER          [A-Za-z_\200-\377]
LETTER_OR_DIGIT [A-Za-z_0-9\200-\377]

KEY              {LETTER}{LETTER_OR_DIGIT}*

UNQUOTED_STRING {LETTER}({LETTER_OR_DIGIT}|[-._:/])*
STRING          \'([^'\n]|\\.)*\'

%%

\n              Lineno++; return POOL_EOL;
[ \t\r]+        /* eat whitespace */
#.*$            /* eat comment */

{KEY}           return POOL_KEY;
{STRING}        return POOL_STRING;
{UNQUOTED_STRING} return POOL_UNQUOTED_STRING;
{INTEGER}       return POOL_INTEGER;
{REAL}          return POOL_REAL;
=               return POOL_EQUALS;

.               return POOL_PARSE_ERROR;

%%

static int eval_logical(char *str);

int pool_get_config(char *confpath)
{
	FILE *fd;
	int token;
	char key[1024];
	static char *default_reset_query_list[] = {"ABORT", "RESET ALL", "SET SESSION AUTHORIZATION DEFAULT"};

	/* set hardcoded default values */
	pool_config.listen_addresses = "localhost";
	pool_config.port = 9999;
	pool_config.socket_dir = DEFAULT_SOCKET_DIR;
	pool_config.backend_host_name = "";
	pool_config.backend_port = 5432;
	pool_config.backend_socket_dir = DEFAULT_SOCKET_DIR;
	pool_config.secondary_backend_host_name = "";
	pool_config.secondary_backend_port = 0;		/* no secondary backend */
	pool_config.num_init_children = 32;
	pool_config.child_life_time = 300;
	pool_config.connection_life_time = 0;
	pool_config.child_max_connections = 0;
	pool_config.max_pool = 4;
	pool_config.logdir = DEFAULT_LOGDIR;

	pool_config.current_backend_host_name = "";
	pool_config.current_backend_port = 5432;

	pool_config.replication_timeout = 0;
	pool_config.load_balance_mode = 0;
	pool_config.replication_stop_on_mismatch = 0;
	pool_config.replicate_select = 0;
	pool_config.weight_master = 0.5;
	pool_config.weight_secondary = 0.5;
	pool_config.reset_query_list = default_reset_query_list;
	pool_config.num_reset_queries = sizeof(default_reset_query_list)/sizeof(char *);
	pool_config.reset_query_list = default_reset_query_list;
	pool_config.print_timestamp = 1;
	pool_config.master_slave_mode = 0;
	pool_config.connection_cache = 1;
	pool_config.health_check_timeout = 20;
	pool_config.health_check_period = 0;
	pool_config.health_check_user = "nobody";
	pool_config.insert_lock = 0;
	pool_config.num_servers = 0;
	pool_config.log_statement = 0;
	pool_config.log_connections = 0;
	pool_config.log_hostname = 0;
	pool_config.enable_pool_hba = 1;
	pool_config.memcached_bin = DEFAULT_MEMCACHED_BIN;
	pool_config.query_cache_mode = 1;
	pool_config.query_cache_expiration = 30;
#define PARSE_ERROR()		pool_error("pool_config: parse error at line %d '%s'", Lineno, yytext)

	/* open config file */
	fd = fopen(confpath, "r");
	if (!fd)
	{
		fprintf(stderr, "pool_config: could not open configuration file (%s)\n",
				POOL_CONF_FILE_NAME);
		fprintf(stderr, "pool_config: using default values...\n");
		return 0;
	}

	yyin = fd;
	Lineno = 1;

	while ((token = yylex()))
	{
		if (token == POOL_PARSE_ERROR)
		{
			PARSE_ERROR();
			return(-1);
		}
		if (token == POOL_EOL)
			continue;

		if (token != POOL_KEY)
		{
			PARSE_ERROR();
			return(-1);
		}

		strncpy(key, yytext, sizeof(key));

		pool_debug("key: %s", key);

		token = yylex();

		if (token == POOL_EQUALS)
			token = yylex();

		pool_debug("value: %s kind: %d", yytext, token);

		if (!strcmp(key, "allow_inet_domain_socket"))
		{
			/* for backward compatibility */
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			if (v)
				pool_config.listen_addresses = strdup("*");
			else
				pool_config.listen_addresses = strdup("");
		}
		else if (!strcmp(key, "listen_addresses"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.listen_addresses = str;
		}

		else if (!strcmp(key, "port"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 1024)
			{
				pool_error("pool_config: %s must be 1024 or higher numeric value", key);
				return(-1);
			}
			pool_config.port = v;
		}
		else if (!strcmp(key, "socket_dir"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.socket_dir = str;
		}
		else if (!strcmp(key, "backend_host_name"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.backend_host_name = str;
			pool_debug(":%s:", pool_config.backend_host_name);
		}
		else if (!strcmp(key, "backend_port"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 1024)
			{
				pool_error("pool_config: %s must be 1024 or higher numeric value", key);
				return(-1);
			}
			pool_config.backend_port = v;
		}
		else if (!strcmp(key, "secondary_backend_host_name"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.secondary_backend_host_name = str;
			pool_debug(":%s:", pool_config.secondary_backend_host_name);
		}
		else if (!strcmp(key, "secondary_backend_port"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || (v != 0 && v < 1024))
			{
				pool_error("pool_config: %s must be 1024 or higher numeric value", key);
				return(-1);
			}
			pool_config.secondary_backend_port = v;
		}
		else if (!strcmp(key, "num_init_children"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 1)
			{
				pool_error("pool_config: %s must be higher than 1 numeric value", key);
				return(-1);
			}
			pool_config.num_init_children = v;
		}
		else if (!strcmp(key, "child_life_time"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.child_life_time = v;
		}
		else if (!strcmp(key, "connection_life_time"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.connection_life_time = v;
		}
		else if (!strcmp(key, "child_max_connections"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.child_max_connections = v;
		}
		else if (!strcmp(key, "max_pool"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.max_pool = v;
		}
		else if (!strcmp(key, "logdir"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.logdir = str;
		}
		else if (!strcmp(key, "backend_socket_dir"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.backend_socket_dir = str;
		}
		else if (!strcmp(key, "replication_timeout"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.replication_timeout = v;
		}
		else if (!strcmp(key, "load_balance_mode"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.load_balance_mode = v;
		}
		else if (!strcmp(key, "replication_stop_on_mismatch"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_debug("replication_stop_on_mismatch: %d", v);
			pool_config.replication_stop_on_mismatch = v;
		}
		else if (!strcmp(key, "replicate_select"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_debug("replicate_select: %d", v);
			pool_config.replicate_select = v;
		}
		else if (!strcmp(key, "weight_master"))
		{
			double v = atof(yytext);

			if (v < 0.0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}

			pool_debug("weight_master: %f", v);
			pool_config.weight_master = v;
		}
		else if (!strcmp(key, "weight_secondary"))
		{
			double v = atof(yytext);

			if (v < 0.0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}

			pool_debug("weight_secondary: %f", v);
			pool_config.weight_secondary = v;
		}

		else if (!strcmp(key, "reset_query_list"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.reset_query_list = extract_string_tokens(str, ";", &pool_config.num_reset_queries);
			if (pool_config.reset_query_list == NULL)
			{
				return(-1);
			}
		}

		if (pool_config.weight_master == 0.0 && pool_config.weight_secondary == 0.0)
		{
			pool_error("pool_config: both of weight_master and weight_secondary cannot be 0");
			return(-1);
		}

		else if (!strcmp(key, "print_timestamp"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.print_timestamp = v;
		}

		else if (!strcmp(key, "master_slave_mode"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
		}

		else if (!strcmp(key, "connection_cache"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.connection_cache = v;
		}

		else if (!strcmp(key, "health_check_timeout"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be equal or higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.health_check_timeout = v;
		}

		else if (!strcmp(key, "health_check_period"))
		{
			int v = atoi(yytext);

			if (token != POOL_INTEGER || v < 0)
			{
				pool_error("pool_config: %s must be equal or higher than 0 numeric value", key);
				return(-1);
			}
			pool_config.health_check_period = v;
		}

		else if (!strcmp(key, "health_check_user"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.health_check_user = str;
		}

		else if (!strcmp(key, "insert_lock"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.insert_lock = v;
		}

		else if (!strcmp(key, "ignore_leading_white_space"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.ignore_leading_white_space = v;
		}
       	else if (!strcmp(key, "log_statement"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.log_statement = v;
		}
       	else if (!strcmp(key, "log_connections"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.log_connections = v;
		}
       	else if (!strcmp(key, "log_hostname"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.log_hostname = v;
		}
       	else if (!strcmp(key, "enable_pool_hba"))
		{
			int v = eval_logical(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.enable_pool_hba = v;
		}
       	else if (!strcmp(key, "memcached_bin"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}
			pool_config.memcached_bin = str;
		}
       	else if (!strcmp(key, "query_cache_mode"))
		{
			char *str;

			if (token != POOL_STRING && token != POOL_UNQUOTED_STRING && token != POOL_KEY)
			{
				PARSE_ERROR();
				return(-1);
			}
			str = extract_string(yytext, token);
			if (str == NULL)
			{
				return(-1);
			}

			if (strcmp(str, "active") == 0)
			{
				pool_config.query_cache_mode = 1;
			}
			else if (strcmp(str, "passive") == 0)
			{
				pool_config.query_cache_mode = 0;
			}
			else
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
		}
       	else if (!strcmp(key, "query_cache_expiration"))
		{
			int v = atoi(yytext);

			if (v < 0)
			{
				pool_error("pool_config: invalid value %s for %s", yytext, key);
				return(-1);
			}
			pool_config.query_cache_expiration = v;
		}
	}

	if (pool_config.backend_port)
		pool_config.server_status[pool_config.num_servers++] = 1;

	if (pool_config.secondary_backend_port)
		pool_config.server_status[pool_config.num_servers++] = 1;

	return 0;
}

static char *extract_string(char *value, POOL_TOKEN token)
{
	char *ret;

	ret = strdup(value);
	if (!ret)
	{
		pool_error("extract_string: out of memory");
		return NULL;
	}

	if (token == POOL_STRING)
	{
		ret[strlen(ret)-1] = '\0';
		return (ret+1);
	}
	return ret;
}

static int eval_logical(char *str)
{
	int ret;

	if (!strcasecmp(str, "true"))
		ret = 1;
	else if (!strcasecmp(str, "false"))
		ret = 0;
	else if (!strcmp(str, "1"))
		ret = 1;
	else if (!strcmp(str, "0"))
		ret = 0;
	else
		ret = -1;

	return ret;
}

/*
 * extract tokens separated by delimi from str. return value is an
 * array of pointers to malloced strings. number of tokens is set to
 * n; note that str will be destroyed by strtok(). Also return value
 * points to static data, that means subsequent call will change the
 * return value.
 */
#define MAXTOKENS 1024
static char **extract_string_tokens(char *str, char *delimi, int *n)
{
	char *token;
	static char *tokens[MAXTOKENS];

	*n = 0;

	for (token = strtok(str, delimi); token != NULL && *n < MAXTOKENS; token = strtok(NULL, delimi))
	{
		tokens[*n] = strdup(token);
		if (tokens[*n] == NULL)
		{
			pool_error("extract_string_tokens: out of memory");
			return NULL;
		}
		pool_debug("extract_string_tokens: token: %s", tokens[*n]);
		(*n)++;
	}
	return tokens;
}
