/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Olson.
 *
 * %sccs.include.redist.c%
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)bt_split.c	5.8 (Berkeley) %G%";
#endif /* LIBC_SCCS and not lint */

#include <sys/types.h>

#define	__DBINTERFACE_PRIVATE
#include <db.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btree.h"

static int	 bt_preserve __P((BTREE *, pgno_t));
static PAGE	*bt_psplit __P((BTREE *, PAGE *, PAGE *, PAGE *, int *));
static PAGE	*bt_page __P((BTREE *, PAGE *, PAGE **, PAGE **, int *));
static PAGE	*bt_root __P((BTREE *, PAGE *, PAGE **, PAGE **, int *));
static int	 bt_rroot __P((BTREE *, PAGE *, PAGE *, PAGE *));
static int	 bt_broot __P((BTREE *, PAGE *, PAGE *, PAGE *));
static recno_t	 rec_total __P((PAGE *));

#ifdef STATISTICS
u_long	bt_rootsplit, bt_split, bt_sortsplit, bt_pfxsaved;
#endif

/*
 * __BT_SPLIT -- Split the tree.
 *
 * Parameters:
 *	t:	tree
 *	h:	page to split
 *	key:	key to insert
 *	data:	data to insert
 *	flags:	BIGKEY/BIGDATA flags
 *	nbytes:	length of insertion
 *	skip:	index to leave open
 *
 * Returns:
 *	RET_ERROR, RET_SUCCESS
 */
