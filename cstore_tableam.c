#include "postgres.h"

#include <math.h>

#include "miscadmin.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/rewriteheap.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "cstore.h"
#include "cstore_tableam.h"

#define CSTORE_TABLEAM_NAME "cstore_tableam"

typedef struct CStoreScanDescData
{
	TableScanDescData cs_base;
	TableReadState *cs_readState;
} CStoreScanDescData;

typedef struct CStoreScanDescData *CStoreScanDesc;

static TableWriteState *CStoreWriteState = NULL;
static ExecutorEnd_hook_type PreviousExecutorEndHook = NULL;
static MemoryContext CStoreContext = NULL;
static object_access_hook_type prevObjectAccessHook = NULL;

/* forward declaration for static functions */
static void CStoreTableAMObjectAccessHook(ObjectAccessType access, Oid classId, Oid
										  objectId, int subId,
										  void *arg);
static bool IsCStoreTableAmTable(Oid relationId);

static CStoreOptions *
CStoreTableAMGetOptions(void)
{
	CStoreOptions *cstoreOptions = palloc0(sizeof(CStoreOptions));
	cstoreOptions->compressionType = cstore_compression;
	cstoreOptions->stripeRowCount = cstore_stripe_row_count;
	cstoreOptions->blockRowCount = cstore_block_row_count;
	return cstoreOptions;
}


static MemoryContext
GetCStoreMemoryContext()
{
	if (CStoreContext == NULL)
	{
		CStoreContext = AllocSetContextCreate(TopMemoryContext, "cstore context",
											  ALLOCSET_DEFAULT_SIZES);
	}
	return CStoreContext;
}


static void
ResetCStoreMemoryContext()
{
	if (CStoreContext != NULL)
	{
		MemoryContextReset(CStoreContext);
	}
}


static void
cstore_init_write_state(Relation relation)
{
	/*TODO: upgrade lock to serialize writes */

	if (CStoreWriteState != NULL)
	{
		/* TODO: consider whether it's possible for a new write to start */
		/* before an old one is flushed */
		Assert(CStoreWriteState->relation->rd_id == relation->rd_id);
	}

	if (CStoreWriteState == NULL)
	{
		CStoreOptions *cstoreOptions = CStoreTableAMGetOptions();
		TupleDesc tupdesc = RelationGetDescr(relation);

		elog(LOG, "initializing write state for relation %d", relation->rd_id);
		CStoreWriteState = CStoreBeginWrite(relation,
											cstoreOptions->compressionType,
											cstoreOptions->stripeRowCount,
											cstoreOptions->blockRowCount,
											tupdesc);
	}
}


static void
cstore_free_write_state()
{
	if (CStoreWriteState != NULL)
	{
		elog(LOG, "flushing write state for relation %d",
			 CStoreWriteState->relation->rd_id);
		CStoreEndWrite(CStoreWriteState);
		CStoreWriteState = NULL;
	}
}


static List *
RelationColumnList(Relation rel)
{
	List *columnList = NIL;
	TupleDesc tupdesc = RelationGetDescr(rel);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Index varno = 0;
		AttrNumber varattno = i + 1;
		Oid vartype = tupdesc->attrs[i].atttypid;
		int32 vartypmod = 0;
		Oid varcollid = 0;
		Index varlevelsup = 0;
		Var *var;

		if (tupdesc->attrs[i].attisdropped)
		{
			continue;
		}

		var = makeVar(varno, varattno, vartype, vartypmod,
					  varcollid, varlevelsup);
		columnList = lappend(columnList, var);
	}

	return columnList;
}


static const TupleTableSlotOps *
cstore_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}


