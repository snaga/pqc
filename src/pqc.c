/*
 * pqc.c
 *
 * Copyright(C) 2010 Uptime Technologies, LLC.
 */
#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <libmemcached/memcached.h>

#include "pool.h"
#include "pqc.h"

#define MEMCACHED_PID_FILE "/tmp/memcached.pid"

static memcached_st *memc = NULL;
static memcached_server_st *servers = NULL;
static memcached_return rc;

static char cache_key[PQC_MAX_KEY];

static size_t buflen = 0;
static size_t bufsize = 0;
static char *buf = NULL;

static int pqc_start_memcached(int);
static int pqc_stop_memcached();
static char *encode_key(const char *, char *, size_t);

/*
 * State flags combination:
 *
 * cache:off       IsQueryCacheEnabled = 0, UseQueryCache = 0/1 (no effort)
 * cache:on        IsQueryCacheEnabled = 1, UseQueryCache = 1
 * cache:refresh   IsQueryCacheEnabled = 1, UseQueryCache = 0
 */
int IsQueryCacheEnabled = 1;
int UseQueryCache = 1;

int FoundInQueryCache = 0;

int
pqc_init(int run_as_daemon)
{
  pqc_start_memcached(run_as_daemon);

  if ( memc!=NULL )
  {
    /* FIXME: need to warn initializing twice (or more). */
    return 1;
  }

  memc = memcached_create(NULL);
  if ( memc==NULL )
  {
    return 0;
  }

  servers = memcached_server_list_append(NULL, "127.0.0.1", 11211, &rc);
  if (rc != MEMCACHED_SUCCESS)
  {
    pool_debug("pqc_init: memcached_server_list_append() failed.");
  }

  rc = memcached_server_push(memc, servers);
  if (rc != MEMCACHED_SUCCESS)
  {
    pool_debug("pqc_init: memcached_server_push() failed.");
  }

  rc = memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, 0);
  if (rc != MEMCACHED_SUCCESS) {
    pool_error("pqc_init: %s", memcached_strerror(memc, rc));
    return 0;
  }

  pool_debug("pqc_init: memcached has been successfully initialized.");
  pool_debug("pqc_init: Query Cache Mode = %d", pool_config.query_cache_mode);

  return 1;
}

static int
pqc_start_memcached(int run_as_daemon)
{
  char *argv[16];
  int argc = 0;
  pid_t pid;

  argv[argc++] = pool_config.memcached_bin;
  argv[argc++] = "-l";
  argv[argc++] = "127.0.0.1";
  argv[argc++] = "-p";
  argv[argc++] = "11211";
  argv[argc++] = "-vv";

  if (run_as_daemon)
  {
    argv[argc++] = "-d";
    argv[argc++] = "-P";
    argv[argc++] = MEMCACHED_PID_FILE;
  }

  argv[argc++] = NULL;

  pid = fork();

  if (pid < 0)
  {
    pool_error("fork() failed.");
    return 0;
  }

  if (pid == 0)
  {
    execv(pool_config.memcached_bin, argv);

    /* never reach here. */
    pool_error("Failed to start \"%s\".", pool_config.memcached_bin);
    exit(-1);
  }

  pool_debug("pqc_start_memcached: memcached launched.");

  return 1;
}

static int
pqc_stop_memcached()
{
  pid_t pid;
  FILE *fp;

  fp = fopen(MEMCACHED_PID_FILE, "r");
  if (fp == NULL)
  {
    pool_error("PID file can't be opened. - %s", MEMCACHED_PID_FILE);
    return 0;
  }

  fscanf(fp, "%d", &pid);
  fclose(fp);

  pool_debug("Sending SIGTERM to PID %d...", pid);

  kill(pid, SIGTERM);

  fprintf(stderr, "stop request sent to memcached. waiting for termination...");

  while (kill(pid, 0) == 0)
  {
    fprintf(stderr, ".");
    sleep(1);
  }
  fprintf(stderr, "done.\n");

  return 1;
}

int
pqc_check_cache_avail(POOL_CONNECTION *frontend, const char *query)
{
  char buf[PQC_MAX_VALUE];
  size_t len;

  if (!UseQueryCache)
    return 0;

  memset(buf, 0, sizeof(buf));

  pool_debug("pqc_check_cache_avail: %s", query);

  if ( !pqc_get_cache(frontend, query, (char **)&(buf[0]), &len) )
    return 0;

  return 1;
}