int
__bt_split(t, h, key, data, flags, nbytes, skip)
	BTREE *t;
	PAGE *h;
	const DBT *key, *data;
	u_long flags;
	size_t nbytes;
	int skip;
{
	BINTERNAL *bi;
	BLEAF *bl;
	DBT a, b;
	EPGNO *parent;
	PAGE *l, *r, *lchild, *rchild;
	index_t nxtindex;
	size_t nksize;
	int nosplit;
	char *dest;

	/*
	 * Split the page into two pages, l and r.  The split routines return
	 * a pointer to the page into which the key should be inserted and skip
	 * set to the offset which should be used.  Additionally, l and r are
	 * pinned.
	 */
	h = h->pgno == P_ROOT ?
	    bt_root(t, h, &l, &r, &skip) : bt_page(t, h, &l, &r, &skip);
	if (h == NULL)
		return (RET_ERROR);

	/*
	 * Grab the space and insert the [rb]leaf structure.  Always a [rb]leaf
	 * structure since key inserts always cause a leaf page to split first.
	 */
	h->linp[skip] = h->upper -= nbytes;
	dest = (char *)h + h->upper;
	if (ISSET(t, BTF_RECNO))
		WR_RLEAF(dest, data, flags)
	else
		WR_BLEAF(dest, key, data, flags)

	/*
	 * Now we walk the parent page stack -- a LIFO stack of the pages that
	 * were traversed when we searched for the page that split.  Each stack
	 * entry is a page number and a page index offset.  The offset is for
	 * the page traversed on the search.  We've just split a page, so we
	 * have to insert a new key into the parent page.
	 *
	 * If the insert into the parent page causes it to split, may have to
	 * continue splitting all the way up the tree.  We stop if the root
	 * splits or the page inserted into didn't have to split to hold the
	 * new key.  Some algorithms replace the key for the old page as well
	 * as the new page.  We don't, as there's no reason to believe that the
	 * first key on the old page is any better than the key we have, and,
	 * in the case of a key being placed at index 0 causing the split, the
	 * key is unavailable.
	 *
	 * There are a maximum of 5 pages pinned at any time.  We keep the left
	 * and right pages pinned while working on the parent.   The 5 are the
	 * two children, left parent and right parent (when the parent splits)
	 * and the root page or the overflow key page when calling bt_preserve.
	 * This code must make sure that all pins are released other than the
	 * root page or overflow page which is unlocked elsewhere.
	 */
	for (nosplit = 0; (parent = BT_POP(t)) != NULL;) {
		lchild = l;
		rchild = r;

		/* Get the parent page. */
		if ((h = mpool_get(t->bt_mp, parent->pgno, 0)) == NULL)
			goto err2;

	 	/* The new key goes ONE AFTER the index. */
		skip = parent->index + 1;

		/*
		 * Calculate the space needed on the parent page.
		 *
		 * Space hack when inserting into BINTERNAL pages.  Only need to
		 * retain the number of bytes that will distinguish between the
		 * new entry and the LAST entry on the page to its left.  If the
		 * keys compare equal, retain the entire key.  Note, we don't
		 * touch overflow keys and the entire key must be retained for
		 * the next-to-leftmost key on the leftmost page of each level,
		 * or the search will fail.
		 */
		switch (rchild->flags & P_TYPE) {
		case P_BINTERNAL:
			bi = GETBINTERNAL(rchild, 0);
			nbytes = NBINTERNAL(bi->ksize);
			if (t->bt_pfx && (h->prevpg != P_INVALID || skip > 1) &&
			    !(bi->flags & P_BIGKEY)) {
				BINTERNAL *tbi;
				tbi =
				    GETBINTERNAL(lchild, NEXTINDEX(lchild) - 1);
				a.size = tbi->ksize;
				a.data = tbi->bytes;
				b.size = bi->ksize;
				b.data = bi->bytes;
				goto prefix;
			} else
				nksize = 0;
			break;
		case P_BLEAF:
			bl = GETBLEAF(rchild, 0);
			nbytes = NBINTERNAL(bl->ksize);
			if (t->bt_pfx && (h->prevpg != P_INVALID || skip > 1) &&
			    !(bl->flags & P_BIGKEY)) {
				BLEAF *tbl;
				size_t n;

				tbl = GETBLEAF(lchild, NEXTINDEX(lchild) - 1);
				a.size = tbl->ksize;
				a.data = tbl->bytes;
				b.size = bl->ksize;
				b.data = bl->bytes;
prefix:				nksize = t->bt_pfx(&a, &b);
				n = NBINTERNAL(nksize);
				if (n < nbytes) {
#ifdef STATISTICS
					bt_pfxsaved += nbytes - n;
#endif
					nbytes = n;
				} else
					nksize = 0;
			} else
				nksize = 0;
			break;
		case P_RINTERNAL:
		case P_RLEAF:
			nbytes = NRINTERNAL;
			break;
		default:
			abort();
		}

		/* Split the parent page if necessary or shift the indices. */
		if (h->upper - h->lower < nbytes + sizeof(index_t)) {
			h = h->pgno == P_ROOT ?
			    bt_root(t, h, &l, &r, &skip) :
			    bt_page(t, h, &l, &r, &skip);
			if (h == NULL)
				goto err1;
		} else {
			if (skip < (nxtindex = NEXTINDEX(h)))
				bcopy(h->linp + skip, h->linp + skip + 1,
				    (nxtindex - skip) * sizeof(index_t));
			h->lower += sizeof(index_t);
			nosplit = 1;
		}

		/* Insert the key into the parent page. */
		switch(rchild->flags & P_TYPE) {
		case P_BINTERNAL:
			h->linp[skip] = h->upper -= nbytes;
			dest = (char *)h + h->linp[skip];
			bcopy(bi, dest, nbytes);
			if (nksize)
				((BINTERNAL *)dest)->ksize = nksize;
			((BINTERNAL *)dest)->pgno = rchild->pgno;
			break;
		case P_BLEAF:
			h->linp[skip] = h->upper -= nbytes;
			dest = (char *)h + h->linp[skip];
			WR_BINTERNAL(dest, nksize ? nksize : bl->ksize,
			    rchild->pgno, bl->flags & P_BIGKEY);
			bcopy(bl->bytes, dest, nksize ? nksize : bl->ksize);
			if (bl->flags & P_BIGKEY &&
			    bt_preserve(t, *(pgno_t *)bl->bytes) == RET_ERROR)
				goto err1;
			break;
		case P_RINTERNAL:
			/* Update both left and right page counts. */
			h->linp[skip] = h->upper -= nbytes;
			dest = (char *)h + h->linp[skip];
			((RINTERNAL *)dest)->nrecs = rec_total(rchild);
			((RINTERNAL *)dest)->pgno = rchild->pgno;
			dest = (char *)h + h->linp[skip - 1];
			((RINTERNAL *)dest)->nrecs = rec_total(lchild);
			((RINTERNAL *)dest)->pgno = lchild->pgno;
			break;
		case P_RLEAF:
			/* Update both left and right page counts. */
			h->linp[skip] = h->upper -= nbytes;
			dest = (char *)h + h->linp[skip];
			((RINTERNAL *)dest)->nrecs = NEXTINDEX(rchild);
			((RINTERNAL *)dest)->pgno = rchild->pgno;
			dest = (char *)h + h->linp[skip - 1];
			((RINTERNAL *)dest)->nrecs = NEXTINDEX(lchild);
			((RINTERNAL *)dest)->pgno = lchild->pgno;
			break;
		default:
			abort();
		}

		/* Unpin the held pages. */
		if (nosplit) {
			mpool_put(t->bt_mp, h, MPOOL_DIRTY);
			break;
		}
		mpool_put(t->bt_mp, lchild, MPOOL_DIRTY);
		mpool_put(t->bt_mp, rchild, MPOOL_DIRTY);
	}

	/* Unpin the held pages. */
	mpool_put(t->bt_mp, l, MPOOL_DIRTY);
	mpool_put(t->bt_mp, r, MPOOL_DIRTY);

	/* Clear any pages left on the stack. */
	BT_CLR(t);
	return (RET_SUCCESS);

	/*
	 * If something fails in the above loop we were already walking back
	 * up the tree and the tree is now inconsistent.  Nothing much we can
	 * do about it but release any memory we're holding.
	 */
err1:	mpool_put(t->bt_mp, lchild, MPOOL_DIRTY);
	mpool_put(t->bt_mp, rchild, MPOOL_DIRTY);

err2:	mpool_put(t->bt_mp, l, 0);
	mpool_put(t->bt_mp, r, 0);
	__dbpanic(t->bt_dbp);
	return (RET_ERROR);
}