static TableScanDesc
cstore_beginscan(Relation relation, Snapshot snapshot,
				 int nkeys, ScanKey key,
				 ParallelTableScanDesc parallel_scan,
				 uint32 flags)
{
	TupleDesc tupdesc = relation->rd_att;
	TableReadState *readState = NULL;
	CStoreScanDesc scan = palloc(sizeof(CStoreScanDescData));
	List *columnList = NIL;
	MemoryContext oldContext = MemoryContextSwitchTo(GetCStoreMemoryContext());

	scan->cs_base.rs_rd = relation;
	scan->cs_base.rs_snapshot = snapshot;
	scan->cs_base.rs_nkeys = nkeys;
	scan->cs_base.rs_key = key;
	scan->cs_base.rs_flags = flags;
	scan->cs_base.rs_parallel = parallel_scan;

	columnList = RelationColumnList(relation);

	readState = CStoreBeginRead(relation, tupdesc, columnList, NULL);

	scan->cs_readState = readState;

	MemoryContextSwitchTo(oldContext);
	return ((TableScanDesc) scan);
}


static void
cstore_endscan(TableScanDesc sscan)
{
	CStoreScanDesc scan = (CStoreScanDesc) sscan;
	CStoreEndRead(scan->cs_readState);
	scan->cs_readState = NULL;
}


static void
cstore_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
			  bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	elog(ERROR, "cstore_rescan not implemented");
}


static bool
cstore_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	CStoreScanDesc scan = (CStoreScanDesc) sscan;
	bool nextRowFound;
	MemoryContext oldContext = MemoryContextSwitchTo(GetCStoreMemoryContext());

	ExecClearTuple(slot);

	nextRowFound = CStoreReadNextRow(scan->cs_readState, slot->tts_values,
									 slot->tts_isnull);

	MemoryContextSwitchTo(oldContext);

	if (!nextRowFound)
	{
		return false;
	}

	ExecStoreVirtualTuple(slot);
	return true;
}


static Size
cstore_parallelscan_estimate(Relation rel)
{
	elog(ERROR, "cstore_parallelscan_estimate not implemented");
}


static Size
cstore_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	elog(ERROR, "cstore_parallelscan_initialize not implemented");
}


static void
cstore_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	elog(ERROR, "cstore_parallelscan_reinitialize not implemented");
}


static IndexFetchTableData *
cstore_index_fetch_begin(Relation rel)
{
	elog(ERROR, "cstore_index_fetch_begin not implemented");
}


static void
cstore_index_fetch_reset(IndexFetchTableData *scan)
{
	elog(ERROR, "cstore_index_fetch_reset not implemented");
}


static void
cstore_index_fetch_end(IndexFetchTableData *scan)
{
	elog(ERROR, "cstore_index_fetch_end not implemented");
}


static bool
cstore_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	elog(ERROR, "cstore_index_fetch_tuple not implemented");
}


static bool
cstore_fetch_row_version(Relation relation,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot)
{
	elog(ERROR, "cstore_fetch_row_version not implemented");
}


static void
cstore_get_latest_tid(TableScanDesc sscan,
					  ItemPointer tid)
{
	elog(ERROR, "cstore_get_latest_tid not implemented");
}


static bool
cstore_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	elog(ERROR, "cstore_tuple_tid_valid not implemented");
}


static bool
cstore_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	return true;
}


static TransactionId
cstore_compute_xid_horizon_for_tuples(Relation rel,
									  ItemPointerData *tids,
									  int nitems)
{
	elog(ERROR, "cstore_compute_xid_horizon_for_tuples not implemented");
}


static void
cstore_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	HeapTuple heapTuple;
	MemoryContext oldContext = MemoryContextSwitchTo(GetCStoreMemoryContext());

	cstore_init_write_state(relation);

	heapTuple = ExecCopySlotHeapTuple(slot);
	if (HeapTupleHasExternal(heapTuple))
	{
		/* detoast any toasted attributes */
		HeapTuple newTuple = toast_flatten_tuple(heapTuple,
												 slot->tts_tupleDescriptor);

		ExecForceStoreHeapTuple(newTuple, slot, true);
	}

	slot_getallattrs(slot);

	CStoreWriteRow(CStoreWriteState, slot->tts_values, slot->tts_isnull);
	MemoryContextSwitchTo(oldContext);
}


