/*
 * Copyright (c) 2013-2018,2020-2021 Red Hat.
 * Copyright (c) 1995-2002 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * Thread-safe notes
 *
 * - only static here is typename[] in typeStr() ... this is only used
 *   on an unlikely error path when -Dlogmeta is in effect, so don't
 *   bother with any locking
 *
 */

#include "pmapi.h"
#include "libpcp.h"
#include "fault.h"
#include "internal.h"
#include <stddef.h>
#include <assert.h>

/* bytes for a length field in a header/trailer, or a string length field */
#define LENSIZE	4

/*
 * external metadata record types -> string
 */
static char *
typeStr(int type)
{
    static char	typename[16];
    switch (type) {
	case TYPE_DESC:
		    return "DESC";
	case TYPE_INDOM_V2:
		    return "INDOM_V2";
	case TYPE_LABEL_V2:
		    return "LABEL_V2";
	case TYPE_TEXT:
		    return "TEXT";
	case TYPE_INDOM:
		    return "INDOM";
	case TYPE_INDOM_DELTA:
		    return "INDOM_DELTA";
	case TYPE_LABEL:
		    return "LABEL";
	default:
		    snprintf(typename, sizeof(typename)-1, "BAD (%d)", type);
		    return typename;
    }
}

static void
StrTimestamp(const __pmTimestamp *tsp)
{
    if (tsp == NULL)
	fprintf(stderr, "<null timestamp>");
    else
	__pmPrintTimestamp(stderr, tsp);
}

static inline int
sameinst(const __pmLogInDom *a, const __pmLogInDom *b, int index)
{
    if (a->instlist[index] != b->instlist[index])
	return 0;
    return strcmp(a->namelist[index], b->namelist[index]) == 0;
}

/*
 * Return true (non-zero) if the indoms are the same, else false (zero).
 * The time stamp does not matter in this indom comparison.  Because we
 * keep sorted instance lists in memory (see addinsts) we are able to do
 * a linear indom comparison here.
 */
static int
sameindom(const __pmLogInDom *idp1, const __pmLogInDom *idp2)
{
    int			i, numinst;

    if ((numinst = idp1->numinst) != idp2->numinst)
	return 0;
    for (i = 0; i < numinst; i++)
	if (!sameinst(idp1, idp2, i))
	    break;
    return i == numinst;
}

/*
 * Sort the given instance arrays based on ascending identifier,
 * before associating them with the __pmLogInDom.  This allows a
 * variety of optimised lookups in subsequent code that needs to
 * search for specific instances, compare instance domains, etc.
 * Use an insertion sort because its often the case that we're
 * dealing with close-to-sorted data.  Because we have dependent
 * arrays, we cannot use the usual qsort/sort_r routines here.
 */
static void
addinsts(__pmLogInDom *idp, int numinst, int *instlist, char **namelist)
{
    int			i, j, id;
    char		*name;

    for (i = 0; i < numinst; i++) {
	name = namelist[i];
	id = instlist[i];
	j = i;

	while (j > 0 && id < instlist[j-1]) {
	    namelist[j] = namelist[j-1];
	    instlist[j] = instlist[j-1];
	    j = j - 1;
	}
	namelist[j] = name;
	instlist[j] = id;
    }

    idp->numinst = numinst;
    idp->instlist = instlist;
    idp->namelist = namelist;
}

/*
 * Add the given instance domain to the hashed instance domain.
 * Filter out duplicates.
 */
int
addindom(__pmLogCtl *lcp, pmInDom indom, const __pmTimestamp *tsp, int numinst, 
         int *instlist, char **namelist, __int32_t *indom_buf, int allinbuf)
{
    __pmLogInDom	*idp, *idp_prev;
    __pmLogInDom	*idp_cached, *idp_time;
    __pmHashNode	*hp;
    int			timecmp;
    int			sts;

PM_FAULT_POINT("libpcp/" __FILE__ ":1", PM_FAULT_ALLOC);
    if ((idp = (__pmLogInDom *)malloc(sizeof(__pmLogInDom))) == NULL)
	return -oserror();
    idp->stamp = *tsp;		/* struct assignment */
    idp->buf = indom_buf;
    idp->allinbuf = allinbuf;
    addinsts(idp, numinst, instlist, namelist);

    if (pmDebugOptions.logmeta) {
	char    strbuf[20];
	fprintf(stderr, "addindom( ..., %s, ", pmInDomStr_r(indom, strbuf, sizeof(strbuf)));
	StrTimestamp(tsp);
	fprintf(stderr, ", numinst=%d)\n", numinst);
    }

    if ((hp = __pmHashSearch((unsigned int)indom, &lcp->l_hashindom)) == NULL) {
	idp->next = NULL;
	sts = __pmHashAdd((unsigned int)indom, (void *)idp, &lcp->l_hashindom);
	if (sts > 0) {
	    /* __pmHashAdd returns 1 for success, but we want 0. */
	    sts = 0;
	}
	return sts;
    }

    /*
     * Filter out identical indoms. This is very common in multi-archive
     * contexts where the individual archives almost always use the same
     * instance domains.
     *
     * The indoms need to be sorted by decreasing time stamp. Before
     * multi-archive contexts, this happened automatically. Now we
     * must do it explicitly. Duplicates must be moved to the head of their
     * time slot.
     */
    sts = 0;
    idp_prev = NULL;
    for (idp_cached = (__pmLogInDom *)hp->data; idp_cached; idp_cached = idp_cached->next) {
	timecmp = __pmTimestampCmp(&idp_cached->stamp, &idp->stamp);

	/*
	 * If the time of the current cached item is before our time,
	 * then insert here.
	 */
	if (timecmp < 0)
	    break;

	/*
	 * If the time of the current cached item is the same as our time,
	 * search for a duplicate in this time slot. If found, move it
	 * to the head of this time slot. Otherwise insert this new item
	 * at the head of the time slot.
	 */
	if (timecmp == 0) {
	    assert(sts == 0);
	    idp_time = idp_prev; /* just before this time slot */
	    do {
		/* Have we found a duplicate? */
		if (pmDebugOptions.logmeta && pmDebugOptions.desperate) {
		    char	strbuf[20];
		    fprintf(stderr, "indom: %s sameindom(",
			pmInDomStr_r(indom, strbuf, sizeof(strbuf)));
		    __pmPrintTimestamp(stderr, &idp_cached->stamp);
		    fprintf(stderr, "[%d numinst],", idp_cached->numinst);
		    __pmPrintTimestamp(stderr, &idp->stamp);
		    fprintf(stderr, "[%d numinst]) ? ", idp->numinst);
		}
		if (sameindom(idp_cached, idp)) {
		    sts = PMLOGPUTINDOM_DUP; /* duplicate */
		    if (pmDebugOptions.logmeta && pmDebugOptions.desperate)
			fprintf(stderr, "yes\n");
		    break;
		}
		if (pmDebugOptions.logmeta && pmDebugOptions.desperate)
		    fprintf(stderr, "no\n");
		/* Try the next one */
		idp_prev = idp_cached;
		idp_cached = idp_cached->next;
		if (idp_cached == NULL)
		    break;
		timecmp = __pmTimestampCmp(&idp_cached->stamp, &idp->stamp);
	    } while (timecmp == 0);

	    if (sts == PMLOGPUTINDOM_DUP) {
		/*
		 * We found a duplicate. We can't free instlist, namelist and
		 * indom_buf because we don't know where the storage
		 * came from. Only the caller knows. The best we can do is to
		 * indicate that we found a duplicate and let the caller manage
		 * them. We do, however need to free idp.
		 */
		free(idp);
		if (idp_prev == idp_time) {
		    /* The duplicate is already in the right place. */
		    return sts; /* ok -- duplicate */
		}

		/* Unlink the duplicate and set it up to be re-inserted. */
		assert(idp_cached != NULL);
		if (idp_prev)
		    idp_prev->next = idp_cached->next;
		else
		    hp->data = (void *)idp_cached->next;
		idp = idp_cached;
	    }

	    /*
	     * Regardless of whether or not a duplicate was found, we will be
	     * inserting the indom we have at the head of the time slot.
	     */
	    idp_prev = idp_time;
	    break;
	}

	/*
	 * The time of the current cached item is after our time.
	 * Just keep looking.
	 */
	idp_prev = idp_cached;
    }

    /* Insert at the identified insertion point. */
    if (idp_prev == NULL) {
	idp->next = (__pmLogInDom *)hp->data;
	hp->data = (void *)idp;
    }
    else {
	idp->next = idp_prev->next;
	idp_prev->next = idp;
    }

    return sts;
}