/*
 * BT_PAGE -- Split a non-root page of a btree.
 *
 * Parameters:
 *	t:	tree
 *	h:	root page
 *	lp:	pointer to left page pointer
 *	rp:	pointer to right page pointer
 *	skip:	pointer to index to leave open
 *
 * Returns:
 *	Pointer to page in which to insert or NULL on error.
 */
static PAGE *
bt_page(t, h, lp, rp, skip)
	BTREE *t;
	PAGE *h, **lp, **rp;
	int *skip;
{
	PAGE *l, *r, *tp;
	pgno_t npg;

#ifdef STATISTICS
	++bt_split;
#endif
	/* Put the new right page for the split into place. */
	if ((r = __bt_new(t, &npg)) == NULL)
		return (NULL);
	r->pgno = npg;
	r->lower = BTDATAOFF;
	r->upper = t->bt_psize;
	r->nextpg = h->nextpg;
	r->prevpg = h->pgno;
	r->flags = h->flags & P_TYPE;

	/*
	 * If we're splitting the last page on a level because we're appending
	 * a key to it (skip is NEXTINDEX()), it's likely that the data is
	 * sorted.  Adding an empty page on the side of the level is less work
	 * and can push the fill factor much higher than normal.  If we're
	 * wrong it's no big deal, we'll just do the split the right way next
	 * time.  It may look like it's equally easy to do a similar hack for
	 * reverse sorted data, that is, split the tree left, but it's not.
	 * Don't even try.
	 */
	if (h->nextpg == P_INVALID && *skip == NEXTINDEX(h)) {
#ifdef STATISTICS
		++bt_sortsplit;
#endif
		h->nextpg = r->pgno;
		r->lower = BTDATAOFF + sizeof(index_t);
		*skip = 0;
		*lp = h;
		*rp = r;
		return (r);
	}

	/* Put the new left page for the split into place. */
	if ((l = malloc(t->bt_psize)) == NULL) {
		mpool_put(t->bt_mp, r, 0);
		return (NULL);
	}
	l->pgno = h->pgno;
	l->nextpg = r->pgno;
	l->prevpg = h->prevpg;
	l->lower = BTDATAOFF;
	l->upper = t->bt_psize;
	l->flags = h->flags & P_TYPE;

	/* Fix up the previous pointer of the page after the split page. */
	if (h->nextpg != P_INVALID) {
		if ((tp = mpool_get(t->bt_mp, h->nextpg, 0)) == NULL) {
			free(l);
			/* XXX mpool_free(t->bt_mp, r->pgno); */
			return (NULL);
		}
		tp->prevpg = r->pgno;
		mpool_put(t->bt_mp, tp, 0);
	}

	/*
	 * Split right.  The key/data pairs aren't sorted in the btree page so
	 * it's simpler to copy the data from the split page onto two new pages
	 * instead of copying half the data to the right page and compacting
	 * the left page in place.  Since the left page can't change, we have
	 * to swap the original and the allocated left page after the split.
	 */
	tp = bt_psplit(t, h, l, r, skip);

	/* Move the new left page onto the old left page. */
	bcopy(l, h, t->bt_psize);
	if (tp == l)
		tp = h;
	free(l);

	*lp = h;
	*rp = r;
	return (tp);
}