int
pqc_push_current_query(const char *query)
{
  strncpy(cache_key, query, sizeof(cache_key));

  return 1;
}

char *
pqc_pop_current_query(void)
{
  return cache_key;
}

void
pqc_buf_init(void)
{
  buflen = 0;
  if (buf == NULL) {
    bufsize = PQC_MAX_VALUE;
    buf = (char *)malloc(bufsize);
  }
  memset(buf, 0, bufsize);
}

int
pqc_buf_add(const char *s, size_t len)
{
  int pos=0;

  if ( !IsQueryCacheEnabled )
    return 0;

  /* the buffer is exceeded, and disabled. */
  if ( buf==NULL || buflen<0 )
    return 0;

  /*
   * If a buffer size is exceeded, free the buffer and set -1 to its length
   * to disable the buffer.
   */
  if ( buflen + len >= PQC_MAX_CACHELEN )
  {
    pool_log("The result set too large to store into the cache. (len=%d)", buflen+len);
    free(buf);
    buf = NULL;
    buflen = -1;
    return 0;
  }

  // Check if we need to increase the buffer size
  if (buflen + len >= bufsize) {
    // Allocate enough memory to complete the copy plus PQC_MAX_VALUE buffer space
    size_t newbufsize = ( (buflen + len) / PQC_MAX_VALUE + 1 ) * PQC_MAX_VALUE;
    pool_debug("pqc_buf_add: realloc buf bufsize=%d newbufsize=%d", bufsize, newbufsize);
    bufsize = newbufsize;
    buf = (char *)realloc(buf, bufsize);
  }

  for (pos=0 ; pos<len ; pos++)
  {
    buf[buflen+pos] = s[pos];
  }
  buflen += pos;

  pool_debug("pqc_buf_add: len=%d, total=%d bufsize=%d", len, buflen, bufsize);

  return buflen;
}

int
pqc_buf_len(void)
{
  return buflen;
}

char *
pqc_buf_get(void)
{
  return buf;
}


static char *
encode_key(const char *s, char *buf, size_t buflen)
{
  int i;

  memset(buf, 0, buflen);

  /* replace ' ' to '_'. */
  for (i=0 ; i<strlen(s) ; i++)
  {
    if (i>=buflen)
      break;

    if ( s[i]==' ' || iscntrl(s[i]) )
      buf[i] = '_';
    else
      buf[i] = s[i];
  }

  pool_debug("encode_key: `%s' -> `%s'", s, buf);

  return buf;
}

static void
dump_cache_data(const char *data, size_t len)
{
  int i;

  pool_debug("dump_cache_data: len = %d", len);

  for (i=0 ; (len-i)>=10 ; i+=10) {
    pool_debug("dump_cache_data: %d(%c) %d(%c) %d(%c) %d(%c) %d(%c) %d(%c) %d(%c) %d(%c) %d(%c) %d(%c) ",
	       data[i+0], data[i+0],
	       data[i+1], data[i+1],
	       data[i+2], data[i+2],
	       data[i+3], data[i+3],
	       data[i+4], data[i+4],
	       data[i+5], data[i+5],
	       data[i+6], data[i+6],
	       data[i+7], data[i+7],
	       data[i+8], data[i+8],
	       data[i+9], data[i+9]);
  }

  for (; i<len ; i++) {
    pool_debug("dump_cache_data: %d(%c) ", data[i], data[i]);
  }
}

int
pqc_set_cache(POOL_CONNECTION *frontend, const char *query, const char *data, size_t datalen)
{
  char tmpkey[PQC_MAX_KEY];

  if ( !IsQueryCacheEnabled )
    return 0;

  if ( strlen(query)<=0 )
    return 0;

  pool_debug("pqc_set_cache: Query=%s", query);
  dump_cache_data(data, datalen);

  if ( frontend!=NULL && frontend->database!=NULL )
  {
    char tmp[PQC_MAX_KEY];

    snprintf(tmp, sizeof(tmp), "%s %s", frontend->database, query);
    encode_key(tmp, tmpkey, sizeof(tmpkey));
  }
  else
  {
    encode_key(query, tmpkey, sizeof(tmpkey));
  }

  rc = memcached_set(memc, tmpkey, strlen(tmpkey), data, datalen, pool_config.query_cache_expiration, 0);

  if (rc != MEMCACHED_SUCCESS)
  {
    pool_error("pqc_set_cache: %s", memcached_strerror(memc, rc));
    return 0;
  }

  pool_debug("pqc_set_cache: succeeded.");

  return 1;
}

