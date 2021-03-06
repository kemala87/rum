/*-------------------------------------------------------------------------
 *
 * rumutil.c
 *	  utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 2015-2016, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"

#include "rum.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

PG_FUNCTION_INFO_V1(rumhandler);

/* Kind of relation optioms for rum index */
static relopt_kind rum_relopt_kind;

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables. */
	DefineCustomIntVariable("rum_fuzzy_search_limit",
				  "Sets the maximum allowed result for exact search by RUM.",
							NULL,
							&RumFuzzySearchLimit,
							0, 0, INT_MAX,
							PGC_USERSET, 0,
							NULL, NULL, NULL);

	rum_relopt_kind = add_reloption_kind();

	add_string_reloption(rum_relopt_kind, "attach",
						 "Column name to attach as additional info",
						 NULL, NULL);
	add_string_reloption(rum_relopt_kind, "to",
						 "Column name to add a order by column",
						 NULL, NULL);
	add_bool_reloption(rum_relopt_kind, "order_by_attach",
			  "Use (addinfo, itempointer) order instead of just itempointer",
					   false);
}

/*
 * RUM handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
rumhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = RUMNProcs;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = true;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = rumbuild;
	amroutine->ambuildempty = rumbuildempty;
	amroutine->aminsert = ruminsert;
	amroutine->ambulkdelete = rumbulkdelete;
	amroutine->amvacuumcleanup = rumvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = gincostestimate;
	amroutine->amoptions = rumoptions;
	amroutine->amvalidate = rumvalidate;
	amroutine->ambeginscan = rumbeginscan;
	amroutine->amrescan = rumrescan;
	amroutine->amgettuple = rumgettuple;
	amroutine->amgetbitmap = rumgetbitmap;
	amroutine->amendscan = rumendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * initRumState: fill in an empty RumState struct to describe the index
 *
 * Note: assorted subsidiary data is allocated in the CurrentMemoryContext.
 */
