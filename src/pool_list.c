/* -*-pgsql-c-*- */
/*
 *
 * $Header: /cvsroot/pgpool/pgpool/pool_list.c,v 1.1 2007/02/01 15:31:59 yamaguti Exp $
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
 * pool_list.c.: Implementation of singly-linked homogeneous lists
 *
 */

#include <string.h>
#include <errno.h>

#include "pool.h"
#include "pool_type.h"
#include "pool_list.h"

static List * new_list(void);
/* static void new_head_cell(List *list); */
static void new_tail_cell(List *list);
static void list_free_private(List *list, bool deep);

#ifndef __GNUC__

ListCell * list_head(List *l)
{
	return l ? l->head : NULL;
}

ListCell * list_tail(List *l)
{
	return l ? l->tail : NULL;
}

int list_length(List *l)
{
	return l ? l->length : 0;
}

#endif /* __GNUC__ */

List * lappend(List *list, void *datum)
{
	if (list == NIL)
		list = new_list();
	else
		new_tail_cell(list);

	lfirst(list->tail) = datum;

	return list;
}

List * lappend_int(List *list, int datum)
{
	if (list == NIL)
		list = new_list();
	else
		new_tail_cell(list);

	lfirst_int(list->tail) = datum;

	return list;
}

static List * new_list(void)
{
	List *new_list;
	ListCell *new_head;

	new_head = (ListCell *)malloc(sizeof(*new_head));
	if (new_head == NULL)
	{
		pool_error("new_list: malloc failed: %s", strerror(errno));
		exit(1);
	}
	new_head->next = NULL;

	new_list = (List *)malloc(sizeof(*new_list));
	if (new_list == NULL)
	{
		pool_error("new_list: malloc failed: %s", strerror(errno));
		exit(1);
	}
	new_list->length = 1;
	new_list->head = new_head;
	new_list->tail = new_head;

	return new_list;
}

#ifdef NOT_USED
static void new_head_cell(List *list)
{
	ListCell *new_head;

	new_head = (ListCell *)malloc(sizeof(*new_head));
	if (new_head == NULL)
	{
		pool_error("new_head_cell: malloc failed: %s", strerror(errno));
		exit(1);
	}
	new_head->next = list->head;

	list->head = new_head;
	list->length++;
}
#endif

static void new_tail_cell(List *list)
{
	ListCell *new_tail;

	new_tail = (ListCell *)malloc(sizeof(*new_tail));
	if (new_tail == NULL)
	{
		pool_error("new_tail_cell: malloc failed: %s", strerror(errno));
		exit(1);
	}
	new_tail->next = NULL;

	list->tail->next = new_tail;
	list->tail = new_tail;
	list->length++;
}

/*
 * free all storage in a list, but not the pointed-to elements
 */
void list_free(List *list)
{
	list_free_private(list, false);
}

/*
 * free all storage in a list, and the pointed-to elements iff deep is true
 */
static void list_free_private(List *list, bool deep)
{
	ListCell *cell;

	cell = list_head(list);
	while (cell != NULL)
	{
		ListCell *tmp = cell;

		cell = lnext(cell);
		if (deep)
			free(lfirst(tmp));
		free(tmp);
	}

	if (list)
		free(list);
}
