/* -*-pgsql-c-*- */
/*
 *
 * $Header: /cvsroot/pgpool/pgpool/pool_list.h,v 1.1 2007/02/01 15:31:59 yamaguti Exp $
 *
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Portions Copyright (c) 2003-2007,	PgPool Global Development Group
 * Portions Copyright (c) 2004, PostgreSQL Global Development Group
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
 * pool_list.h.: interface to pool_list.c
 *
 */

#ifndef POOL_LIST_H
#define POOL_LIST_H

#include <stdlib.h>

#define NIL ((List *) NULL)

#define lnext(lc) ((lc)->next)
#define lfirst(lc) ((lc)->data.ptr_value)
#define lfirst_int(lc) ((lc)->data.int_value)
#define foreach(cell, l) \
	for ((cell) = list_head(l); (cell) != NULL; (cell) = lnext(cell))
#define forboth(cell1, list1, cell2, list2) \
	for ((cell1) = list_head(list1), (cell2) = list_head(list2); \
		 (cell1) != NULL && (cell2) != NULL; \
		 (cell1) = lnext(cell1), (cell2) = lnext(cell2))

typedef struct ListCell ListCell;

typedef struct List
{
	int length;
	ListCell *head;
	ListCell *tail;
} List;

struct ListCell
{
	union
	{
		void *ptr_value;
		int int_value;
	} data;
	ListCell *next;
};

#ifdef __GNUC__

static __inline__ ListCell *
list_head(List *l)
{
	return l ? l->head : NULL;
}

static __inline__ ListCell *
list_tail(List *l)
{
	return l ? l->tail : NULL;
}

static __inline__ int
list_length(List *l)
{
	return l ? l->length : 0;
}

#else

extern ListCell * list_head(List *l);
extern ListCell * list_tail(List *l);
extern int list_length(List *l);

#endif /* __GNUC__ */

extern List * lappend(List *list, void *datum);
extern List * lappend_int(List *list, int datum);
extern void list_free(List *list);

#endif /* POOL_LIST_H */