void
initRumState(RumState * state, Relation index)
{
	TupleDesc	origTupdesc = RelationGetDescr(index);
	int			i;

	MemSet(state, 0, sizeof(RumState));

	state->index = index;
	state->isBuild = false;
	state->oneCol = (origTupdesc->natts == 1) ? true : false;
	state->origTupdesc = origTupdesc;

	state->attrnOrderByColumn = InvalidAttrNumber;
	state->attrnAddToColumn = InvalidAttrNumber;
	if (index->rd_options)
	{
		RumOptions *options = (RumOptions *) index->rd_options;

		if (options->orderByColumn > 0)
		{
			char	   *colname = (char *) options + options->orderByColumn;
			AttrNumber	attrnOrderByHeapColumn;

			attrnOrderByHeapColumn = get_attnum(index->rd_index->indrelid, colname);

			if (!AttributeNumberIsValid(attrnOrderByHeapColumn))
				elog(ERROR, "attribute \"%s\" is not found in table", colname);

			state->attrnOrderByColumn = get_attnum(index->rd_id, colname);

			if (!AttributeNumberIsValid(state->attrnOrderByColumn))
				elog(ERROR, "attribute \"%s\" is not found in index", colname);
		}

		if (options->addToColumn > 0)
		{
			char	   *colname = (char *) options + options->addToColumn;
			AttrNumber	attrnAddToHeapColumn;

			attrnAddToHeapColumn = get_attnum(index->rd_index->indrelid, colname);

			if (!AttributeNumberIsValid(attrnAddToHeapColumn))
				elog(ERROR, "attribute \"%s\" is not found in table", colname);

			state->attrnAddToColumn = get_attnum(index->rd_id, colname);

			if (!AttributeNumberIsValid(state->attrnAddToColumn))
				elog(ERROR, "attribute \"%s\" is not found in index", colname);
		}

		if (!(AttributeNumberIsValid(state->attrnOrderByColumn) &&
			  AttributeNumberIsValid(state->attrnAddToColumn)))
			elog(ERROR, "AddTo and OrderBy columns should be defined both");

		if (options->useAlternativeOrder)
		{
			if (!(AttributeNumberIsValid(state->attrnOrderByColumn) &&
				  AttributeNumberIsValid(state->attrnAddToColumn)))
				elog(ERROR, "to use alternative ordering AddTo and OrderBy should be defined");

			state->useAlternativeOrder = true;
		}
	}

	for (i = 0; i < origTupdesc->natts; i++)
	{
		RumConfig	*rumConfig = state->rumConfig + i;

		rumConfig->addInfoTypeOid = InvalidOid;

		if (index_getprocid(index, i + 1, RUM_CONFIG_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->configFn[i]),
						   index_getprocinfo(index, i + 1, RUM_CONFIG_PROC),
						   CurrentMemoryContext);

			FunctionCall1(&state->configFn[i], PointerGetDatum(rumConfig));
		}

		if (state->attrnAddToColumn == i + 1)
		{
			if (OidIsValid(rumConfig->addInfoTypeOid))
				elog(ERROR, "AddTo could should not have AddInfo");

			rumConfig->addInfoTypeOid = origTupdesc->attrs[
									state->attrnOrderByColumn - 1]->atttypid;
		}

		if (state->oneCol)
		{
			state->tupdesc[i] = CreateTemplateTupleDesc(
						OidIsValid(rumConfig->addInfoTypeOid) ? 2 : 1, false);
			TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 1, NULL,
							   origTupdesc->attrs[i]->atttypid,
							   origTupdesc->attrs[i]->atttypmod,
							   origTupdesc->attrs[i]->attndims);
			TupleDescInitEntryCollation(state->tupdesc[i], (AttrNumber) 1,
										origTupdesc->attrs[i]->attcollation);
			if (OidIsValid(rumConfig->addInfoTypeOid))
			{
				TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 2, NULL,
								   rumConfig->addInfoTypeOid, -1, 0);
				state->addAttrs[i] = state->tupdesc[i]->attrs[1];
			}
			else
			{
				state->addAttrs[i] = NULL;
			}
		}
		else
		{
			state->tupdesc[i] = CreateTemplateTupleDesc(
						OidIsValid(rumConfig->addInfoTypeOid) ? 3 : 2, false);
			TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 1, NULL,
							   INT2OID, -1, 0);
			TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 2, NULL,
							   origTupdesc->attrs[i]->atttypid,
							   origTupdesc->attrs[i]->atttypmod,
							   origTupdesc->attrs[i]->attndims);
			TupleDescInitEntryCollation(state->tupdesc[i], (AttrNumber) 2,
										origTupdesc->attrs[i]->attcollation);
			if (OidIsValid(rumConfig->addInfoTypeOid))
			{
				TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 3, NULL,
								   rumConfig->addInfoTypeOid, -1, 0);
				state->addAttrs[i] = state->tupdesc[i]->attrs[2];
			}
			else
			{
				state->addAttrs[i] = NULL;
			}
		}

		fmgr_info_copy(&(state->compareFn[i]),
					   index_getprocinfo(index, i + 1, GIN_COMPARE_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->extractValueFn[i]),
					   index_getprocinfo(index, i + 1, GIN_EXTRACTVALUE_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->extractQueryFn[i]),
					   index_getprocinfo(index, i + 1, GIN_EXTRACTQUERY_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->consistentFn[i]),
					   index_getprocinfo(index, i + 1, GIN_CONSISTENT_PROC),
					   CurrentMemoryContext);

		/*
		 * Check opclass capability to do partial match.
		 */
		if (index_getprocid(index, i + 1, GIN_COMPARE_PARTIAL_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->comparePartialFn[i]),
				   index_getprocinfo(index, i + 1, GIN_COMPARE_PARTIAL_PROC),
						   CurrentMemoryContext);
			state->canPartialMatch[i] = true;
		}
		else
		{
			state->canPartialMatch[i] = false;
		}

		/*
		 * Check opclass capability to do pre consistent check.
		 */
		if (index_getprocid(index, i + 1, RUM_PRE_CONSISTENT_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->preConsistentFn[i]),
					index_getprocinfo(index, i + 1, RUM_PRE_CONSISTENT_PROC),
						   CurrentMemoryContext);
			state->canPreConsistent[i] = true;
		}
		else
		{
			state->canPreConsistent[i] = false;
		}

		/*
		 * Check opclass capability to do order by.
		 */
		if (index_getprocid(index, i + 1, RUM_ORDERING_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->orderingFn[i]),
						   index_getprocinfo(index, i + 1, RUM_ORDERING_PROC),
						   CurrentMemoryContext);
			state->canOrdering[i] = true;
		}
		else
		{
			state->canOrdering[i] = false;
		}

		if (index_getprocid(index, i + 1, RUM_OUTER_ORDERING_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->outerOrderingFn[i]),
					index_getprocinfo(index, i + 1, RUM_OUTER_ORDERING_PROC),
						   CurrentMemoryContext);
			state->canOuterOrdering[i] = true;
		}
		else
		{
			state->canOuterOrdering[i] = false;
		}

		/*
		 * If the index column has a specified collation, we should honor that
		 * while doing comparisons.  However, we may have a collatable storage
		 * type for a noncollatable indexed data type (for instance, hstore
		 * uses text index entries).  If there's no index collation then
		 * specify default collation in case the support functions need
		 * collation.  This is harmless if the support functions don't care
		 * about collation, so we just do it unconditionally.  (We could
		 * alternatively call get_typcollation, but that seems like expensive
		 * overkill --- there aren't going to be any cases where a RUM storage
		 * type has a nondefault collation.)
		 */
		if (OidIsValid(index->rd_indcollation[i]))
			state->supportCollation[i] = index->rd_indcollation[i];
		else
			state->supportCollation[i] = DEFAULT_COLLATION_OID;
	}

	if (AttributeNumberIsValid(state->attrnOrderByColumn))
	{
		/* Follow FIXME comment(s) to understand */
		if (origTupdesc->attrs[state->attrnOrderByColumn - 1]->attbyval == false)
			elog(ERROR, "currently, RUM doesn't support order by over pass-by-reference column");
	}
}