static int
addlabel(__pmArchCtl *acp, unsigned int type, unsigned int ident, int nsets,
		pmLabelSet *labelsets, const __pmTimestamp *tsp)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmLogLabelSet	*idp, *idp_prev;
    __pmLogLabelSet	*idp_cached;
    __pmHashNode	*hp;
    __pmHashCtl		*l_hashtype;
    pmLabelSet		*label;
    int			timecmp;
    int			sts;

PM_FAULT_POINT("libpcp/" __FILE__ ":13", PM_FAULT_ALLOC);
    if ((idp = (__pmLogLabelSet *)malloc(sizeof(__pmLogLabelSet))) == NULL)
	return -oserror();
    idp->stamp = *tsp;		/* struct assignment */
    idp->type = type;
    idp->ident = ident;
    idp->nsets = nsets;
    idp->labelsets = labelsets;

    if (pmDebugOptions.logmeta) {
	fprintf(stderr, "addlabel( ..., %u, %u, ", type, ident);
	StrTimestamp(tsp);
	fprintf(stderr, ", nsets=%d)\n", nsets);
    }

    type &= ~(PM_LABEL_COMPOUND|PM_LABEL_OPTIONAL);
    if (type == PM_LABEL_CONTEXT)
	ident = PM_ID_NULL;

    if ((sts = __pmLogLookupLabel(acp, type, ident, &label, NULL)) <= 0) {

	idp->next = NULL;

	if ((hp = __pmHashSearch(type, &lcp->l_hashlabels)) == NULL) {
	    if ((l_hashtype = (__pmHashCtl *) calloc(1, sizeof(__pmHashCtl))) == NULL) {
		free(idp);
		return -oserror();
	    }

	    sts = __pmHashAdd(type, (void *)l_hashtype, &lcp->l_hashlabels);
	    if (sts < 0) {
		free(idp);
		return sts;
	    }
	} else {
	    l_hashtype = (__pmHashCtl *)hp->data;
	}

	/* __pmHashAdd returns 1 for success, but we want 0. */
	sts = __pmHashAdd(ident, (void *)idp, l_hashtype);
	if (sts > 0)
	    sts = 0;
	return sts;
    }

    if ((hp = __pmHashSearch(type, &lcp->l_hashlabels)) == NULL) {
	free(idp);
	return PM_ERR_NOLABELS;
    }

    l_hashtype = (__pmHashCtl *)hp->data;

    if ((hp = __pmHashSearch(ident, l_hashtype)) == NULL) {
	free(idp);
	return PM_ERR_NOLABELS;
    }

    sts = 0;
    idp_prev = NULL;
    for (idp_cached = (__pmLogLabelSet *)hp->data; idp_cached; idp_cached = idp_cached->next) {
	timecmp = __pmTimestampCmp(&idp_cached->stamp, &idp->stamp);

	/*
	 * If the time of the current cached item is before our time,
	 * then insert here.
	 */
	if (timecmp < 0)
	    break;

	/*
	 * The time of the current cached item is after our time.
	 * Just keep looking.
	 */
	idp_prev = idp_cached;
    }

    /* Insert at the identified insertion point. */
    if (idp_prev == NULL) {
	idp->next = (__pmLogLabelSet *)hp->data;
	hp->data = (void *)idp;
    }
    else {
	idp->next = idp_prev->next;
	idp_prev->next = idp;
    }

    return sts;
}

/* Return 1 (true) if the sets are the same, 0 (false) otherwise */
static int
samelabelset(const pmLabelSet *set1, const pmLabelSet *set2)
{
    int			n1, n2;
    const pmLabel	*l1, *l2;

    /*
     * The instance identifiers must be the same. */
    if (set1->inst != set2->inst)
	return 0; /* not the same */
    
    /* The sets must be of the same size. */
    if (set1->nlabels != set2->nlabels)
	return 0; /* not the same */

    /*
     * Check that each label in set1 is also in set2 with the same value.
     * We already know that the sets are of the same size, so that is
     * sufficient to declare the sets to be the same.
     */
    for (n1 = 0; n1 < set1->nlabels; n1++) {
	l1 = &set1->labels[n1];
	for (n2 = 0; n2 < set2->nlabels; n2++) {
	    l2 = &set2->labels[n2];

	    /* Is the label name the same? */
	    if (l1->namelen != l2->namelen)
		continue;
	    if (memcmp(& set1->json[l1->name], & set2->json[l2->name],
		       l1->namelen) != 0)
		continue;

	    /* Is the label value the same? If not, then we can abandon the
	     * comparison immediately, since we have labels with the same name
	     * but different values.
	     */
	    if (l1->valuelen != l2->valuelen)
		return 0; /* not the same */
	    if (memcmp(& set1->json[l1->value], & set2->json[l2->value],
		       l1->valuelen) != 0)
		return 0; /* not the same */

	    /* We found l1 in set2. */
	    break;
	}

	/*
	 * If l1 was not in set2, then we can abandon the comparison
	 * immediately.
	 */
	if (n2 == set2->nlabels)
	    return 0; /* not the same */
    }
		
    /* All of the labels in set1 are in set2 with the same values. */
    return 1; /* the same */
}

/*
 * Discard any label sets within idp which are also within idp_next.
 */
static void
discard_dup_labelsets(__pmLogLabelSet *idp, const __pmLogLabelSet *idp_next)
{
    int			i, j;

    for (i = 0; i < idp->nsets; ++i) {
	for (j = 0; j < idp_next->nsets; ++j) {
	    if (samelabelset(&idp->labelsets[i], &idp_next->labelsets[j])) {
		/* We found a duplicate. Discard the one within idp. */
		if (idp->labelsets[i].nlabels > 0)
		    free(idp->labelsets[i].labels);
		if (idp->labelsets[i].json)
		    free(idp->labelsets[i].json);
		--idp->nsets;
		if (idp->nsets > i) {
		    memmove(&idp->labelsets[i], &idp->labelsets[i+1],
			    (idp->nsets - i) * sizeof(idp->labelsets[i]));
		}
		/* Careful with the next iteration. */
		--i;
		break;
	    }
	}
    }
}

/*
 * Check for duplicate label sets. This is very common in multi-archive
 * contexts. Since label sets are timestamped, only identical ones
 * adjacent in time are actually duplicates.
 *
 * addlabel() does not assume that label sets are added in chronological order
 * so we do this after all of the meta data for each individual archive
 * has been read. At this point we know that the label sets are stored in reverse
 * chronological order.
 */
static void
check_dup_labels(const __pmArchCtl *acp)
{
    __pmLogCtl		*lcp;
    __pmLogLabelSet	*idp, *idp_prev, *idp_next;
    __pmHashCtl		*l_hashlabels;
    __pmHashCtl		*l_hashtype;
    __pmHashNode	*hplabels, *hptype;
    int			type;
    int			ident;

    /* Traverse the double hash table representing the label sets. */
    lcp = acp->ac_log;
    l_hashlabels = &lcp->l_hashlabels;
    for (type = 0; type < l_hashlabels->hsize; ++type) {
        for (hplabels = l_hashlabels->hash[type]; hplabels; hplabels = hplabels->next) {
	    l_hashtype = (__pmHashCtl *)hplabels->data;
	    for (ident = 0; ident < l_hashtype->hsize; ++ident) {
		for (hptype = l_hashtype->hash[ident]; hptype; hptype = hptype->next) {
		    idp_prev = NULL;
		    for (idp = (__pmLogLabelSet *)hptype->data; idp; idp = idp_next) {
			idp_next = idp->next;
			if (idp_next == NULL)
			    break; /* done */

			/*
			 * idp and idp_next each hold sets of label sets. Since idp is
			 * later in time, we want to discard any label sets within
			 * idp which are the same as any label sets in idp_next.
			 */
			discard_dup_labelsets(idp, idp_next);
			if (idp->nsets == 0) {
			    /*
			     * All label sets within idp were discarded.
			     * unlink it and free it.
			     */
			    if (idp_prev)
				idp_prev->next = idp_next;
			    else
				hptype->data = idp_next;
			    free(idp->labelsets);
			    free(idp);
			}
			else
			    idp_prev = idp;
		    }
		}
	    }
	}
    }
}

