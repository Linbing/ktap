/*
 * tstring.c - ktap tstring data struction manipulation function
 *
 * Copyright (C) 2012 Jovi Zhang <bookjovi@gmail.com>
 *
 * Author: Jovi Zhang <bookjovi@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/slab.h>
#include "ktap.h"
#else
#include "ktap_types.h"
#endif

#define STRING_MAXSHORTLEN	40

int tstring_cmp(const Tstring *ls, const Tstring *rs)
{
	const char *l = getstr(ls);
	size_t ll = ls->tsv.len;
	const char *r = getstr(rs);
	size_t lr = rs->tsv.len;

	for (;;) {
		int temp = strcmp(l, r);
		if (temp != 0)
			return temp;
		else {  /* strings are equal up to a `\0' */
			size_t len = strlen(l);  /* index of first `\0' in both strings */
			if (len == lr)  /* r is finished? */
				return (len == ll) ? 0 : 1;
			else if (len == ll)  /* l is finished? */
				return -1;

			/* both strings longer than `len'; go on comparing (after the `\0') */
			len++;
			l += len; ll -= len; r += len; lr -= len;
		}
	}
}


/*
 * equality for long strings
 */
int tstring_eqlngstr(Tstring *a, Tstring *b)
{
	size_t len = a->tsv.len;

	return (a == b) || ((len == b->tsv.len) &&
		(memcmp(getstr(a), getstr(b), len) == 0));
}


/*
 * equality for strings
 */
int tstring_eqstr(Tstring *a, Tstring *b)
{
	return (a->tsv.tt == b->tsv.tt) &&
	       (a->tsv.tt == KTAP_TSHRSTR ? eqshrstr(a, b) : tstring_eqlngstr(a, b));
}


#define STRING_HASHLIMIT	5
unsigned int string_hash(const char *str, size_t l, unsigned int seed)
{
	unsigned int h = seed ^ l;
	size_t l1;
	size_t step = (l >> STRING_HASHLIMIT) + 1;

	for (l1 = l; l1 >= step; l1 -= step)
		h = h ^ ((h<<5) + (h>>2) + (u8)(str[l1 - 1]));

	return h;
}


/*
 * resizes the string table
 */
void tstring_resize(ktap_State *ks, int newsize)
{
	int i;
	Stringtable *tb = &G(ks)->strt;

	/* todo: */
	/* cannot resize while GC is traversing strings */
	//ktapc_runtilstate(L, ~bitmask(GCSsweepstring));

	if (newsize > tb->size) {
		ktap_realloc(ks, tb->hash, tb->size, newsize, Gcobject *);

	for (i = tb->size; i < newsize; i++)
		tb->hash[i] = NULL;
	}

	/* rehash */
	for (i = 0; i < tb->size; i++) {
		Gcobject *p = tb->hash[i];
		tb->hash[i] = NULL;

		while (p) {
			Gcobject *next = gch(p)->next;
			unsigned int h = lmod(gco2ts(p)->hash, newsize);

			gch(p)->next = tb->hash[h];
			tb->hash[h] = p;
			//resetoldbit(p);  /* see MOVE OLD rule */
			p = next;
		}
	}

	if (newsize < tb->size) {
		/* shrinking slice must be empty */
		ktap_realloc(ks, tb->hash, tb->size, newsize, Gcobject *);
	}

	tb->size = newsize;
}


/*
 * creates a new string object
 */
static Tstring *createstrobj(ktap_State *ks, const char *str, size_t l,
			     int tag, unsigned int h, Gcobject **list)
{
	Tstring *ts;
	size_t totalsize;  /* total size of TString object */

	totalsize = sizeof(Tstring) + ((l + 1) * sizeof(char));
	ts = &newobject(ks, tag, totalsize, list)->ts;
	ts->tsv.len = l;
	ts->tsv.hash = h;
	ts->tsv.extra = 0;
	memcpy(ts + 1, str, l * sizeof(char));
	((char *)(ts + 1))[l] = '\0';  /* ending 0 */
	return ts;
}


/*
 * creates a new short string, inserting it into string table
 */
static Tstring *newshrstr(ktap_State *ks, const char *str, size_t l,
			  unsigned int h)
{
	Gcobject **list;
	Stringtable *tb = &G(ks)->strt;
	Tstring *s;

	if (tb->nuse >= (int)tb->size)
		tstring_resize(ks, tb->size * 2);  /* too crowded */

	list = &tb->hash[lmod(h, tb->size)];
	s = createstrobj(ks, str, l, KTAP_TSHRSTR, h, list);
	tb->nuse++;
	return s;
}


/*
 * checks whether short string exists and reuses it or creates a new one
 */
static Tstring *internshrstr(ktap_State *ks, const char *str, size_t l)
{
	Gcobject *o;
	global_State *g = G(ks);
	unsigned int h = string_hash(str, l, g->seed);

	for (o = g->strt.hash[lmod(h, g->strt.size)]; o != NULL;
	     o = gch(o)->next) {
		Tstring *ts = rawgco2ts(o);

		if (h == ts->tsv.hash && ts->tsv.len == l &&
		   (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
			return ts;
		}
	}

	return newshrstr(ks, str, l, h);  /* not found; create a new string */
}


/*
 * new string (with explicit length)
 */
Tstring *tstring_newlstr(ktap_State *ks, const char *str, size_t l)
{
	/* short string? */
	if (l <= STRING_MAXSHORTLEN)
		return internshrstr(ks, str, l);
	else
		return createstrobj(ks, str, l, KTAP_TLNGSTR, G(ks)->seed, NULL);
}


/*
 * new zero-terminated string
 */
Tstring *tstring_new(ktap_State *ks, const char *str)
{
	return tstring_newlstr(ks, str, strlen(str));
}

void tstring_freeall(ktap_State *ks)
{
	global_State *g = G(ks);
	int h;

	for (h = 0; h < g->strt.size; h++) {
		Gcobject *o, *next;
		o = g->strt.hash[h];
		while (o) {
			next = gch(o)->next;
			ktap_free(ks, o);
			o = next;
		}
		g->strt.hash[h] = NULL;
	}

	ktap_free(ks, g->strt.hash);
}

void tstring_dump(ktap_State *ks)
{
	Gcobject *o;
	global_State *g = G(ks);
	int h;

	ktap_printf(ks, "tstring dump: strt size: %d, nuse: %d\n", g->strt.size, g->strt.nuse);
	for (h = 0; h < g->strt.size; h++) {
		for (o = g->strt.hash[h]; o != NULL; o = gch(o)->next) {
			Tstring *ts = rawgco2ts(o);
			ktap_printf(ks, "%s [%d]\n", getstr(ts), ts->tsv.len);
		}
	}
}