/*
 * Extract attribute (column) number of stored entry from RUM tuple
 */
OffsetNumber
rumtuple_get_attrnum(RumState * rumstate, IndexTuple tuple)
{
	OffsetNumber colN;

	if (rumstate->oneCol)
	{
		/* column number is not stored explicitly */
		colN = FirstOffsetNumber;
	}
	else
	{
		Datum		res;
		bool		isnull;

		/*
		 * First attribute is always int16, so we can safely use any tuple
		 * descriptor to obtain first attribute of tuple
		 */
		res = index_getattr(tuple, FirstOffsetNumber, rumstate->tupdesc[0],
							&isnull);
		Assert(!isnull);

		colN = DatumGetUInt16(res);
		Assert(colN >= FirstOffsetNumber && colN <= rumstate->origTupdesc->natts);
	}

	return colN;
}

/*
 * Extract stored datum (and possible null category) from RUM tuple
 */
Datum
rumtuple_get_key(RumState * rumstate, IndexTuple tuple,
				 RumNullCategory * category)
{
	Datum		res;
	bool		isnull;

	if (rumstate->oneCol)
	{
		/*
		 * Single column index doesn't store attribute numbers in tuples
		 */
		res = index_getattr(tuple, FirstOffsetNumber, rumstate->origTupdesc,
							&isnull);
	}
	else
	{
		/*
		 * Since the datum type depends on which index column it's from, we
		 * must be careful to use the right tuple descriptor here.
		 */
		OffsetNumber colN = rumtuple_get_attrnum(rumstate, tuple);

		res = index_getattr(tuple, OffsetNumberNext(FirstOffsetNumber),
							rumstate->tupdesc[colN - 1],
							&isnull);
	}

	if (isnull)
		*category = RumGetNullCategory(tuple, rumstate);
	else
		*category = RUM_CAT_NORM_KEY;

	return res;
}

/*
 * Allocate a new page (either by recycling, or by extending the index file)
 * The returned buffer is already pinned and exclusive-locked
 * Caller is responsible for initializing the page by calling RumInitBuffer
 */
Buffer
RumNewBuffer(Relation index)
{
	Buffer		buffer;
	bool		needLock;

	/* First, try to get a page from FSM */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBuffer(index, blkno);

		/*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			if (PageIsNew(page))
				return buffer;	/* OK to use, if never initialized */

			if (RumPageIsDeleted(page))
				return buffer;	/* OK to use */

			LockBuffer(buffer, RUM_UNLOCK);
		}

		/* Can't use it, so release buffer and try again */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file */
	needLock = !RELATION_IS_LOCAL(index);
	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);

	buffer = ReadBuffer(index, P_NEW);
	LockBuffer(buffer, RUM_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	return buffer;
}

void
RumInitPage(Page page, uint32 f, Size pageSize)
{
	RumPageOpaque opaque;

	PageInit(page, pageSize, sizeof(RumPageOpaqueData));

	opaque = RumPageGetOpaque(page);
	memset(opaque, 0, sizeof(RumPageOpaqueData));
	opaque->flags = f;
	opaque->leftlink = InvalidBlockNumber;
	opaque->rightlink = InvalidBlockNumber;
}