static void
cstore_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								CommandId cid, int options,
								BulkInsertState bistate, uint32 specToken)
{
	elog(ERROR, "cstore_tuple_insert_speculative not implemented");
}


static void
cstore_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	elog(ERROR, "cstore_tuple_complete_speculative not implemented");
}


static void
cstore_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
					CommandId cid, int options, BulkInsertState bistate)
{
	MemoryContext oldContext = MemoryContextSwitchTo(GetCStoreMemoryContext());

	cstore_init_write_state(relation);

	for (int i = 0; i < ntuples; i++)
	{
		TupleTableSlot *tupleSlot = slots[i];
		HeapTuple heapTuple = ExecCopySlotHeapTuple(tupleSlot);

		if (HeapTupleHasExternal(heapTuple))
		{
			/* detoast any toasted attributes */
			HeapTuple newTuple = toast_flatten_tuple(heapTuple,
													 tupleSlot->tts_tupleDescriptor);

			ExecForceStoreHeapTuple(newTuple, tupleSlot, true);
		}

		slot_getallattrs(tupleSlot);

		CStoreWriteRow(CStoreWriteState, tupleSlot->tts_values, tupleSlot->tts_isnull);
	}
	MemoryContextSwitchTo(oldContext);
}


static TM_Result
cstore_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changingPart)
{
	elog(ERROR, "cstore_tuple_delete not implemented");
}


static TM_Result
cstore_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, bool *update_indexes)
{
	elog(ERROR, "cstore_tuple_update not implemented");
}


static TM_Result
cstore_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	elog(ERROR, "cstore_tuple_lock not implemented");
}


static void
cstore_finish_bulk_insert(Relation relation, int options)
{
	/*TODO: flush relation like for heap? */
	/* free write state or only in ExecutorEnd_hook? */

	/* for COPY */
	cstore_free_write_state();
}


static void
cstore_relation_set_new_filenode(Relation rel,
								 const RelFileNode *newrnode,
								 char persistence,
								 TransactionId *freezeXid,
								 MultiXactId *minmulti)
{
	SMgrRelation srel;
	DataFileMetadata *metadata = ReadDataFileMetadata(rel->rd_node.relNode, true);
	uint64 blockRowCount = 0;

	if (metadata != NULL)
	{
		/* existing table (e.g. TRUNCATE), use existing blockRowCount */
		blockRowCount = metadata->blockRowCount;
	}
	else
	{
		/* new table, use options */
		CStoreOptions *options = CStoreTableAMGetOptions();
		blockRowCount = options->blockRowCount;
	}

	/* delete old relfilenode metadata */
	DeleteDataFileMetadataRowIfExists(rel->rd_node.relNode);

	Assert(persistence == RELPERSISTENCE_PERMANENT);
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	srel = RelationCreateStorage(*newrnode, persistence);
	InitCStoreDataFileMetadata(newrnode->relNode, blockRowCount);
	smgrclose(srel);
}


static void
cstore_relation_nontransactional_truncate(Relation rel)
{
	DataFileMetadata *metadata = ReadDataFileMetadata(rel->rd_node.relNode, false);

	/*
	 * No need to set new relfilenode, since the table was created in this
	 * transaction and no other transaction can see this relation yet. We
	 * can just truncate the relation.
	 *
	 * This is similar to what is done in heapam_relation_nontransactional_truncate.
	 */
	RelationTruncate(rel, 0);

	/* Delete old relfilenode metadata and recreate it */
	DeleteDataFileMetadataRowIfExists(rel->rd_node.relNode);
	InitCStoreDataFileMetadata(rel->rd_node.relNode, metadata->blockRowCount);
}