static int
addtext(__pmArchCtl *acp, unsigned int ident, unsigned int type, const char *buffer)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmHashNode	*hp;
    __pmHashCtl		*l_hashtype;
    char		*text;
    int			sts;

PM_FAULT_POINT("libpcp/" __FILE__ ":15", PM_FAULT_ALLOC);
    if (pmDebugOptions.logmeta) {
	char	strbuf[20];
	if ((type & PM_TEXT_INDOM) == PM_TEXT_INDOM)
	    fprintf(stderr, "addtext( ..., %s (indom), ",
		    pmInDomStr_r((pmInDom)ident, strbuf, sizeof(strbuf)));
	else
	    fprintf(stderr, "addtext( ..., %s, ",
		    pmIDStr_r((pmID)ident, strbuf, sizeof(strbuf)));
	if ((type & PM_TEXT_ONELINE) == PM_TEXT_ONELINE) {
	    fprintf(stderr, "ONELINE");
	    if ((type & PM_TEXT_HELP) == PM_TEXT_HELP)
		fprintf(stderr, "|HELP");
	}
	else if ((type & PM_TEXT_HELP) == PM_TEXT_HELP)
	    fprintf(stderr, "HELP");
	else
	    fprintf(stderr, "type=%u??", type);
	fprintf(stderr, ")\n");
    }

    if ((sts = __pmLogLookupText(acp, ident, type, &text)) < 0) {
	/* This is a new help text record. Add it to the hash structure. */
	if ((hp = __pmHashSearch(type, &lcp->l_hashtext)) == NULL) {
	    if ((l_hashtype = (__pmHashCtl *)calloc(1, sizeof(__pmHashCtl))) == NULL)
		return -oserror();

	    sts = __pmHashAdd(type, (void *)l_hashtype, &lcp->l_hashtext);
	    if (sts < 0)
		return sts;
	} else {
	    l_hashtype = (__pmHashCtl *)hp->data;
	}

	if ((text = strdup(buffer)) == NULL)
	    return -oserror();

	if ((sts = __pmHashAdd(ident, (void *)text, l_hashtype)) > 0) {
	    /* __pmHashAdd returns 1 for success, but we want 0. */
	    sts = 0;
	}
	return sts;
    }

    /*
     * This help text already exists. Tolerate change for the purpose of making
     * corrections over time. Do this by keeping the latest version and
     * discarding the original, if they are different.
     */
    if (strcmp(buffer, text) != 0) {
	/*
	 * Find the hash table entry. We know it's there because
	 * __pmLogLookupText() succeeded above.
	 */
	hp = __pmHashSearch(type, &lcp->l_hashtext);
	assert(hp != NULL);

	l_hashtype = (__pmHashCtl *)hp->data;
	hp = __pmHashSearch(ident, l_hashtype);
	assert(hp != NULL);

	/* Free the existing text and keep the new text. */
	assert (text == (char *)hp->data);
	free(text);
	hp->data = (void*)strdup(buffer);
	if (hp->data == NULL)
	    return -oserror();
    }
    
    return sts;
}

int
__pmLogAddDesc(__pmArchCtl *acp, const pmDesc *newdp)
{
    __pmHashNode	*hp;
    __pmLogCtl		*lcp = acp->ac_log;
    pmDesc		*dp, *olddp;

    if ((hp = __pmHashSearch((int)newdp->pmid, &lcp->l_hashpmid)) != NULL) {
	/* PMID is already in the hash table - check for conflicts. */
	olddp = (pmDesc *)hp->data;
	if (newdp->type != olddp->type)
	    return PM_ERR_LOGCHANGETYPE;
	if (newdp->sem != olddp->sem)
	    return PM_ERR_LOGCHANGESEM;
	if (newdp->indom != olddp->indom)
	    return PM_ERR_LOGCHANGEINDOM;
	if (newdp->units.dimSpace != olddp->units.dimSpace ||
	    newdp->units.dimTime != olddp->units.dimTime ||
	    newdp->units.dimCount != olddp->units.dimCount ||
	    newdp->units.scaleSpace != olddp->units.scaleSpace ||
	    newdp->units.scaleTime != olddp->units.scaleTime ||
	    newdp->units.scaleCount != olddp->units.scaleCount)
	    return PM_ERR_LOGCHANGEUNITS;

	/* PMID is already known and checks out - we're done here. */
	return 0;
    }

    /* Add a copy of the descriptor into the PMID:desc hash table. */
PM_FAULT_POINT("libpcp/" __FILE__ ":2", PM_FAULT_ALLOC);
    if ((dp = (pmDesc *)malloc(sizeof(pmDesc))) == NULL)
	return -oserror();
    *dp = *newdp;

    return __pmHashAdd((int)dp->pmid, (void *)dp, &lcp->l_hashpmid);
}

int
__pmLogAddPMNSNode(__pmArchCtl *acp, pmID pmid, const char *name)
{
    __pmLogCtl		*lcp = acp->ac_log;
    int			sts;

    /*
     * If we see a duplicate name with a different PMID, its a
     * recoverable error.
     * We wont be able to see all of the data in the log, but
     * its better to provide access to some rather than none,
     * esp. when only one or two metric IDs may be corrupted
     * in this way (which we may not be interested in anyway).
     */
    sts = __pmAddPMNSNode(lcp->l_pmns, pmid, name);
    if (sts == PM_ERR_PMID)
	sts = 0;
    return sts;
}

int
__pmLogAddInDom(__pmArchCtl *acp, const __pmTimestamp *tsp, const pmInResult *in,
		__int32_t *tbuf, int allinbuf)
{
    return addindom(acp->ac_log, in->indom, tsp,
		    in->numinst, in->instlist, in->namelist, tbuf, allinbuf);
}

int
__pmLogAddLabelSets(__pmArchCtl *acp, const __pmTimestamp *tsp, unsigned int type,
		unsigned int ident, int nsets, pmLabelSet *labelsets)
{
    return addlabel(acp, type, ident, nsets, labelsets, tsp);
}

int
__pmLogAddText(__pmArchCtl *acp, unsigned int ident, unsigned int type, const char *buffer)
{
    return addtext(acp, ident, type, buffer);
}

/*
 * Load _all_ of the hashed pmDesc and __pmLogInDom structures from the metadata
 * log file -- used at the initialization (NewContext) of an archive.
 * Also load all the metric names from the metadata log file and create l_pmns,
 * if it does not already exist.
 */