void
RumInitBuffer(GenericXLogState *state, Buffer buffer, uint32 flags,
			  bool isBuild)
{
	Page		page;

	if (isBuild)
		page = BufferGetPage(buffer);
	else
		page = GenericXLogRegisterBuffer(state, buffer,
										 GENERIC_XLOG_FULL_IMAGE);

	RumInitPage(page, flags, BufferGetPageSize(buffer));
}

void
RumInitMetabuffer(GenericXLogState *state, Buffer metaBuffer, bool isBuild)
{
	Page		metaPage;
	RumMetaPageData *metadata;

	/* Initialize contents of meta page */
	if (isBuild)
		metaPage = BufferGetPage(metaBuffer);
	else
		metaPage = GenericXLogRegisterBuffer(state, metaBuffer,
											 GENERIC_XLOG_FULL_IMAGE);

	RumInitPage(metaPage, RUM_META, BufferGetPageSize(metaBuffer));
	metadata = RumPageGetMeta(metaPage);
	memset(metadata, 0, sizeof(RumMetaPageData));

	metadata->head = metadata->tail = InvalidBlockNumber;
	metadata->tailFreeSize = 0;
	metadata->nPendingPages = 0;
	metadata->nPendingHeapTuples = 0;
	metadata->nTotalPages = 0;
	metadata->nEntryPages = 0;
	metadata->nDataPages = 0;
	metadata->nEntries = 0;
	metadata->rumVersion = RUM_CURRENT_VERSION;

	((PageHeader) metaPage)->pd_lower += sizeof(RumMetaPageData);
}

/*
 * Compare two keys of the same index column
 */
int
rumCompareEntries(RumState * rumstate, OffsetNumber attnum,
				  Datum a, RumNullCategory categorya,
				  Datum b, RumNullCategory categoryb)
{
	/* if not of same null category, sort by that first */
	if (categorya != categoryb)
		return (categorya < categoryb) ? -1 : 1;

	/* all null items in same category are equal */
	if (categorya != RUM_CAT_NORM_KEY)
		return 0;

	/* both not null, so safe to call the compareFn */
	return DatumGetInt32(FunctionCall2Coll(&rumstate->compareFn[attnum - 1],
									  rumstate->supportCollation[attnum - 1],
										   a, b));
}

/*
 * Compare two keys of possibly different index columns
 */
int
rumCompareAttEntries(RumState * rumstate,
					 OffsetNumber attnuma, Datum a, RumNullCategory categorya,
					 OffsetNumber attnumb, Datum b, RumNullCategory categoryb)
{
	/* attribute number is the first sort key */
	if (attnuma != attnumb)
		return (attnuma < attnumb) ? -1 : 1;

	return rumCompareEntries(rumstate, attnuma, a, categorya, b, categoryb);
}


/*
 * Support for sorting key datums in rumExtractEntries
 *
 * Note: we only have to worry about null and not-null keys here;
 * rumExtractEntries never generates more than one placeholder null,
 * so it doesn't have to sort those.
 */
typedef struct
{
	Datum		datum;
	Datum		addInfo;
	bool		isnull;
	bool		addInfoIsNull;
} keyEntryData;

typedef struct
{
	FmgrInfo   *cmpDatumFunc;
	Oid			collation;
	bool		haveDups;
} cmpEntriesArg;

static int
cmpEntries(const void *a, const void *b, void *arg)
{
	const keyEntryData *aa = (const keyEntryData *) a;
	const keyEntryData *bb = (const keyEntryData *) b;
	cmpEntriesArg *data = (cmpEntriesArg *) arg;
	int			res;

	if (aa->isnull)
	{
		if (bb->isnull)
			res = 0;			/* NULL "=" NULL */
		else
			res = 1;			/* NULL ">" not-NULL */
	}
	else if (bb->isnull)
		res = -1;				/* not-NULL "<" NULL */
	else
		res = DatumGetInt32(FunctionCall2Coll(data->cmpDatumFunc,
											  data->collation,
											  aa->datum, bb->datum));

	/*
	 * Detect if we have any duplicates.  If there are equal keys, qsort must
	 * compare them at some point, else it wouldn't know whether one should go
	 * before or after the other.
	 */
	if (res == 0)
		data->haveDups = true;

	return res;
}


/*
 * Extract the index key values from an indexable item
 *
 * The resulting key values are sorted, and any duplicates are removed.
 * This avoids generating redundant index entries.
 */