static void
cstore_relation_copy_data(Relation rel, const RelFileNode *newrnode)
{
	elog(ERROR, "cstore_relation_copy_data not implemented");
}


/*
 * cstore_relation_copy_for_cluster is called on VACUUM FULL, at which
 * we should copy data from OldHeap to NewHeap.
 *
 * In general TableAM case this can also be called for the CLUSTER command
 * which is not applicable for cstore since it doesn't support indexes.
 */
static void
cstore_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples,
								 double *tups_vacuumed,
								 double *tups_recently_dead)
{
	TableWriteState *writeState = NULL;
	TableReadState *readState = NULL;
	CStoreOptions *cstoreOptions = NULL;
	Datum *sourceValues = NULL;
	bool *sourceNulls = NULL;
	Datum *targetValues = NULL;
	bool *targetNulls = NULL;
	TupleDesc sourceDesc = RelationGetDescr(OldHeap);
	TupleDesc targetDesc = RelationGetDescr(NewHeap);

	if (OldIndex != NULL || use_sort)
	{
		ereport(ERROR, (errmsg("cstore_am doesn't support indexes")));
	}

	/*
	 * copy_table_data in cluster.c assumes tuple descriptors are exactly
	 * the same. Even dropped columns exist and are marked as attisdropped
	 * in the target relation.
	 */
	Assert(sourceDesc->natts == targetDesc->natts);

	cstoreOptions = CStoreTableAMGetOptions();

	writeState = CStoreBeginWrite(NewHeap,
								  cstoreOptions->compressionType,
								  cstoreOptions->stripeRowCount,
								  cstoreOptions->blockRowCount,
								  targetDesc);

	readState = CStoreBeginRead(OldHeap, sourceDesc, RelationColumnList(OldHeap), NULL);

	sourceValues = palloc0(sourceDesc->natts * sizeof(Datum));
	sourceNulls = palloc0(sourceDesc->natts * sizeof(bool));

	targetValues = palloc0(targetDesc->natts * sizeof(Datum));
	targetNulls = palloc0(targetDesc->natts * sizeof(bool));

	*num_tuples = 0;

	while (CStoreReadNextRow(readState, sourceValues, sourceNulls))
	{
		memset(targetNulls, true, targetDesc->natts * sizeof(bool));

		for (int attrIndex = 0; attrIndex < sourceDesc->natts; attrIndex++)
		{
			FormData_pg_attribute *sourceAttr = TupleDescAttr(sourceDesc, attrIndex);

			if (!sourceAttr->attisdropped)
			{
				targetNulls[attrIndex] = sourceNulls[attrIndex];
				targetValues[attrIndex] = sourceValues[attrIndex];
			}
		}

		CStoreWriteRow(writeState, targetValues, targetNulls);
		(*num_tuples)++;
	}

	*tups_vacuumed = *num_tuples;

	CStoreEndWrite(writeState);
	CStoreEndRead(readState);
}


static bool
cstore_scan_analyze_next_block(TableScanDesc scan, BlockNumber blockno,
							   BufferAccessStrategy bstrategy)
{
	/*
	 * Our access method is not pages based, i.e. tuples are not confined
	 * to pages boundaries. So not much to do here. We return true anyway
	 * so acquire_sample_rows() in analyze.c would call our
	 * cstore_scan_analyze_next_tuple() callback.
	 */
	return true;
}


static bool
cstore_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
							   double *liverows, double *deadrows,
							   TupleTableSlot *slot)
{
	/*
	 * Currently we don't do anything smart to reduce number of rows returned
	 * for ANALYZE. The TableAM API's ANALYZE functions are designed for page
	 * based access methods where it chooses random pages, and then reads
	 * tuples from those pages.
	 *
	 * We could do something like that here by choosing sample stripes or blocks,
	 * but getting that correct might need quite some work. Since cstore_fdw's
	 * ANALYZE scanned all rows, as a starter we do the same here and scan all
	 * rows.
	 */
	if (cstore_getnextslot(scan, ForwardScanDirection, slot))
	{
		(*liverows)++;
		return true;
	}

	return false;
}