int
__pmLogLoadMeta(__pmArchCtl *acp)
{
    __pmLogCtl		*lcp = acp->ac_log;
    int			rlen;
    int			check;
    int			sts = 0;
    __pmLogHdr		h;
    __pmFILE		*f = lcp->l_mdfp;
    int			numpmid = 0;
    int			n;
    int			numnames;
    int			i;
    int			len;
    char		name[MAXPATHLEN];
    
    if (lcp->l_pmns == NULL) {
	if ((sts = __pmNewPMNS(&(lcp->l_pmns))) < 0)
	    goto end;
    }

    __pmFseek(f, (long)__pmLogLabelSize(lcp), SEEK_SET);
    for ( ; ; ) {
	n = (int)__pmFread(&h, 1, sizeof(__pmLogHdr), f);

	/* swab hdr */
	h.len = ntohl(h.len);
	h.type = ntohl(h.type);

	if (n != sizeof(__pmLogHdr) || h.len <= 0) {
            if (__pmFeof(f)) {
		__pmClearerr(f);
                sts = 0;
		goto end;
            }
	    if (pmDebugOptions.logmeta) {
		fprintf(stderr, "__pmLogLoadMeta: header read -> %d: expected: %d or len=%d\n",
			n, (int)sizeof(__pmLogHdr), h.len);
	    }
	    if (__pmFerror(f)) {
		__pmClearerr(f);
		sts = -oserror();
	    }
	    else
		sts = PM_ERR_LOGREC;
	    goto end;
	}
	if (pmDebugOptions.logmeta) {
	    fprintf(stderr, "__pmLogLoadMeta: record len=%d, type=%s @ offset=%d\n",
		h.len, typeStr(h.type), (int)(__pmFtell(f) - sizeof(__pmLogHdr)));
	}
	rlen = h.len - (int)sizeof(__pmLogHdr) - (int)sizeof(int);
	if (h.type == TYPE_DESC) {
	    pmDesc		desc;

	    numpmid++;
	    if ((n = (int)__pmFread(&desc, 1, sizeof(pmDesc), f)) != sizeof(pmDesc)) {
		if (pmDebugOptions.logmeta) {
		    fprintf(stderr, "__pmLogLoadMeta: pmDesc read -> %d: expected: %d\n",
			    n, (int)sizeof(pmDesc));
		}
		if (__pmFerror(f)) {
		    __pmClearerr(f);
		    sts = -oserror();
		}
		else
		    sts = PM_ERR_LOGREC;
		goto end;
	    }

	    /* swab desc */
	    desc.type = ntohl(desc.type);
	    desc.sem = ntohl(desc.sem);
	    desc.indom = __ntohpmInDom(desc.indom);
	    desc.units = __ntohpmUnits(desc.units);
	    desc.pmid = __ntohpmID(desc.pmid);

	    if ((sts = __pmLogAddDesc(acp, &desc)) < 0)
		goto end;

	    /* read in the names & store in PMNS tree ... */
	    if ((n = (int)__pmFread(&numnames, 1, sizeof(numnames), f)) != 
		sizeof(numnames)) {
		if (pmDebugOptions.logmeta) {
		    fprintf(stderr, "%s: numnames read -> %d: expected: %d\n",
			    "__pmLogLoadMeta", n, (int)sizeof(numnames));
		}
		if (__pmFerror(f)) {
		    __pmClearerr(f);
		    sts = -oserror();
		}
		else
		    sts = PM_ERR_LOGREC;
		goto end;
	    }
	    else {
		/* swab numnames */
		numnames = ntohl(numnames);
	    }

	    for (i = 0; i < numnames; i++) {
		if ((n = (int)__pmFread(&len, 1, sizeof(len), f)) != 
		    sizeof(len)) {
		    if (pmDebugOptions.logmeta) {
			fprintf(stderr, "%s: len name[%d] read -> %d: expected: %d\n",
				"__pmLogLoadMeta", i, n, (int)sizeof(len));
		    }
		    if (__pmFerror(f)) {
			__pmClearerr(f);
			sts = -oserror();
		    }
		    else
			sts = PM_ERR_LOGREC;
		    goto end;
		}
		else {
		    /* swab len */
		    len = ntohl(len);
		}

		if ((n = (int)__pmFread(name, 1, len, f)) != len) {
		    if (pmDebugOptions.logmeta) {
			fprintf(stderr, "%s: name[%d] read -> %d: expected: %d\n",
				"__pmLogLoadMeta", i, n, len);
		    }
		    if (__pmFerror(f)) {
			__pmClearerr(f);
			sts = -oserror();
		    }
		    else
			sts = PM_ERR_LOGREC;
		    goto end;
		}
		name[len] = '\0';
		if (pmDebugOptions.logmeta) {
		    char	strbuf[20];
		    fprintf(stderr, "%s: PMID: %s name: %s\n",
			    "__pmLogLoadMeta",
			    pmIDStr_r(desc.pmid, strbuf, sizeof(strbuf)), name);
		}

		/* Add the new PMNS node into this context */
		if ((sts = __pmLogAddPMNSNode(acp, desc.pmid, name)) < 0)
		    goto end;
	    }/*for*/
	}
	else if (h.type == TYPE_INDOM || h.type == TYPE_INDOM_V2) {
	    __pmTimestamp	stamp;
	    pmInResult		in;
	    __int32_t		*buf;
	    int			allinbuf;

	    if ((allinbuf = __pmLogLoadInDom(acp, rlen, h.type, &in, &stamp, &buf)) < 0) {
		sts = allinbuf;
		goto end;
	    }
	    if (in.numinst > 0) {
		/*
		 * we have instances, so in.namelist is not NULL
		 */
		if ((sts = __pmLogAddInDom(acp, &stamp, &in, buf, allinbuf)) < 0) {
		    free(buf);
		    if (!allinbuf)
			free(in.namelist);
		    goto end;
		}
		/* If this indom was a duplicate, then we need to free tbuf and
		   namelist, as appropriate. */
		if (sts == PMLOGPUTINDOM_DUP) {
		    free(buf);
		    if (!allinbuf)
			free(in.namelist);
		}
	    }
	    else {
		/* no instances, or an error */
		free(buf);
	    }
	}
	else if (h.type == TYPE_LABEL || h.type == TYPE_LABEL_V2) {
	    char		*tbuf;
	    int			k;
	    int			j;
	    int			type;
	    int			ident;
	    int			nsets;
	    int			inst;
	    int			jsonlen;
	    int			nlabels;
	    __pmTimestamp	stamp;
	    pmLabelSet		*labelsets = NULL;

PM_FAULT_POINT("libpcp/" __FILE__ ":11", PM_FAULT_ALLOC);
	    if ((tbuf = (char *)malloc(rlen)) == NULL) {
		sts = -oserror();
		goto end;
	    }
	    if ((n = (int)__pmFread(tbuf, 1, rlen, f)) != rlen) {
		if (pmDebugOptions.logmeta) {
		    fprintf(stderr, "%s: label read -> %d: expected: %d\n",
			    "__pmLogLoadMeta", n, rlen);
		}
		if (__pmFerror(f)) {
		    __pmClearerr(f);
		    sts = -oserror();
		}
		else
		    sts = PM_ERR_LOGREC;
		free(tbuf);
		goto end;
	    }

	    k = 0;
	    if (h.type == TYPE_LABEL_V2) {
		pmTimeval		*tvp;
		tvp = (pmTimeval *)&tbuf[k];
		stamp.sec = ntohl(tvp->tv_sec);
		stamp.nsec = ntohl(tvp->tv_usec) * 1000;
		k += 2*sizeof(__int32_t);
	    }
	    else {
		__pmTimestamp	*tsp;
		tsp = (__pmTimestamp *)&tbuf[k];
		stamp.sec = tsp->sec;
		__ntohll((char *)&stamp.sec);
		stamp.nsec = ntohl(tsp->nsec);
		k += sizeof(__uint64_t) + sizeof(__int32_t);
	    }

	    type = ntohl(*((unsigned int*)&tbuf[k]));
	    k += sizeof(type);

	    ident = ntohl(*((unsigned int*)&tbuf[k]));
	    k += sizeof(ident);

	    nsets = *((unsigned int *)&tbuf[k]);
	    nsets = ntohl(nsets);
	    k += sizeof(nsets);

	    if (nsets > 0 &&
		(labelsets = (pmLabelSet *)calloc(nsets, sizeof(pmLabelSet))) == NULL) {
		sts = -oserror();
		free(tbuf);
		goto end;
	    }

	    for (i = 0; i < nsets; i++) {
		inst = *((unsigned int*)&tbuf[k]);
		inst = ntohl(inst);
		k += sizeof(inst);
		labelsets[i].inst = inst;

		jsonlen = ntohl(*((unsigned int*)&tbuf[k]));
		k += sizeof(jsonlen);
		labelsets[i].jsonlen = jsonlen;

		if (jsonlen < 0 || jsonlen > PM_MAXLABELJSONLEN) {
		    if (pmDebugOptions.logmeta)
			fprintf(stderr, "%s: corrupted json in labelset. jsonlen=%d\n",
					"__pmLogLoadMeta", jsonlen);
		    sts = PM_ERR_LOGREC;
		    free(labelsets);
		    free(tbuf);
		    goto end;
		}

		if ((labelsets[i].json = (char *)malloc(jsonlen+1)) == NULL) {
		    sts = -oserror();
		    free(labelsets);
		    free(tbuf);
		    goto end;
		}

		memcpy((void *)labelsets[i].json, (void *)&tbuf[k], jsonlen);
		labelsets[i].json[jsonlen] = '\0';
		k += jsonlen;

		/* label nlabels */
		nlabels = ntohl(*((unsigned int *)&tbuf[k]));
		k += sizeof(nlabels);
		labelsets[i].nlabels = nlabels;

		if (nlabels > 0) { /* nlabels < 0 is an error code. skip it here */
		    if (nlabels > PM_MAXLABELS || k + nlabels * sizeof(pmLabel) > rlen) {
			/* corrupt archive metadata detected. GH #475 */
			if (pmDebugOptions.logmeta)
			    fprintf(stderr, "%s: corrupted labelset. nlabels=%d\n",
					    "__pmLogLoadMeta", nlabels);
			sts = PM_ERR_LOGREC;
			free(labelsets);
			free(tbuf);
			goto end;
		    }

		    if ((labelsets[i].labels = (pmLabel *)calloc(nlabels, sizeof(pmLabel))) == NULL) {
			sts = -oserror();
			free(labelsets);
			free(tbuf);
			goto end;
		    }

		    /* label pmLabels */
		    for (j = 0; j < nlabels; j++) {
			labelsets[i].labels[j] = *((pmLabel *)&tbuf[k]);
			__ntohpmLabel(&labelsets[i].labels[j]);
			k += sizeof(pmLabel);
		    }
		}
	    }
	    free(tbuf);

	    if ((sts = addlabel(acp, type, ident, nsets, labelsets, &stamp)) < 0)
		goto end;
	}
	else if (h.type == TYPE_TEXT) {
	    char		*tbuf;
	    int			type;
	    int			ident;
	    int			k;

PM_FAULT_POINT("libpcp/" __FILE__ ":16", PM_FAULT_ALLOC);
	    if ((tbuf = (char *)malloc(rlen)) == NULL) {
		sts = -oserror();
		goto end;
	    }
	    if ((n = (int)__pmFread(tbuf, 1, rlen, f)) != rlen) {
		if (pmDebugOptions.logmeta) {
		    fprintf(stderr, "%s: text read -> %d: expected: %d\n",
				    "__pmLogLoadMeta", n, rlen);
		}
		if (__pmFerror(f)) {
		    __pmClearerr(f);
		    sts = -oserror();
		}
		else
		    sts = PM_ERR_LOGREC;
		free(tbuf);
		goto end;
	    }

	    k = 0;
	    type = ntohl(*((unsigned int *)&tbuf[k]));
	    k += sizeof(type);
	    if (!(type & (PM_TEXT_ONELINE|PM_TEXT_HELP))) {
		if (pmDebugOptions.logmeta) {
		    fprintf(stderr, "__pmLogLoadMeta: bad text type -> %x\n",
			    type);
		}
		free(tbuf);
		continue;
	    }
	    else if (type & PM_TEXT_INDOM)
		ident = __ntohpmInDom(*((unsigned int *)&tbuf[k]));
	    else if (type & PM_TEXT_PMID)
		ident = __ntohpmID(*((unsigned int *)&tbuf[k]));
	    else {
		if (pmDebugOptions.logmeta) {
		    fprintf(stderr, "%s: bad text ident -> %x\n",
				    "__pmLogLoadMeta", type);
		}
		free(tbuf);
		continue;
	    }
	    k += sizeof(ident);

	    sts = addtext(acp, ident, type, (char *)&tbuf[k]);
	    free(tbuf);
	    if (sts < 0)
		goto end;
	}
	else
	    __pmFseek(f, (long)rlen, SEEK_CUR);
	n = (int)__pmFread(&check, 1, sizeof(check), f);
	check = ntohl(check);
	if (n != sizeof(check) || h.len != check) {
	    if (pmDebugOptions.logmeta) {
		fprintf(stderr, "%s: trailer read -> %d or len=%d: "
				"expected %d @ offset=%d\n", "__pmLogLoadMeta",
			n, check, h.len, (int)(__pmFtell(f) - sizeof(check)));
	    }
	    if (__pmFerror(f)) {
		__pmClearerr(f);
		sts = -oserror();
	    }
	    else
		sts = PM_ERR_LOGREC;
	    goto end;
	}
    }/*for*/
end:

    /* Check for duplicate label sets. */
    check_dup_labels(acp);
    
    __pmFseek(f, (long)__pmLogLabelSize(lcp), SEEK_SET);

    if (sts == 0) {
	if (numpmid == 0) {
	    if (pmDebugOptions.logmeta) {
		fprintf(stderr, "%s: no metrics found?\n", "__pmLogLoadMeta");
	    }
	    sts = PM_ERR_LOGREC;
	}
	else
	    __pmFixPMNSHashTab(lcp->l_pmns, numpmid, 1);
    }
    return sts;
}