/*
 * BT_ROOT -- Split the root page of a btree.
 *
 * Parameters:
 *	t:	tree
 *	h:	root page
 *	lp:	pointer to left page pointer
 *	rp:	pointer to right page pointer
 *	skip:	pointer to index to leave open
 *
 * Returns:
 *	Pointer to page in which to insert or NULL on error.
 */
static PAGE *
bt_root(t, h, lp, rp, skip)
	BTREE *t;
	PAGE *h, **lp, **rp;
	int *skip;
{
	PAGE *l, *r, *tp;
	pgno_t lnpg, rnpg;

#ifdef STATISTICS
	++bt_split;
	++bt_rootsplit;
#endif
	/* Put the new left and right pages for the split into place. */
	if ((l = __bt_new(t, &lnpg)) == NULL ||
	    (r = __bt_new(t, &rnpg)) == NULL)
		return (NULL);
	l->pgno = lnpg;
	r->pgno = rnpg;
	l->nextpg = r->pgno;
	r->prevpg = l->pgno;
	l->prevpg = r->nextpg = P_INVALID;
	l->lower = r->lower = BTDATAOFF;
	l->upper = r->upper = t->bt_psize;
	l->flags = r->flags = h->flags & P_TYPE;

	/* Split the root page. */
	tp = bt_psplit(t, h, l, r, skip);

	/* Make the root page look right. */
	if ((ISSET(t, BTF_RECNO) ?
	    bt_rroot(t, h, l, r) : bt_broot(t, h, l, r)) == RET_ERROR)
		return (NULL);

	*lp = l;
	*rp = r;
	return (tp);
}

/*
 * BT_RROOT -- Fix up the recno root page after the split.
 *
 * Parameters:
 *	t:	tree
 *	h:	root page
 *
 * Returns:
 *	RET_ERROR, RET_SUCCESS
 */
static int
bt_rroot(t, h, l, r)
	BTREE *t;
	PAGE *h, *l, *r;
{
	char *dest;

	/* Insert the left and right keys, set the header information. */
	h->linp[0] = h->upper = t->bt_psize - NRINTERNAL;
	dest = (char *)h + h->upper;
	WR_RINTERNAL(dest,
	    l->flags & P_RLEAF ? NEXTINDEX(l) : rec_total(l), l->pgno);

	h->linp[1] = h->upper -= NRINTERNAL;
	dest = (char *)h + h->upper;
	WR_RINTERNAL(dest,
	    r->flags & P_RLEAF ? NEXTINDEX(r) : rec_total(r), r->pgno);

	h->lower = BTDATAOFF + 2 * sizeof(index_t);

	/* Unpin the root page, set to recno internal page. */
	h->flags &= ~P_TYPE;
	h->flags |= P_RINTERNAL;
	mpool_put(t->bt_mp, h, MPOOL_DIRTY);

	return (RET_SUCCESS);
}

/*
 * BT_BROOT -- Fix up the btree root page after the split.
 *
 * Parameters:
 *	t:	tree
 *	h:	root page
 *
 * Returns:
 *	RET_ERROR, RET_SUCCESS
 */