static double
cstore_index_build_range_scan(Relation heapRelation,
							  Relation indexRelation,
							  IndexInfo *indexInfo,
							  bool allow_sync,
							  bool anyvisible,
							  bool progress,
							  BlockNumber start_blockno,
							  BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state,
							  TableScanDesc scan)
{
	elog(ERROR, "cstore_index_build_range_scan not implemented");
}


static void
cstore_index_validate_scan(Relation heapRelation,
						   Relation indexRelation,
						   IndexInfo *indexInfo,
						   Snapshot snapshot,
						   ValidateIndexState *state)
{
	elog(ERROR, "cstore_index_validate_scan not implemented");
}


static uint64
cstore_relation_size(Relation rel, ForkNumber forkNumber)
{
	uint64 nblocks = 0;

	/* Open it at the smgr level if not already done */
	RelationOpenSmgr(rel);

	/* InvalidForkNumber indicates returning the size for all forks */
	if (forkNumber == InvalidForkNumber)
	{
		for (int i = 0; i < MAX_FORKNUM; i++)
		{
			nblocks += smgrnblocks(rel->rd_smgr, i);
		}
	}
	else
	{
		nblocks = smgrnblocks(rel->rd_smgr, forkNumber);
	}

	return nblocks * BLCKSZ;
}


static bool
cstore_relation_needs_toast_table(Relation rel)
{
	return false;
}


static void
cstore_estimate_rel_size(Relation rel, int32 *attr_widths,
						 BlockNumber *pages, double *tuples,
						 double *allvisfrac)
{
	RelationOpenSmgr(rel);
	*pages = smgrnblocks(rel->rd_smgr, MAIN_FORKNUM);
	*tuples = CStoreTableRowCount(rel);

	/*
	 * Append-only, so everything is visible except in-progress or rolled-back
	 * transactions.
	 */
	*allvisfrac = 1.0;

	get_rel_data_width(rel, attr_widths);
}


static bool
cstore_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	elog(ERROR, "cstore_scan_sample_next_block not implemented");
}


static bool
cstore_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	elog(ERROR, "cstore_scan_sample_next_tuple not implemented");
}


static void
CStoreExecutorEnd(QueryDesc *queryDesc)
{
	cstore_free_write_state();
	if (PreviousExecutorEndHook)
	{
		PreviousExecutorEndHook(queryDesc);
	}
	else
	{
		standard_ExecutorEnd(queryDesc);
	}
	ResetCStoreMemoryContext();
}


void
cstore_tableam_init()
{
	PreviousExecutorEndHook = ExecutorEnd_hook;
	ExecutorEnd_hook = CStoreExecutorEnd;
	prevObjectAccessHook = object_access_hook;
	object_access_hook = CStoreTableAMObjectAccessHook;
}


void
cstore_tableam_finish()
{
	ExecutorEnd_hook = PreviousExecutorEndHook;
}


/*
 * Implements object_access_hook. One of the places this is called is just
 * before dropping an object, which allows us to clean-up resources for
 * cstore tables.
 *
 * See the comments for CStoreFdwObjectAccessHook for more details.
 */
static void
CStoreTableAMObjectAccessHook(ObjectAccessType access, Oid classId, Oid objectId, int
							  subId,
							  void *arg)
{
	if (prevObjectAccessHook)
	{
		prevObjectAccessHook(access, classId, objectId, subId, arg);
	}

	/*
	 * Do nothing if this is not a DROP relation command.
	 */
	if (access != OAT_DROP || classId != RelationRelationId || OidIsValid(subId))
	{
		return;
	}

	/*
	 * Lock relation to prevent it from being dropped and to avoid
	 * race conditions in the next if block.
	 */
	LockRelationOid(objectId, AccessShareLock);

	if (IsCStoreTableAmTable(objectId))
	{
		/*
		 * Drop metadata. No need to drop storage here since for
		 * tableam tables storage is managed by postgres.
		 */
		Relation rel = table_open(objectId, AccessExclusiveLock);
		DeleteDataFileMetadataRowIfExists(rel->rd_node.relNode);

		/* keep the lock since we did physical changes to the relation */
		table_close(rel, NoLock);
	}
}