/*
 * scan the hashed data structures to find a pmDesc, given a pmid
 */
int
__pmLogLookupDesc(__pmArchCtl *acp, pmID pmid, pmDesc *dp)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmHashNode	*hp;
    pmDesc		*tp;

    if ((hp = __pmHashSearch((unsigned int)pmid, &lcp->l_hashpmid)) == NULL)
	return PM_ERR_PMID_LOG;

    tp = (pmDesc *)hp->data;
    *dp = *tp;			/* struct assignment */
    return 0;
}

/*
 * Add a new pmDesc into the metadata log, and to the hashed data structures
 * If numnames is positive, then write out any associated PMNS names.
 */
int
__pmLogPutDesc(__pmArchCtl *acp, const pmDesc *dp, int numnames, char **names)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmFILE		*f = lcp->l_mdfp;
    pmDesc		*tdp;
    int			olen;		/* length to write out */
    int			i, sts, len;
    typedef struct {			/* skeletal external record */
	__pmLogHdr	hdr;
	pmDesc		desc;
	int		numnames;	/* not present if numnames == 0 */
	char		data[0];	/* will be expanded */
    } ext_t;
    ext_t	*out;

    len = sizeof(__pmLogHdr) + sizeof(pmDesc) + LENSIZE;
    if (numnames > 0) {
        len += sizeof(numnames);
        for (i = 0; i < numnames; i++)
            len += LENSIZE + (int)strlen(names[i]);
    }
PM_FAULT_POINT("libpcp/" __FILE__ ":10", PM_FAULT_ALLOC);
    if ((out = (ext_t *)calloc(1, len)) == NULL)
	return -oserror();

    out->hdr.len = htonl(len);
    out->hdr.type = htonl(TYPE_DESC);
    out->desc.type = htonl(dp->type);
    out->desc.sem = htonl(dp->sem);
    out->desc.indom = __htonpmInDom(dp->indom);
    out->desc.units = __htonpmUnits(dp->units);
    out->desc.pmid = __htonpmID(dp->pmid);

    if (numnames > 0) {
	char	*op = (char *)&out->data;

	out->numnames = htonl(numnames);

        /* copy the names and length prefix */
        for (i = 0; i < numnames; i++) {
	    int slen = (int)strlen(names[i]);
	    olen = htonl(slen);
	    memmove((void *)op, &olen, sizeof(olen));
	    op += sizeof(olen);
	    memmove((void *)op, names[i], slen);
	    op += slen;
	}
	/* trailer length */
	memmove((void *)op, &out->hdr.len, sizeof(out->hdr.len));
    }
    else {
	/* no names, trailer length lands on numnames in ext_t */
	out->numnames = out->hdr.len;
    }

    if ((sts = __pmFwrite(out, 1, len, f)) != len) {
	char	strbuf[20];
	char	errmsg[PM_MAXERRMSGLEN];
	pmprintf("__pmLogPutDesc(...,pmid=%s,name=%s): write failed: returned %d expecting %d: %s\n",
	    pmIDStr_r(dp->pmid, strbuf, sizeof(strbuf)),
	    numnames > 0 ? names[0] : "<none>", len, sts,
	    osstrerror_r(errmsg, sizeof(errmsg)));
	pmflush();
	free(out);
	return -oserror();
    }

    free(out);

    /*
     * need to make a copy of the pmDesc, and add this, since caller
     * may re-use *dp
     */