Datum *
rumExtractEntries(RumState * rumstate, OffsetNumber attnum,
				  Datum value, bool isNull,
				  int32 *nentries, RumNullCategory ** categories,
				  Datum **addInfo, bool **addInfoIsNull)
{
	Datum	   *entries;
	bool	   *nullFlags;
	int32		i;

	/*
	 * We don't call the extractValueFn on a null item.  Instead generate a
	 * placeholder.
	 */
	if (isNull)
	{
		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		entries[0] = (Datum) 0;
		*addInfo = (Datum *) palloc(sizeof(Datum));
		(*addInfo)[0] = (Datum) 0;
		*addInfoIsNull = (bool *) palloc(sizeof(bool));
		(*addInfoIsNull)[0] = true;
		*categories = (RumNullCategory *) palloc(sizeof(RumNullCategory));
		(*categories)[0] = RUM_CAT_NULL_ITEM;
		return entries;
	}

	/* OK, call the opclass's extractValueFn */
	nullFlags = NULL;			/* in case extractValue doesn't set it */
	*addInfo = NULL;
	*addInfoIsNull = NULL;
	entries = (Datum *)
		DatumGetPointer(FunctionCall5Coll(&rumstate->extractValueFn[attnum - 1],
									  rumstate->supportCollation[attnum - 1],
										  value,
										  PointerGetDatum(nentries),
										  PointerGetDatum(&nullFlags),
										  PointerGetDatum(addInfo),
										  PointerGetDatum(addInfoIsNull)
										  ));

	/*
	 * Generate a placeholder if the item contained no keys.
	 */
	if (entries == NULL || *nentries <= 0)
	{
		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		entries[0] = (Datum) 0;
		*addInfo = (Datum *) palloc(sizeof(Datum));
		(*addInfo)[0] = (Datum) 0;
		*addInfoIsNull = (bool *) palloc(sizeof(bool));
		(*addInfoIsNull)[0] = true;
		*categories = (RumNullCategory *) palloc(sizeof(RumNullCategory));
		(*categories)[0] = RUM_CAT_EMPTY_ITEM;
		return entries;
	}

	if (!(*addInfo))
	{
		(*addInfo) = (Datum *) palloc(sizeof(Datum) * *nentries);
		for (i = 0; i < *nentries; i++)
			(*addInfo)[i] = (Datum) 0;
	}
	if (!(*addInfoIsNull))
	{
		(*addInfoIsNull) = (bool *) palloc(sizeof(bool) * *nentries);
		for (i = 0; i < *nentries; i++)
			(*addInfoIsNull)[i] = true;
	}

	/*
	 * If the extractValueFn didn't create a nullFlags array, create one,
	 * assuming that everything's non-null.  Otherwise, run through the array
	 * and make sure each value is exactly 0 or 1; this ensures binary
	 * compatibility with the RumNullCategory representation.
	 */
	if (nullFlags == NULL)
		nullFlags = (bool *) palloc0(*nentries * sizeof(bool));
	else
	{
		for (i = 0; i < *nentries; i++)
			nullFlags[i] = (nullFlags[i] ? true : false);
	}
	/* now we can use the nullFlags as category codes */
	*categories = (RumNullCategory *) nullFlags;

	/*
	 * If there's more than one key, sort and unique-ify.
	 *
	 * XXX Using qsort here is notationally painful, and the overhead is
	 * pretty bad too.  For small numbers of keys it'd likely be better to use
	 * a simple insertion sort.
	 */
	if (*nentries > 1)
	{
		keyEntryData *keydata;
		cmpEntriesArg arg;

		keydata = (keyEntryData *) palloc(*nentries * sizeof(keyEntryData));
		for (i = 0; i < *nentries; i++)
		{
			keydata[i].datum = entries[i];
			keydata[i].isnull = nullFlags[i];
			keydata[i].addInfo = (*addInfo)[i];
			keydata[i].addInfoIsNull = (*addInfoIsNull)[i];
		}

		arg.cmpDatumFunc = &rumstate->compareFn[attnum - 1];
		arg.collation = rumstate->supportCollation[attnum - 1];
		arg.haveDups = false;
		qsort_arg(keydata, *nentries, sizeof(keyEntryData),
				  cmpEntries, (void *) &arg);

		if (arg.haveDups)
		{
			/* there are duplicates, must get rid of 'em */
			int32		j;

			entries[0] = keydata[0].datum;
			nullFlags[0] = keydata[0].isnull;
			(*addInfo)[0] = keydata[0].addInfo;
			(*addInfoIsNull)[0] = keydata[0].addInfoIsNull;
			j = 1;
			for (i = 1; i < *nentries; i++)
			{
				if (cmpEntries(&keydata[i - 1], &keydata[i], &arg) != 0)
				{
					entries[j] = keydata[i].datum;
					nullFlags[j] = keydata[i].isnull;
					(*addInfo)[j] = keydata[i].addInfo;
					(*addInfoIsNull)[j] = keydata[i].addInfoIsNull;
					j++;
				}
			}
			*nentries = j;
		}
		else
		{
			/* easy, no duplicates */
			for (i = 0; i < *nentries; i++)
			{
				entries[i] = keydata[i].datum;
				nullFlags[i] = keydata[i].isnull;
				(*addInfo)[i] = keydata[i].addInfo;
				(*addInfoIsNull)[i] = keydata[i].addInfoIsNull;
			}
		}

		pfree(keydata);
	}

	return entries;
}