static int
bt_broot(t, h, l, r)
	BTREE *t;
	PAGE *h, *l, *r;
{
	BINTERNAL *bi;
	BLEAF *bl;
	size_t nbytes;
	char *dest;

	/*
	 * If the root page was a leaf page, change it into an internal page.
	 * We copy the key we split on (but not the key's data, in the case of
	 * a leaf page) to the new root page.
	 *
	 * The btree comparison code guarantees that the left-most key on any
	 * level of the tree is never used, so it doesn't need to be filled
	 * in.  (This is not just convenience -- if the insert index is 0, we
	 * don't *have* a key to fill in.)  The right key is available because
	 * the split code guarantees not to split on the skipped index.
	 */
	nbytes = NBINTERNAL(0);
	h->linp[0] = h->upper = t->bt_psize - nbytes;
	dest = (char *)h + h->upper;
	WR_BINTERNAL(dest, 0, l->pgno, 0);

	switch(h->flags & P_TYPE) {
	case P_BLEAF:
		bl = GETBLEAF(r, 0);
		nbytes = NBINTERNAL(bl->ksize);
		h->linp[1] = h->upper -= nbytes;
		dest = (char *)h + h->upper;
		WR_BINTERNAL(dest, bl->ksize, r->pgno, 0);
		bcopy(bl->bytes, dest, bl->ksize);

		/*
		 * If the key is on an overflow page, mark the overflow chain
		 * so it isn't deleted when the leaf copy of the key is deleted.
		 */
		if (bl->flags & P_BIGKEY &&
		    bt_preserve(t, *(pgno_t *)bl->bytes) == RET_ERROR)
			return (RET_ERROR);
		break;
	case P_BINTERNAL:
		bi = GETBINTERNAL(r, 0);
		nbytes = NBINTERNAL(bi->ksize);
		h->linp[1] = h->upper -= nbytes;
		dest = (char *)h + h->upper;
		bcopy(bi, dest, nbytes);
		((BINTERNAL *)dest)->pgno = r->pgno;
		break;
	default:
		abort();
	}
	h->lower = BTDATAOFF + 2 * sizeof(index_t);

	/* Unpin the root page, set to btree internal page. */
	h->flags &= ~P_TYPE;
	h->flags |= P_BINTERNAL;
	mpool_put(t->bt_mp, h, MPOOL_DIRTY);

	return (RET_SUCCESS);
}

/*
 * BT_PSPLIT -- Do the real work of splitting the page.
 *
 * Parameters:
 *	t:	tree
 *	h:	page to be split
 *	l:	page to put lower half of data
 *	r:	page to put upper half of data
 *	skip:	pointer to index to leave open
 *
 * Returns:
 *	Pointer to page in which to insert.
 */