PM_FAULT_POINT("libpcp/" __FILE__ ":5", PM_FAULT_ALLOC);
    if ((tdp = (pmDesc *)malloc(sizeof(pmDesc))) == NULL)
	return -oserror();
    *tdp = *dp;		/* struct assignment */
    return __pmHashAdd((int)dp->pmid, (void *)tdp, &lcp->l_hashpmid);
}

static __pmLogInDom *
searchindom(__pmLogCtl *lcp, pmInDom indom, __pmTimestamp *tsp)
{
    __pmHashNode	*hp;
    __pmLogInDom	*idp;

    if (pmDebugOptions.logmeta) {
	char	strbuf[20];
	fprintf(stderr, "searchindom( ..., %s, ", pmInDomStr_r(indom, strbuf, sizeof(strbuf)));
	StrTimestamp(tsp);
	fprintf(stderr, ")\n");
    }

    if ((hp = __pmHashSearch((unsigned int)indom, &lcp->l_hashindom)) == NULL)
	return NULL;

    idp = (__pmLogInDom *)hp->data;
    if (tsp != NULL) {
	for ( ; idp != NULL; idp = idp->next) {
	    /*
	     * need first one at or earlier than the requested time
	     */
	    if (__pmTimestampCmp(&idp->stamp, tsp) <= 0)
		break;
	    if (pmDebugOptions.logmeta) {
		fprintf(stderr, "request @ ");
		StrTimestamp(tsp);
		fprintf(stderr, " is too early for indom @ ");
		StrTimestamp(&idp->stamp);
		fputc('\n', stderr);
	    }
	}
	if (idp == NULL)
	    return NULL;
    }

    if (pmDebugOptions.logmeta) {
	fprintf(stderr, "success for indom @ ");
	StrTimestamp(&idp->stamp);
	fputc('\n', stderr);
    }
    return idp;
}

/*
 * for the given indom retrieve the instance domain that is correct
 * as of the latest time (tsp == NULL) or at a designated
 * time
 */
int
__pmLogGetInDom(__pmArchCtl *acp, pmInDom indom, __pmTimestamp *tsp, int **instlist, char ***namelist)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmLogInDom	*idp = searchindom(lcp, indom, tsp);

    if (idp == NULL)
	return PM_ERR_INDOM_LOG;

    *instlist = idp->instlist;
    *namelist = idp->namelist;

    return idp->numinst;
}

int
__pmLogLookupInDom(__pmArchCtl *acp, pmInDom indom, __pmTimestamp *tsp, 
		   const char *name)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmLogInDom	*idp = searchindom(lcp, indom, tsp);
    int			i;

    if (idp == NULL)
	return PM_ERR_INDOM_LOG;

    if (idp->numinst < 0)
	return idp->numinst;

    /* full match */
    for (i = 0; i < idp->numinst; i++) {
	if (strcmp(name, idp->namelist[i]) == 0)
	    return idp->instlist[i];
    }

    /* half-baked match to first space */
    for (i = 0; i < idp->numinst; i++) {
	char	*p = idp->namelist[i];
	while (*p && *p != ' ')
	    p++;
	if (*p == ' ') {
	    if (strncmp(name, idp->namelist[i], p - idp->namelist[i]) == 0)
		return idp->instlist[i];
	}
    }

    return PM_ERR_INST_LOG;
}

int
__pmLogNameInDom(__pmArchCtl *acp, pmInDom indom, __pmTimestamp *tsp, int inst, char **name)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmLogInDom	*idp = searchindom(lcp, indom, tsp);
    int			i;

    if (idp == NULL)
	return PM_ERR_INDOM_LOG;

    if (idp->numinst < 0)
	return idp->numinst;

    for (i = 0; i < idp->numinst; i++) {
	if (inst == idp->instlist[i]) {
	    *name = idp->namelist[i];
	    return 0;
	}
    }

    return PM_ERR_INST_LOG;
}

/*
 * scan the hash-of-hashes data structure to find a pmLabel,
 * given an identifier and label type.
 */
int
__pmLogLookupLabel(__pmArchCtl *acp, unsigned int type, unsigned int ident,
		pmLabelSet **label, const __pmTimestamp *tsp)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmHashCtl		*label_hash;
    __pmHashNode	*hp;
    __pmLogLabelSet	*ls;

    type &= ~(PM_LABEL_COMPOUND|PM_LABEL_OPTIONAL);
    if (type == PM_LABEL_CONTEXT)
	ident = PM_ID_NULL;

    if ((hp = __pmHashSearch(type, &lcp->l_hashlabels)) == NULL)
	return PM_ERR_NOLABELS;

    label_hash = (__pmHashCtl *)hp->data;
    if ((hp = __pmHashSearch(ident, label_hash)) == NULL)
	return PM_ERR_NOLABELS;

    ls = (__pmLogLabelSet *)hp->data;
    if (tsp != NULL) {
	for ( ; ls != NULL; ls = ls->next) {
	    if (__pmTimestampCmp(&ls->stamp, tsp) <= 0)
		break;
	}
	if (ls == NULL)
	    return 0;
    }
    *label = ls->labelsets;
    return ls->nsets;
}

int
__pmLogPutLabel(__pmArchCtl *acp, unsigned int type, unsigned int ident,
		int nsets, pmLabelSet *labelsets, const __pmTimestamp * const tsp)
{
    __pmLogCtl		*lcp = acp->ac_log;
    char		*ptr;
    int			sts = 0;
    int			i, j;
    size_t		len;
    __int32_t		*lenp;
    int			inst;
    int			nlabels;
    int			jsonlen;
    int			convert;
    pmLabel		label;
    
    void		*out;

    /*
     * Common leader fields on disk (before instances) ...
     * V2: 32-bits for len, type, usec, labeltype, ident, nsets
     *     + 32 bits for sec
     * V3: 32-bits for len, type, nsec, labeltype, ident, nsets
     *     + 64 bits for sec
     */
    len = 6 * sizeof(__int32_t);
    if (__pmLogVersion(lcp) >= PM_LOG_VERS03)
	len += sizeof(__int64_t);
    else
	len += sizeof(__int32_t);

    for (i = 0; i < nsets; i++) {
	len += sizeof(unsigned int);	/* instance identifier */
	len += sizeof(int) + labelsets[i].jsonlen; /* json */
	len += sizeof(int);		/* count or error code */
	if (labelsets[i].nlabels > 0)
	    len += (labelsets[i].nlabels * sizeof(pmLabel));
    }
    len += LENSIZE;

PM_FAULT_POINT("libpcp/" __FILE__ ":12", PM_FAULT_ALLOC);
    if (__pmLogVersion(lcp) >= PM_LOG_VERS03) {
	__pmExtLabelSet_v3	*v3;
	__pmTimestamp		stamp;
	if ((v3 = (__pmExtLabelSet_v3 *)malloc(len)) == NULL)
	    return -oserror();
	/* swab all output fields */
	v3->len = htonl(len);
	v3->type = htonl(TYPE_LABEL);
	stamp.sec = tsp->sec;
	stamp.nsec = tsp->nsec;
	__htonpmTimestamp(&stamp);
	memcpy((void *)&v3->sec[0], (void *)&stamp.sec, sizeof(stamp.sec));
	v3->nsec = stamp.nsec;
	v3->labeltype = htonl(type);
	v3->ident = htonl(ident);
	v3->nsets = htonl(nsets);
	out = (void *)v3;
	ptr = (char *)&v3->data;
	lenp = &v3->len;
    }
    else {
	__pmExtLabelSet_v2	*v2;
	if ((v2 = (__pmExtLabelSet_v2 *)malloc(len)) == NULL)
	    return -oserror();
	/* swab all output fields */
	v2->len = htonl(len);
	v2->type = htonl(TYPE_LABEL_V2);
	v2->sec = (__int32_t)tsp->sec;
	v2->sec = htonl(v2->sec);
	v2->usec = htonl(tsp->nsec / 1000);
	v2->labeltype = htonl(type);
	v2->ident = htonl(ident);
	v2->nsets = htonl(nsets);
	out = (void *)v2;
	ptr = (char *)&v2->data;
	lenp = &v2->len;
    }

    for (i = 0; i < nsets; i++) {
    	/* label inst */
    	inst = htonl(labelsets[i].inst);
	memmove((void *)ptr, (void *)&inst, sizeof(inst));
	ptr += sizeof(inst);

	/* label jsonlen */
	jsonlen = labelsets[i].jsonlen;
	convert = htonl(jsonlen);
	memmove((void *)ptr, (void *)&convert, sizeof(jsonlen));
	ptr += sizeof(jsonlen);

	/* label string */
	memmove((void *)ptr, (void *)labelsets[i].json, jsonlen);
	ptr += jsonlen;

	/* label nlabels */
	nlabels = labelsets[i].nlabels;
	convert = htonl(nlabels);
	memmove((void *)ptr, (void *)&convert, sizeof(nlabels));
	ptr += sizeof(nlabels);

	/* label pmLabels */
	for (j = 0; j < nlabels; j++) {
	    label = labelsets[i].labels[j];
	    __htonpmLabel(&label);
	    memmove((void *)ptr, (void *)&label, sizeof(label));
	    ptr += sizeof(label);
	}
    }

    memcpy((void *)ptr, lenp, sizeof(*lenp));

    if ((sts = __pmFwrite(out, 1, len, lcp->l_mdfp)) != len) {
	char	errmsg[PM_MAXERRMSGLEN];

	pmprintf("__pmLogPutLabel(...,type=%d,ident=%d): write failed: returned %d expecting %zd: %s\n",
		type, ident, sts, len, osstrerror_r(errmsg, sizeof(errmsg)));
	pmflush();
	free(out);
	return -oserror();
    }
    free(out);

    return addlabel(acp, type, ident, nsets, labelsets, tsp);
}

