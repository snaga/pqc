/*
 * pqc.h
 *
 * Copyright(C) 2010 Uptime Technologies, LLC.
 */
#ifndef PQC_H
#define PQC_H

#include <libmemcached/memcached.h>

#define PQC_MAX_KEY   MEMCACHED_MAX_KEY
#define PQC_MAX_VALUE 8192
#define PQC_MAX_CACHELEN (1024*1024)

extern int IsQueryCacheEnabled;
extern int UseQueryCache;

extern int FoundInQueryCache;

extern int pqc_init(int);

extern int pqc_check_cache_avail(POOL_CONNECTION *front, const char *);

extern int pqc_push_current_query(const char *);
extern char *pqc_pop_current_query(void);

extern void pqc_buf_init(void);
extern int pqc_buf_add(const char *, size_t);
extern int pqc_buf_len(void);
extern char *pqc_buf_get(void);

extern int pqc_set_cache(POOL_CONNECTION *, const char *, const char *, size_t);
extern int pqc_get_cache(POOL_CONNECTION *, const char *, char **, size_t *);

extern int pqc_check_cache_hint(const char *, int *, int *, int *);

extern int pqc_send_message(POOL_CONNECTION *, char, int, const char *);
extern int pqc_send_cached_messages(POOL_CONNECTION *, const char *, int);

extern int pqc_destroy(void);

#endif /* PQC_H */