static PAGE *
bt_psplit(t, h, l, r, pskip)
	BTREE *t;
	PAGE *h, *l, *r;
	int *pskip;
{
	BINTERNAL *bi;
	BLEAF *bl;
	RLEAF *rl;
	EPGNO *c;
	PAGE *rval;
	index_t half, skip;
	size_t nbytes;
	void *src;
	int bigkeycnt, isbigkey, nxt, off, top;

	/*
	 * Split the data to the left and right pages. Leave the skip index
	 * open and guarantee that the split doesn't happen on that index (the
	 * right key must be available for the parent page).  Additionally,
	 * make some effort not to split on an overflow key.  This makes it
	 * faster to process internal pages and can save space since overflow
	 * keys used by internal pages are never deleted.
	 */
	bigkeycnt = 0;
	skip = *pskip;
	half = (t->bt_psize - BTDATAOFF) / 2;
	for (nxt = off = 0, top = NEXTINDEX(h); nxt < top; ++off) {
		if (skip == off)
			continue;
		switch (h->flags & P_TYPE) {
		case P_BINTERNAL:
			src = bi = GETBINTERNAL(h, nxt);
			nbytes = NBINTERNAL(bi->ksize);
			isbigkey = bi->flags & P_BIGKEY;
			break;
		case P_BLEAF:
			src = bl = GETBLEAF(h, nxt);
			nbytes = NBLEAF(bl);
			isbigkey = bl->flags & P_BIGKEY;
			break;
		case P_RINTERNAL:
			src = GETRINTERNAL(h, nxt);
			nbytes = NRINTERNAL;
			isbigkey = 0;
			break;
		case P_RLEAF:
			src = rl = GETRLEAF(h, nxt);
			nbytes = NRLEAF(rl);
			isbigkey = 0;
			break;
		default:
			abort();
		}
		++nxt;
		l->linp[off] = l->upper -= nbytes;
		bcopy(src, (char *)l + l->upper, nbytes);

		/* There's no empirical justification for the '3'. */
		if (half < nbytes) {
			if (skip != off + 1)
				if (!isbigkey || bigkeycnt == 3)
					break;
				else
					++bigkeycnt;
		} else
			half -= nbytes;
	}
	l->lower += (off + 1) * sizeof(index_t);

	/*
	 * If splitting the page that the cursor was on, the cursor has to be
	 * adjusted to point to the same record as before the split.  If the
	 * skipped slot and the cursor are both on the left page and the cursor
	 * is on or past the skipped slot, the cursor is incremented by one.
	 * If the skipped slot and the cursor are both on the right page and
	 * the cursor is on or past the skipped slot, the cursor is incremented
	 * by one.  If the skipped slot and the cursor aren't on the same page,
	 * the cursor isn't changed.  Regardless of the relationship of the
	 * skipped slot and the cursor, if the cursor is on the right page it
	 * is decremented by the number of records split to the left page.
	 *
	 * Don't bother checking for the BTF_SEQINIT flag, the page number will
	 * be P_INVALID.
	 */
	c = &t->bt_bcursor;
	if (c->pgno == h->pgno)
		if (c->index < off) {			/* left page */
			c->pgno = l->pgno;
			if (c->index >= skip)
				++c->index;
		} else {				/* right page */
			c->pgno = r->pgno;
			if (c->index >= skip && skip > off)
				++c->index;
			c->index -= off;
		}

	/*
	 * Decide which page to return, and adjust the skip index if the
	 * to-be-inserted-upon page has changed.
	 */
	if (skip > off) {
		rval = r;
		*pskip -= off + 1;
	} else
		rval = l;

	for (off = 0; nxt < top; ++off) {
		if (skip == nxt) {
			skip = 0;
			continue;
		}
		switch (h->flags & P_TYPE) {
		case P_BINTERNAL:
			src = bi = GETBINTERNAL(h, nxt);
			nbytes = NBINTERNAL(bi->ksize);
			break;
		case P_BLEAF:
			src = bl = GETBLEAF(h, nxt);
			nbytes = NBLEAF(bl);
			break;
		case P_RINTERNAL:
			src = GETRINTERNAL(h, nxt);
			nbytes = NRINTERNAL;
			break;
		case P_RLEAF:
			src = rl = GETRLEAF(h, nxt);
			nbytes = NRLEAF(rl);
			break;
		default:
			abort();
		}
		++nxt;
		r->linp[off] = r->upper -= nbytes;
		bcopy(src, (char *)r + r->upper, nbytes);
	}
	r->lower += off * sizeof(index_t);

	/* If the key is being appended to the page, adjust the index. */
	if (skip == top)
		r->lower += sizeof(index_t);

	return (rval);
}

/*
 * BT_PRESERVE -- Mark a chain of pages as used by an internal node.
 *
 * Chains of indirect blocks pointed to by leaf nodes get reclaimed when the
 * record that references them gets deleted.  Chains pointed to by internal
 * pages never get deleted.  This routine marks a chain as pointed to by an
 * internal page.
 *
 * Parameters:
 *	t:	tree
 *	pg:	page number of first page in the chain.
 *
 * Returns:
 *	RET_SUCCESS, RET_ERROR.
 */
static int
bt_preserve(t, pg)
	BTREE *t;
	pgno_t pg;
{
	PAGE *h;

	if ((h = mpool_get(t->bt_mp, pg, 0)) == NULL)
		return (RET_ERROR);
	h->flags |= P_PRESERVE;
	mpool_put(t->bt_mp, h, MPOOL_DIRTY);
	return (RET_SUCCESS);
}

/*
 * REC_TOTAL -- Return the number of recno entries below a page.
 *
 * Parameters:
 *	h:	page
 *
 * Returns:
 *	The number of recno entries below a page.
 *
 * XXX
 * These values could be set by the bt_psplit routine.  The problem is that the
 * entry has to be popped off of the stack etc. or the values have to be passed
 * all the way back to bt_split/bt_rroot and it's not very clean.
 */
static recno_t
rec_total(h)
	PAGE *h;
{
	recno_t recs;
	index_t nxt, top;

	for (recs = 0, nxt = 0, top = NEXTINDEX(h); nxt < top; ++nxt)
		recs += GETRINTERNAL(h, nxt)->nrecs;
	return (recs);
}