/*
 * scan the indirect hash data structure to find any help text,
 * given an identifier (pmid/indom) and type (oneline/fulltext)
 */
int
__pmLogLookupText(__pmArchCtl *acp, unsigned int ident, unsigned int type,
		char **buffer)
{
    __pmLogCtl		*lcp = acp->ac_log;
    __pmHashCtl		*text_hash;
    __pmHashNode	*hp;

    type &= ~PM_TEXT_DIRECT;
    if ((hp = __pmHashSearch(type, &lcp->l_hashtext)) == NULL)
	return PM_ERR_NOTHOST;	/* back-compat error code */

    text_hash = (__pmHashCtl *)hp->data;
    if ((hp = __pmHashSearch(ident, text_hash)) == NULL)
	return PM_ERR_TEXT;

    *buffer = (char *)hp->data;
    return 0;
}

int
__pmLogPutText(__pmArchCtl *acp, unsigned int ident, unsigned int type,
		char *buffer, int cached)
{
    __pmLogCtl		*lcp = acp->ac_log;
    char		*ptr;
    int			sts, len, textlen;
    typedef struct {
	__pmLogHdr	hdr;
	int		type;
	int		ident;
	char		data[0];
    } ext_t;
    ext_t		*out;

    assert(type & (PM_TEXT_HELP|PM_TEXT_ONELINE));
    assert(type & (PM_TEXT_PMID|PM_TEXT_INDOM));

    textlen = strlen(buffer) + 1;
    len = (int)sizeof(ext_t) + textlen + LENSIZE;

PM_FAULT_POINT("libpcp/" __FILE__ ":14", PM_FAULT_ALLOC);
    if ((out = (ext_t *)malloc(len)) == NULL)
	return -oserror();

    /* swab all output fields */
    out->hdr.len = htonl(len);
    out->hdr.type = htonl(TYPE_TEXT);
    out->type = htonl(type);
    out->ident = htonl(ident);

    /* copy in the actual text (ascii) */
    ptr = (char *) &out->data;
    memmove((void *)ptr, buffer, textlen);

    /* trailer length */
    ptr += textlen;
    memmove((void *)ptr, &out->hdr.len, sizeof(out->hdr.len));

    if ((sts = __pmFwrite(out, 1, len, lcp->l_mdfp)) != len) {
	char	errmsg[PM_MAXERRMSGLEN];

	pmprintf("__pmLogPutText(...ident,=%d,type=%d): write failed: returned %d expecting %d: %s\n",
		ident, type, sts, len, osstrerror_r(errmsg, sizeof(errmsg)));
	pmflush();
	free(out);
	return -oserror();
    }
    free(out);

    if (!cached)
	return 0;
    return addtext(acp, ident, type, buffer);
}

int
pmLookupInDomArchive(pmInDom indom, const char *name)
{
    int			n;
    int			j;
    __pmHashNode	*hp;
    __pmLogInDom	*idp;
    __pmContext		*ctxp;

    if (indom == PM_INDOM_NULL)
	return PM_ERR_INDOM;

    if ((n = pmWhichContext()) >= 0) {
	ctxp = __pmHandleToPtr(n);
	if (ctxp == NULL)
	    return PM_ERR_NOCONTEXT;
	if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	    PM_UNLOCK(ctxp->c_lock);
	    return PM_ERR_NOTARCHIVE;
	}

	if ((hp = __pmHashSearch((unsigned int)indom, &ctxp->c_archctl->ac_log->l_hashindom)) == NULL) {
	    PM_UNLOCK(ctxp->c_lock);
	    return PM_ERR_INDOM_LOG;
	}

	for (idp = (__pmLogInDom *)hp->data; idp != NULL; idp = idp->next) {
	    /* full match */
	    for (j = 0; j < idp->numinst; j++) {
		if (strcmp(name, idp->namelist[j]) == 0) {
		    PM_UNLOCK(ctxp->c_lock);
		    return idp->instlist[j];
		}
	    }
	    /* half-baked match to first space */
	    for (j = 0; j < idp->numinst; j++) {
		char	*p = idp->namelist[j];
		while (*p && *p != ' ')
		    p++;
		if (*p == ' ') {
		    if (strncmp(name, idp->namelist[j], p - idp->namelist[j]) == 0) {
			PM_UNLOCK(ctxp->c_lock);
			return idp->instlist[j];
		    }
		}
	    }
	}
	n = PM_ERR_INST_LOG;
	PM_UNLOCK(ctxp->c_lock);
    }

    return n;
}

int
pmNameInDomArchive(pmInDom indom, int inst, char **name)
{
    int			n;
    int			j;
    __pmHashNode	*hp;
    __pmLogInDom	*idp;
    __pmContext		*ctxp;

    if (indom == PM_INDOM_NULL)
	return PM_ERR_INDOM;

    if ((n = pmWhichContext()) >= 0) {
	ctxp = __pmHandleToPtr(n);
	if (ctxp == NULL)
	    return PM_ERR_NOCONTEXT;
	if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	    PM_UNLOCK(ctxp->c_lock);
	    return PM_ERR_NOTARCHIVE;
	}

	if ((hp = __pmHashSearch((unsigned int)indom, &ctxp->c_archctl->ac_log->l_hashindom)) == NULL) {
	    PM_UNLOCK(ctxp->c_lock);
	    return PM_ERR_INDOM_LOG;
	}

	for (idp = (__pmLogInDom *)hp->data; idp != NULL; idp = idp->next) {
	    for (j = 0; j < idp->numinst; j++) {
		if (idp->instlist[j] == inst) {
		    if ((*name = strdup(idp->namelist[j])) == NULL)
			n = -oserror();
		    else
			n = 0;
		    PM_UNLOCK(ctxp->c_lock);
		    return n;
		}
	    }
	}
	n = PM_ERR_INST_LOG;
	PM_UNLOCK(ctxp->c_lock);
    }

    return n;
}

/*
 * Indoms larger than HASH_THRESHOLD will use a hash table
 * to search the instance and name lists to be returned.
 * Smaller indoms will use the regular linear search.
 */
#define HASH_THRESHOLD	16
#define HASH_SIZE	509 /* prime */