/*
 * IsCStoreTableAmTable returns true if relation has cstore_tableam
 * access method. This can be called before extension creation.
 */
static bool
IsCStoreTableAmTable(Oid relationId)
{
	bool result;
	Relation rel;

	if (!OidIsValid(relationId))
	{
		return false;
	}

	/*
	 * Lock relation to prevent it from being dropped &
	 * avoid race conditions.
	 */
	rel = relation_open(relationId, AccessShareLock);
	result = rel->rd_tableam == GetCstoreTableAmRoutine();
	relation_close(rel, NoLock);

	return result;
}


static const TableAmRoutine cstore_am_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = cstore_slot_callbacks,

	.scan_begin = cstore_beginscan,
	.scan_end = cstore_endscan,
	.scan_rescan = cstore_rescan,
	.scan_getnextslot = cstore_getnextslot,

	.parallelscan_estimate = cstore_parallelscan_estimate,
	.parallelscan_initialize = cstore_parallelscan_initialize,
	.parallelscan_reinitialize = cstore_parallelscan_reinitialize,

	.index_fetch_begin = cstore_index_fetch_begin,
	.index_fetch_reset = cstore_index_fetch_reset,
	.index_fetch_end = cstore_index_fetch_end,
	.index_fetch_tuple = cstore_index_fetch_tuple,

	.tuple_fetch_row_version = cstore_fetch_row_version,
	.tuple_get_latest_tid = cstore_get_latest_tid,
	.tuple_tid_valid = cstore_tuple_tid_valid,
	.tuple_satisfies_snapshot = cstore_tuple_satisfies_snapshot,
	.compute_xid_horizon_for_tuples = cstore_compute_xid_horizon_for_tuples,

	.tuple_insert = cstore_tuple_insert,
	.tuple_insert_speculative = cstore_tuple_insert_speculative,
	.tuple_complete_speculative = cstore_tuple_complete_speculative,
	.multi_insert = cstore_multi_insert,
	.tuple_delete = cstore_tuple_delete,
	.tuple_update = cstore_tuple_update,
	.tuple_lock = cstore_tuple_lock,
	.finish_bulk_insert = cstore_finish_bulk_insert,

	.relation_set_new_filenode = cstore_relation_set_new_filenode,
	.relation_nontransactional_truncate = cstore_relation_nontransactional_truncate,
	.relation_copy_data = cstore_relation_copy_data,
	.relation_copy_for_cluster = cstore_relation_copy_for_cluster,
	.relation_vacuum = heap_vacuum_rel,
	.scan_analyze_next_block = cstore_scan_analyze_next_block,
	.scan_analyze_next_tuple = cstore_scan_analyze_next_tuple,
	.index_build_range_scan = cstore_index_build_range_scan,
	.index_validate_scan = cstore_index_validate_scan,

	.relation_size = cstore_relation_size,
	.relation_needs_toast_table = cstore_relation_needs_toast_table,

	.relation_estimate_size = cstore_estimate_rel_size,

	.scan_bitmap_next_block = NULL,
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = cstore_scan_sample_next_block,
	.scan_sample_next_tuple = cstore_scan_sample_next_tuple
};


const TableAmRoutine *
GetCstoreTableAmRoutine(void)
{
	return &cstore_am_methods;
}


PG_FUNCTION_INFO_V1(cstore_tableam_handler);
Datum
cstore_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&cstore_am_methods);
}