int
pqc_get_cache(POOL_CONNECTION *frontend, const char *query, char **buf, size_t *len)
{
  uint32_t flags2;
  char *ptr;
  char tmpkey[PQC_MAX_KEY];

  if ( !IsQueryCacheEnabled )
    return 0;

  if ( strlen(query)<=0 )
    return 0;

  if ( frontend!=NULL && frontend->database!=NULL )
  {
    char tmp[PQC_MAX_KEY];

    snprintf(tmp, sizeof(tmp), "%s %s", frontend->database, query);
    encode_key(tmp, tmpkey, sizeof(tmpkey));
  }
  else
  {
    encode_key(query, tmpkey, sizeof(tmpkey));
  }

  ptr = memcached_get(memc, tmpkey, strlen(tmpkey), len, &flags2, &rc);

  if (rc != MEMCACHED_SUCCESS)
  {
    pool_error("pqc_get_cache: %s", memcached_strerror(memc, rc));
    return 0;
  }

  memcpy(buf, ptr, *len);
  free(ptr);

  pool_debug("pqc_get_cache: Query=%s", query);
  dump_cache_data(buf, *len);

  return 1;
}

int
pqc_check_cache_hint(const char *stmt, int *enabled, int *use, int *hint_offset)
{
  /*
   * Need to check some hint comments, like `cache:refresh',
   * beginning with the statement, and toggle the cache flags.
   */
  if (strncmp("/* cache:off */", stmt, 15) == 0)
  {
    /* This hint doesn't have any effort under the Passive-cache mode. */
    *enabled = false;
    *use = false;
    *hint_offset = 15;

    pool_debug("Query Cache Hint: cache:off");
  }
  else if (strncmp("/* cache:on */", stmt, 14) == 0)
  {
    /* This hint doesn't have any effort on the Active-cache mode. */
    *enabled = true;
    *use = true;
    *hint_offset = 14;

    pool_debug("Query Cache Hint: cache:on");
  }
  else if (strncmp("/* cache:refresh */", stmt, 19) == 0)
  {
    *enabled = true;
    *use = false;
    *hint_offset = 19;

    pool_debug("Query Cache Hint: cache:refresh");
  }

  return *hint_offset;
}

int
pqc_send_message(POOL_CONNECTION *conn, char kind, int len, const char *data)
{

  pool_debug("pqc_send_message: kind=%c, len=%d, data=%p", kind, len, data);

  pool_write(conn, &kind, 1);

  len = htonl(len);   /* convert to network byte order */
  pool_write(conn, &len, sizeof(len));

  len = ntohl(len);   /* convert from network byte order */
  pool_write(conn, (void *)data, len-sizeof(len));

  if (pool_flush(conn))
    return 0;

  return 1;
}

int
pqc_send_cached_messages(POOL_CONNECTION *frontend, const char *qcache, int qcachelen)
{
  int msg = 0;
  int i = 0;
  int is_prepared_stmt = 0;

  while (i < qcachelen)
  {
    char tmpkind;
    int tmplen;
    char tmpbuf[PQC_MAX_VALUE];
    
    tmpkind = qcache[i];
    i += 1;
    
    memcpy(&tmplen, qcache+i, sizeof(tmplen)); /* in network byte order */
    i += sizeof(tmplen);

    tmplen = ntohl(tmplen);
    
    memcpy(tmpbuf, qcache+i, tmplen - sizeof(tmplen));
    i += tmplen - sizeof(tmplen);
    
    /* No need to cache PARSE and BIND responses. */
    if (tmpkind == '1' || tmpkind == '2')
    {
      is_prepared_stmt = 1;
      continue;
    }

    /*
     * In the prepared statement execution, there is no need to send
     * 'T' response to the frontend. A bit ad-hoc hack.
     */
    if (is_prepared_stmt && tmpkind == 'T')
      continue;

    pqc_send_message(frontend, tmpkind, tmplen, tmpbuf);

    msg++;
  }

  return msg;
}

int
pqc_destroy(void)
{
  pqc_stop_memcached();

  if ( memc==NULL )
  {
    /* FIXME: need to warn destroying a not initialized context. */
    return 1;
  }

  memcached_free(memc);

  pool_debug("pqc_destroy: memcached has been destroyed.");

  return 1;
}