static struct {
    int			len;
    int			max;
    int			*list;
} ihash[HASH_SIZE];

static int
find_add_ihash(int id)
{
    int			*tmp, j, i = id % HASH_SIZE; 

    for (j = 0; j < ihash[i].len; j++) {
	if (ihash[i].list[j] == id)
	    return 1;
    }
    ihash[i].len++;
    if (ihash[i].len >= ihash[i].max) {
	ihash[i].max += 8;
	tmp = (int *)realloc(ihash[i].list, ihash[i].max * sizeof(int));
	if (tmp)
	    ihash[i].list = tmp;
	else {
	    ihash[i].len = ihash[i].max = 0;
	    free(ihash[i].list);
	    ihash[i].list = NULL;
	}
    }
    if (ihash[i].list)
	ihash[i].list[ihash[i].len-1] = id;

    return 0;
}

static void
reset_ihash(void)
{
    int			i;

    /* invalidate all entries, but don't free the memory */
    for (i = 0; i < HASH_SIZE; i++)
    	ihash[i].len = 0;
}

/*
 * Internal variant of pmGetInDomArchive() ... ctxp is not NULL for
 * internal callers where the current context is already locked, but
 * NULL for callers from above the PMAPI or internal callers when the
 * current context is not locked.
 */
int
pmGetInDomArchive_ctx(__pmContext *ctxp, pmInDom indom, int **instlist, char ***namelist)
{
    char		*p;
    __pmHashNode	*hp;
    __pmLogInDom	*idp;
    size_t		bytes;
    int			n, i, j;
    int			numinst = 0;
    int			strsize = 0;
    int			*ilist = NULL;
    char		**nlist = NULL;
    char		**olist;
    int			big_indom = 0;
    int			need_unlock = 0;

    /* avoid ambiguity when no instances or errors */
    *instlist = NULL;
    *namelist = NULL;
    if (indom == PM_INDOM_NULL)
	return PM_ERR_INDOM;

    if (ctxp == NULL) {
	if ((n = pmWhichContext()) < 0)
	    return n;
	ctxp = __pmHandleToPtr(n);
	if (ctxp == NULL)
	    return PM_ERR_NOCONTEXT;
	need_unlock = 1;
    }
    else
	PM_ASSERT_IS_LOCKED(ctxp->c_lock);
    if (ctxp->c_type != PM_CONTEXT_ARCHIVE) {
	if (need_unlock)
	    PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_NOTARCHIVE;
    }

    if ((hp = __pmHashSearch((unsigned int)indom, &ctxp->c_archctl->ac_log->l_hashindom)) == NULL) {
	if (need_unlock)
	    PM_UNLOCK(ctxp->c_lock);
	return PM_ERR_INDOM_LOG;
    }

    for (idp = (__pmLogInDom *)hp->data; idp != NULL; idp = idp->next) {
	if (idp->numinst > HASH_THRESHOLD) {
	    big_indom = 1;
	    reset_ihash();
	    break;
	}
    }

    for (idp = (__pmLogInDom *)hp->data; idp != NULL; idp = idp->next) {
	for (j = 0; j < idp->numinst; j++) {
	    if (big_indom) {
		/* big indom - use a hash table */
		i = find_add_ihash(idp->instlist[j]) ? 0 : numinst;
	    }
	    else {
		/* small indom - linear search */
		for (i = 0; i < numinst; i++) {
		    if (idp->instlist[j] == ilist[i])
			break;
		}
	    }

	    if (i < numinst)
		continue;

	    bytes = (numinst + 1) * sizeof(ilist[0]);
PM_FAULT_POINT("libpcp/" __FILE__ ":7", PM_FAULT_ALLOC);
	    if ((ilist = (int *)realloc(ilist, bytes)) == NULL) {
		pmNoMem("pmGetInDomArchive: ilist", bytes, PM_FATAL_ERR);
	    }
	    bytes = (numinst + 1) * sizeof(nlist[0]);
PM_FAULT_POINT("libpcp/" __FILE__ ":8", PM_FAULT_ALLOC);
	    if ((nlist = (char **)realloc(nlist, bytes)) == NULL) {
		pmNoMem("pmGetInDomArchive: nlist", bytes, PM_FATAL_ERR);
	    }
	    ilist[numinst] = idp->instlist[j];
	    nlist[numinst] = idp->namelist[j];
	    strsize += strlen(idp->namelist[j])+1;
	    numinst++;
	}
    }
    bytes = numinst * sizeof(olist[0]) + strsize;
PM_FAULT_POINT("libpcp/" __FILE__ ":9", PM_FAULT_ALLOC);
    if ((olist = (char **)malloc(bytes)) == NULL) {
	pmNoMem("pmGetInDomArchive: olist", bytes, PM_FATAL_ERR);
    }
    p = (char *)olist;
    p += numinst * sizeof(olist[0]);
    for (i = 0; i < numinst; i++) {
	olist[i] = p;
	strcpy(p, nlist[i]);
	p += strlen(nlist[i]) + 1;
    }
    free(nlist);
    *instlist = ilist;
    *namelist = olist;
    n = numinst;

    if (need_unlock)
	PM_UNLOCK(ctxp->c_lock);

    return n;
}

int
pmGetInDomArchive(pmInDom indom, int **instlist, char ***namelist)
{
    return pmGetInDomArchive_ctx(NULL, indom, instlist, namelist);
}

void
__pmLogLoadTimestamp(const __int32_t *buf, __pmTimestamp *tsp)
{
    memcpy((void *)&tsp->sec, (void *)buf, 2*sizeof(__int32_t));
    tsp->nsec = buf[2];
    __ntohpmTimestamp(tsp);
    if (pmDebugOptions.logmeta && pmDebugOptions.desperate) {
	fprintf(stderr, "__pmLogLoadTimestamp: network(%08x%08x %08x nsec)", buf[0], buf[1], buf[2]);
	fprintf(stderr, " -> %" FMT_INT64 ".%09d (%llx %x nsec)\n", tsp->sec, tsp->nsec, (long long)tsp->sec, tsp->nsec);
    }
}

void
__pmLogLoadTimeval(const __int32_t *buf, __pmTimestamp *tsp)
{
    tsp->sec = ntohl(buf[0]);
    tsp->nsec = ntohl(buf[1]) * 1000;
    if (pmDebugOptions.logmeta && pmDebugOptions.desperate) {
	fprintf(stderr, "__pmLogLoadTimeval: network(%08x %08x usec)", buf[0], buf[1]);
	fprintf(stderr, " -> %" FMT_INT64 ".%09d (%llx %x nsec)\n", tsp->sec, tsp->nsec, (long long)tsp->sec, tsp->nsec);
    }
}

void
__pmLogPutTimestamp(const __pmTimestamp *tsp, __int32_t *buf)
{
    __pmTimestamp	stamp;
    stamp = *tsp;
    __htonpmTimestamp(&stamp);
    memcpy((void *)buf, (void *)&tsp->sec, 2*sizeof(__int32_t));
    buf[2] = tsp->nsec;
    if (pmDebugOptions.logmeta && pmDebugOptions.desperate) {
	fprintf(stderr, "__pmLogPutTimestamp: %" FMT_INT64 ".%09d (%llx %x nsec)", tsp->sec, tsp->nsec, (long long)tsp->sec, tsp->nsec);
	fprintf(stderr, " -> network(%08x%08x %08x nsec)\n", buf[0], buf[1], buf[2]);
    }
}

void
__pmLogPutTimeval(const __pmTimestamp *tsp, __int32_t *buf)
{
    buf[0] = htonl((__int32_t)tsp->sec);
    buf[1] = htonl(tsp->nsec / 1000);
    if (pmDebugOptions.logmeta && pmDebugOptions.desperate) {
	fprintf(stderr, "__pmLogPutTimeval: %" FMT_INT64 ".%09d (%llx %x nsec %x usec)", tsp->sec, tsp->nsec, (long long)tsp->sec, tsp->nsec, tsp->nsec / 1000);
	fprintf(stderr, " -> network(%08x %08x usec)\n", buf[0], buf[1]);
    }
}