bytea *
rumoptions(Datum reloptions, bool validate)
{
	relopt_value *options;
	RumOptions *rdopts;
	int			numoptions;
	static const relopt_parse_elt tab[] = {
		{"fastupdate", RELOPT_TYPE_BOOL, offsetof(RumOptions, useFastUpdate)},
		{"attach", RELOPT_TYPE_STRING, offsetof(RumOptions, orderByColumn)},
		{"to", RELOPT_TYPE_STRING, offsetof(RumOptions, addToColumn)},
		{"order_by_attach", RELOPT_TYPE_BOOL, offsetof(RumOptions, useAlternativeOrder)}
	};

	options = parseRelOptions(reloptions, validate, rum_relopt_kind,
							  &numoptions);

	/* if none set, we're done */
	if (numoptions == 0)
		return NULL;

	rdopts = allocateReloptStruct(sizeof(RumOptions), options, numoptions);

	fillRelOptions((void *) rdopts, sizeof(RumOptions), options, numoptions,
				   validate, tab, lengthof(tab));

	pfree(options);

	return (bytea *) rdopts;
}

/*
 * Fetch index's statistical data into *stats
 *
 * Note: in the result, nPendingPages can be trusted to be up-to-date,
 * as can rumVersion; but the other fields are as of the last VACUUM.
 */
void
rumGetStats(Relation index, GinStatsData *stats)
{
	Buffer		metabuffer;
	Page		metapage;
	RumMetaPageData *metadata;

	metabuffer = ReadBuffer(index, RUM_METAPAGE_BLKNO);
	LockBuffer(metabuffer, RUM_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = RumPageGetMeta(metapage);

	stats->nPendingPages = metadata->nPendingPages;
	stats->nTotalPages = metadata->nTotalPages;
	stats->nEntryPages = metadata->nEntryPages;
	stats->nDataPages = metadata->nDataPages;
	stats->nEntries = metadata->nEntries;
	stats->ginVersion = metadata->rumVersion;

	if (stats->ginVersion != RUM_CURRENT_VERSION)
		elog(ERROR, "unexpected RUM index version. Reindex");

	UnlockReleaseBuffer(metabuffer);
}

/*
 * Write the given statistics to the index's metapage
 *
 * Note: nPendingPages and rumVersion are *not* copied over
 */
void
rumUpdateStats(Relation index, const GinStatsData *stats, bool isBuild)
{
	Buffer		metaBuffer;
	Page		metapage;
	RumMetaPageData *metadata;
	GenericXLogState *state;

	metaBuffer = ReadBuffer(index, RUM_METAPAGE_BLKNO);
	LockBuffer(metaBuffer, RUM_EXCLUSIVE);
	if (isBuild)
	{
		metapage = BufferGetPage(metaBuffer);
		START_CRIT_SECTION();
	}
	else
	{
		state = GenericXLogStart(index);
		metapage = GenericXLogRegisterBuffer(state, metaBuffer, 0);
	}
	metadata = RumPageGetMeta(metapage);

	metadata->nTotalPages = stats->nTotalPages;
	metadata->nEntryPages = stats->nEntryPages;
	metadata->nDataPages = stats->nDataPages;
	metadata->nEntries = stats->nEntries;

	if (isBuild)
		MarkBufferDirty(metaBuffer);
	else
		GenericXLogFinish(state);

	UnlockReleaseBuffer(metaBuffer);

	if (isBuild)
		END_CRIT_SECTION();
}

Datum
FunctionCall10Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				   Datum arg3, Datum arg4, Datum arg5,
				   Datum arg6, Datum arg7, Datum arg8,
				   Datum arg9, Datum arg10)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 10, collation, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;
	fcinfo.arg[9] = arg10;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;
	fcinfo.argnull[8] = false;
	fcinfo.argnull[9] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}
