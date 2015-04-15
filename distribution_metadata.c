/*-------------------------------------------------------------------------
 *
 * distribution_metadata.c
 *
 * This file contains functions to access and manage the distributed table
 * metadata.
 *
 * Copyright (c) 2014-2015, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pg_config.h"

#include "distribution_metadata.h"

#include <stddef.h>
#include <string.h>

#include "access/attnum.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/htup.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/sysattr.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "nodes/makefuncs.h"
#include "nodes/memnodes.h" /* IWYU pragma: keep */
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/tqual.h"


/*
 * ShardIntervalListCache is used for caching shard interval lists. It begins
 * initialized to empty list as there are no items in the cache.
 */
static List *ShardIntervalListCache = NIL;

//static bool UseCitusMetadata = false;
//static char *MetadataSchemaName = METADATA_SCHEMA_NAME;
//static char *PartitionTableName = PARTITION_TABLE_NAME;
//
//static char *ShardTableName = SHARD_TABLE_NAME;
//static char *ShardPkeyIndexName = SHARD_PKEY_INDEX_NAME;
//static char *ShardRelationIndexName = SHARD_RELATION_INDEX_NAME;
//static int ShardTableAttributeCount = SHARD_TABLE_ATTRIBUTE_COUNT;
//
//static AttrNumber AttrNumShardId = ATTR_NUM_SHARD_ID;
//static AttrNumber AttrNumShardRelationId = ATTR_NUM_SHARD_RELATION_ID;
//static AttrNumber AttrNumShardStorage = ATTR_NUM_SHARD_STORAGE;
//static AttrNumber AttrNumShardMinValue = ATTR_NUM_SHARD_MIN_VALUE;
//static AttrNumber AttrNumShardMaxValue = ATTR_NUM_SHARD_MAX_VALUE;
//
//static char *ShardPlacementTableName = SHARD_PLACEMENT_TABLE_NAME;
//static char *ShardPlacementPkeyIndexName = SHARD_PLACEMENT_PKEY_INDEX_NAME;
//static char *ShardPlacementShardIndexName = SHARD_PLACEMENT_SHARD_INDEX_NAME;
//static int ShardPlacementTableAttributeCount = SHARD_PLACEMENT_TABLE_ATTRIBUTE_COUNT;
//
//static AttrNumber AttrNumShardPlacementId = ATTR_NUM_SHARD_PLACEMENT_ID;
//static AttrNumber AttrNumShardPlacementShardId = ATTR_NUM_SHARD_PLACEMENT_SHARD_ID;
//static AttrNumber AttrNumShardPlacementShardState = ATTR_NUM_SHARD_PLACEMENT_SHARD_STATE;
//static AttrNumber AttrNumShardPlacementNodeName = ATTR_NUM_SHARD_PLACEMENT_NODE_NAME;
//static AttrNumber AttrNumShardPlacementNodePort = ATTR_NUM_SHARD_PLACEMENT_NODE_PORT;

static bool UseCitusMetadata = true;
static char *MetadataSchemaName = "pg_catalog";
static char *PartitionTableName = "pg_dist_partition";

static char *ShardTableName = "pg_dist_shard";
static char *ShardPkeyIndexName = "pg_dist_shard_shardid_index";
static char *ShardRelationIndexName = "pg_dist_shard_logical_relid_index";
static int ShardTableAttributeCount = 6;

static AttrNumber AttrNumShardId = 2;
static AttrNumber AttrNumShardRelationId = 1;
static AttrNumber AttrNumShardStorage = 3;
static AttrNumber AttrNumShardMinValue = 5;
static AttrNumber AttrNumShardMaxValue = 6;

static char *ShardPlacementTableName = "pg_dist_shard_placement";
static char *ShardPlacementPkeyIndexName = "pg_dist_shard_placement_oid_index";
static char *ShardPlacementShardIndexName = "pg_dist_shard_placement_shardid_index";
static int ShardPlacementTableAttributeCount = 5;

static AttrNumber AttrNumShardPlacementId = ObjectIdAttributeNumber;
static AttrNumber AttrNumShardPlacementShardId = 1;
static AttrNumber AttrNumShardPlacementShardState = 2;
static AttrNumber AttrNumShardPlacementNodeName = 4;
static AttrNumber AttrNumShardPlacementNodePort = 5;

/* local function forward declarations */
static void LoadShardIntervalRow(int64 shardId, Oid *relationId,
								 char **minValue, char **maxValue);
static ShardPlacement * TupleToShardPlacement(HeapTuple heapTuple,
											  TupleDesc tupleDescriptor);


/*
 * LookupShardIntervalList is wrapper around LoadShardIntervalList that uses a
 * cache to avoid multiple lookups of a distributed table's shards within a
 * single session.
 */
List *
LookupShardIntervalList(Oid distributedTableId)
{
	ShardIntervalListCacheEntry *matchingCacheEntry = NULL;
	ListCell *cacheEntryCell = NULL;

	/* search the cache */
	foreach(cacheEntryCell, ShardIntervalListCache)
	{
		ShardIntervalListCacheEntry *cacheEntry = lfirst(cacheEntryCell);
		if (cacheEntry->distributedTableId == distributedTableId)
		{
			matchingCacheEntry = cacheEntry;
			break;
		}
	}

	/* if not found in the cache, load the shard interval and put it in cache */
	if (matchingCacheEntry == NULL)
	{
		MemoryContext oldContext = MemoryContextSwitchTo(CacheMemoryContext);

		List *loadedIntervalList = LoadShardIntervalList(distributedTableId);
		if (loadedIntervalList != NIL)
		{
			matchingCacheEntry = palloc0(sizeof(ShardIntervalListCacheEntry));
			matchingCacheEntry->distributedTableId = distributedTableId;
			matchingCacheEntry->shardIntervalList = loadedIntervalList;

			ShardIntervalListCache = lappend(ShardIntervalListCache, matchingCacheEntry);
		}

		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * The only case we don't cache the shard list is when the distributed table
	 * doesn't have any shards. This is to force reloading shard list on next call.
	 */
	if (matchingCacheEntry == NULL)
	{
		return NIL;
	}

	return matchingCacheEntry->shardIntervalList;
}


/*
 * LoadShardIntervalList returns a list of shard intervals related for a given
 * distributed table. The function returns an empty list if no shards can be
 * found for the given relation.
 */
List *
LoadShardIntervalList(Oid distributedTableId)
{
	List *shardIntervalList = NIL;
	RangeVar *heapRangeVar = NULL;
	RangeVar *indexRangeVar = NULL;
	Relation heapRelation = NULL;
	Relation indexRelation = NULL;
	IndexScanDesc indexScanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, ShardTableName, -1);
	indexRangeVar = makeRangeVar(MetadataSchemaName, ShardRelationIndexName, -1);

	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);
	indexRelation = relation_openrv(indexRangeVar, AccessShareLock);

	ScanKeyInit(&scanKey[0], 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(distributedTableId));

	indexScanDesc = index_beginscan(heapRelation, indexRelation, SnapshotSelf,
									scanKeyCount, 0);
	index_rescan(indexScanDesc, scanKey, scanKeyCount, NULL, 0);

	heapTuple = index_getnext(indexScanDesc, ForwardScanDirection);
	while (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(heapRelation);
		bool isNull = false;

		Datum shardIdDatum = heap_getattr(heapTuple, AttrNumShardId,
										  tupleDescriptor, &isNull);

		int64 shardId = DatumGetInt64(shardIdDatum);
		ShardInterval *shardInterval = LoadShardInterval(shardId);

		shardIntervalList = lappend(shardIntervalList, shardInterval);

		heapTuple = index_getnext(indexScanDesc, ForwardScanDirection);
	}

	index_endscan(indexScanDesc);
	index_close(indexRelation, AccessShareLock);
	relation_close(heapRelation, AccessShareLock);

	return shardIntervalList;
}


/*
 * LoadShardInterval collects metadata for a specified shard in a ShardInterval
 * and returns a pointer to that structure. The function throws an error if no
 * shard can be found using the provided identifier.
 */
ShardInterval *
LoadShardInterval(int64 shardId)
{
	ShardInterval *shardInterval = NULL;
	Datum minValue = 0;
	Datum maxValue = 0;
	char partitionType = '\0';
	Oid intervalTypeId = InvalidOid;
	int32 intervalTypeMod = -1;
	Oid inputFunctionId = InvalidOid;
	Oid typeIoParam = InvalidOid;
	Oid relationId = InvalidOid;
	char *minValueString = NULL;
	char *maxValueString = NULL;

	/* first read the related row from the shard table */
	LoadShardIntervalRow(shardId, &relationId, &minValueString, &maxValueString);

	/* then find min/max values' actual types */
	partitionType = PartitionType(relationId);
	if (partitionType == HASH_PARTITION_TYPE)
	{
		intervalTypeId = INT4OID;
	}
	else
	{
		Var *partitionColumn = PartitionColumn(relationId);
		intervalTypeId = partitionColumn->vartype;
		intervalTypeMod = partitionColumn->vartypmod;
	}

	getTypeInputInfo(intervalTypeId, &inputFunctionId, &typeIoParam);

	/* finally convert min/max values to their actual types */
	minValue = OidInputFunctionCall(inputFunctionId, minValueString,
									typeIoParam, intervalTypeMod);
	maxValue = OidInputFunctionCall(inputFunctionId, maxValueString,
									typeIoParam, intervalTypeMod);

	shardInterval = (ShardInterval *) palloc0(sizeof(ShardInterval));
	shardInterval->id = shardId;
	shardInterval->relationId = relationId;
	shardInterval->minValue = minValue;
	shardInterval->maxValue = maxValue;
	shardInterval->valueTypeId = intervalTypeId;

	return shardInterval;
}


/*
 * LoadFinalizedShardPlacementList returns all placements for a given shard that
 * are in the finalized state. Like LoadShardPlacementList, this function throws
 * an error if the specified shard has not been placed.
 */
List *
LoadFinalizedShardPlacementList(uint64 shardId)
{
	List *finalizedPlacementList = NIL;
	List *shardPlacementList = LoadShardPlacementList(shardId);

	ListCell *shardPlacementCell = NULL;
	foreach(shardPlacementCell, shardPlacementList)
	{
		ShardPlacement *shardPlacement = (ShardPlacement *) lfirst(shardPlacementCell);
		if (shardPlacement->shardState == STATE_FINALIZED)
		{
			finalizedPlacementList = lappend(finalizedPlacementList, shardPlacement);
		}
	}

	return finalizedPlacementList;
}


/*
 * LoadShardPlacementList gathers metadata for every placement of a given shard
 * and returns a list of ShardPlacements containing that metadata. The function
 * throws an error if the specified shard has not been placed.
 */
List *
LoadShardPlacementList(int64 shardId)
{
	List *shardPlacementList = NIL;
	RangeVar *heapRangeVar = NULL;
	RangeVar *indexRangeVar = NULL;
	Relation heapRelation = NULL;
	Relation indexRelation = NULL;
	IndexScanDesc indexScanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, ShardPlacementTableName, -1);
	indexRangeVar = makeRangeVar(MetadataSchemaName, ShardPlacementShardIndexName, -1);

	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);
	indexRelation = relation_openrv(indexRangeVar, AccessShareLock);

	ScanKeyInit(&scanKey[0], 1, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(shardId));

	indexScanDesc = index_beginscan(heapRelation, indexRelation, SnapshotSelf,
									scanKeyCount, 0);
	index_rescan(indexScanDesc, scanKey, scanKeyCount, NULL, 0);

	heapTuple = index_getnext(indexScanDesc, ForwardScanDirection);
	while (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(heapRelation);
		ShardPlacement *shardPlacement = TupleToShardPlacement(heapTuple,
															   tupleDescriptor);
		shardPlacementList = lappend(shardPlacementList, shardPlacement);

		heapTuple = index_getnext(indexScanDesc, ForwardScanDirection);
	}

	index_endscan(indexScanDesc);
	index_close(indexRelation, AccessShareLock);
	relation_close(heapRelation, AccessShareLock);

	/* if no shard placements are found, error out */
	if (shardPlacementList == NIL)
	{
		ereport(ERROR, (errcode(ERRCODE_NO_DATA),
						errmsg("no placements exist for shard with ID "
							   INT64_FORMAT, shardId)));
	}

	return shardPlacementList;
}


/*
 * PartitionColumn looks up the column used to partition a given distributed
 * table and returns a reference to a Var representing that column. If no entry
 * can be found using the provided identifer, this function throws an error.
 */
Var *
PartitionColumn(Oid distributedTableId)
{
	Var *partitionColumn = NULL;
	RangeVar *heapRangeVar = NULL;
	Relation heapRelation = NULL;
	HeapScanDesc scanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, PartitionTableName, -1);
	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);

	ScanKeyInit(&scanKey[0], ATTR_NUM_PARTITION_RELATION_ID, InvalidStrategy,
				F_OIDEQ, ObjectIdGetDatum(distributedTableId));

	scanDesc = heap_beginscan(heapRelation, SnapshotSelf, scanKeyCount, scanKey);

	heapTuple = heap_getnext(scanDesc, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(heapRelation);
		bool isNull = false;

		Datum keyDatum = heap_getattr(heapTuple, ATTR_NUM_PARTITION_KEY,
									  tupleDescriptor, &isNull);

		if (UseCitusMetadata)
		{
			char *keyString = TextDatumGetCString(keyDatum);
			Node *partitionNode = stringToNode(keyString);

			Assert(IsA(partitionNode, Var));
			partitionColumn = (Var *) partitionNode;
		}
		else
		{
			 char *partitionColumnName = TextDatumGetCString(keyDatum);
			 partitionColumn = ColumnNameToColumn(distributedTableId,
			                                      partitionColumnName);
		}
	}
	else
	{
		char *relationName = get_rel_name(distributedTableId);

		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("no partition column is defined for relation \"%s\"",
							   relationName)));
	}

	heap_endscan(scanDesc);
	relation_close(heapRelation, AccessShareLock);

	return partitionColumn;
}


/*
 * PartitionType looks up the type used to partition a given distributed
 * table and returns a char representing this type. If no entry can be found
 * using the provided identifer, this function throws an error.
 */
char
PartitionType(Oid distributedTableId)
{
	char partitionType = 0;
	RangeVar *heapRangeVar = NULL;
	Relation heapRelation = NULL;
	HeapScanDesc scanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, PartitionTableName, -1);
	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);

	ScanKeyInit(&scanKey[0], ATTR_NUM_PARTITION_RELATION_ID, InvalidStrategy,
				F_OIDEQ, ObjectIdGetDatum(distributedTableId));

	scanDesc = heap_beginscan(heapRelation, SnapshotSelf, scanKeyCount, scanKey);

	heapTuple = heap_getnext(scanDesc, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(heapRelation);
		bool isNull = false;

		Datum partitionTypeDatum = heap_getattr(heapTuple, ATTR_NUM_PARTITION_TYPE,
												tupleDescriptor, &isNull);
		partitionType = DatumGetChar(partitionTypeDatum);
	}
	else
	{
		char *relationName = get_rel_name(distributedTableId);

		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("no partition column is defined for relation \"%s\"",
							   relationName)));
	}

	heap_endscan(scanDesc);
	relation_close(heapRelation, AccessShareLock);

	return partitionType;
}


/*
 * IsDistributedTable simply returns whether the specified table is distributed.
 */
bool
IsDistributedTable(Oid tableId)
{
	bool isDistributedTable = false;
	RangeVar *heapRangeVar = NULL;
	Relation heapRelation = NULL;
	HeapScanDesc scanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, PartitionTableName, -1);
	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);

	ScanKeyInit(&scanKey[0], ATTR_NUM_PARTITION_RELATION_ID, InvalidStrategy,
				F_OIDEQ, ObjectIdGetDatum(tableId));

	scanDesc = heap_beginscan(heapRelation, SnapshotSelf, scanKeyCount, scanKey);

	heapTuple = heap_getnext(scanDesc, ForwardScanDirection);

	isDistributedTable = HeapTupleIsValid(heapTuple);

	heap_endscan(scanDesc);
	relation_close(heapRelation, AccessShareLock);

	return isDistributedTable;
}


/*
 *  DistributedTablesExist returns true if pg_shard has a record of any
 *  distributed tables; otherwise this function returns false.
 */
bool
DistributedTablesExist(void)
{
	bool distributedTablesExist = false;
	RangeVar *heapRangeVar = NULL;
	Relation heapRelation = NULL;
	HeapScanDesc scanDesc = NULL;
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, PartitionTableName, -1);
	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);

	scanDesc = heap_beginscan(heapRelation, SnapshotSelf, 0, NULL);

	heapTuple = heap_getnext(scanDesc, ForwardScanDirection);

	/*
	 * Check whether the partition metadata table contains any tuples. If so,
	 * at least one distributed table exists.
	 */
	distributedTablesExist = HeapTupleIsValid(heapTuple);

	heap_endscan(scanDesc);
	relation_close(heapRelation, AccessShareLock);

	return distributedTablesExist;
}


/*
 * ColumnNameToColumn accepts a relation identifier and column name and returns
 * a Var that represents that column in that relation. This function throws an
 * error if the column doesn't exist or is a system column.
 */
Var *
ColumnNameToColumn(Oid relationId, char *columnName)
{
	Var *partitionColumn = NULL;
	Oid columnTypeOid = InvalidOid;
	int32 columnTypeMod = -1;
	Oid columnCollationOid = InvalidOid;

	/* dummy indexes needed by makeVar */
	const Index tableId = 1;
	const Index columnLevelsUp = 0;

	AttrNumber columnId = get_attnum(relationId, columnName);
	if (columnId == InvalidAttrNumber)
	{
		char *relationName = get_rel_name(relationId);

		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("column \"%s\" of relation \"%s\" does not exist",
							   columnName, relationName)));
	}
	else if (!AttrNumberIsForUserDefinedAttr(columnId))
	{
		char *relationName = get_rel_name(relationId);

		ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						errmsg("column \"%s\" of relation \"%s\" is a system column",
							   columnName, relationName)));
	}

	get_atttypetypmodcoll(relationId, columnId, &columnTypeOid, &columnTypeMod,
						  &columnCollationOid);
	partitionColumn = makeVar(tableId, columnId, columnTypeOid, columnTypeMod,
							  columnCollationOid, columnLevelsUp);

	return partitionColumn;
}


/*
 * LoadShardIntervalRow finds the row for the specified shard identifier in the
 * shard table and copies values from that row into the provided output params.
 */
static void
LoadShardIntervalRow(int64 shardId, Oid *relationId, char **minValue,
					 char **maxValue)
{
	RangeVar *heapRangeVar = NULL;
	RangeVar *indexRangeVar = NULL;
	Relation heapRelation = NULL;
	Relation indexRelation = NULL;
	IndexScanDesc indexScanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	heapRangeVar = makeRangeVar(MetadataSchemaName, ShardTableName, -1);
	indexRangeVar = makeRangeVar(MetadataSchemaName, ShardPkeyIndexName, -1);

	heapRelation = relation_openrv(heapRangeVar, AccessShareLock);
	indexRelation = relation_openrv(indexRangeVar, AccessShareLock);

	ScanKeyInit(&scanKey[0], 1, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(shardId));

	indexScanDesc = index_beginscan(heapRelation, indexRelation, SnapshotSelf,
									scanKeyCount, 0);
	index_rescan(indexScanDesc, scanKey, scanKeyCount, NULL, 0);

	heapTuple = index_getnext(indexScanDesc, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		TupleDesc tupleDescriptor = RelationGetDescr(heapRelation);
		bool isNull = false;

		/* TODO: What the heck do I do for shard storage in CitusDB? */
		Datum relationIdDatum = heap_getattr(heapTuple, AttrNumShardRelationId,
											 tupleDescriptor, &isNull);
		Datum minValueDatum = heap_getattr(heapTuple, AttrNumShardMinValue,
										   tupleDescriptor, &isNull);
		Datum maxValueDatum = heap_getattr(heapTuple, AttrNumShardMaxValue,
										   tupleDescriptor, &isNull);

		/* convert and deep copy row's values */
		(*relationId) = DatumGetObjectId(relationIdDatum);
		(*minValue) = TextDatumGetCString(minValueDatum);
		(*maxValue) = TextDatumGetCString(maxValueDatum);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("shard with ID " INT64_FORMAT " does not exist",
							   shardId)));
	}

	index_endscan(indexScanDesc);
	index_close(indexRelation, AccessShareLock);
	relation_close(heapRelation, AccessShareLock);
}


/*
 * TupleToShardPlacement populates a ShardPlacement using values from a row of
 * the placements configuration table and returns a pointer to that struct. The
 * input tuple must not contain any NULLs.
 */
static ShardPlacement *
TupleToShardPlacement(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	ShardPlacement *shardPlacement = NULL;
	bool isNull = false;

	Datum idDatum = heap_getattr(heapTuple, AttrNumShardPlacementId,
								 tupleDescriptor, &isNull);
	Datum shardIdDatum = heap_getattr(heapTuple, AttrNumShardPlacementShardId,
									  tupleDescriptor, &isNull);
	Datum shardStateDatum = heap_getattr(heapTuple, AttrNumShardPlacementShardState,
										 tupleDescriptor, &isNull);
	Datum nodeNameDatum = heap_getattr(heapTuple, AttrNumShardPlacementNodeName,
									   tupleDescriptor, &isNull);
	Datum nodePortDatum = heap_getattr(heapTuple, AttrNumShardPlacementNodePort,
									   tupleDescriptor, &isNull);

	/* FIXME: Do we need to preserve shardlength? Probably, huh? */

	shardPlacement = palloc0(sizeof(ShardPlacement));

	if (UseCitusMetadata)
	{
		shardPlacement->id = (int64) DatumGetObjectId(idDatum);
	}
	else
	{
		shardPlacement->id = DatumGetInt64(idDatum);
	}

	shardPlacement->shardId = DatumGetInt64(shardIdDatum);
	shardPlacement->shardState = DatumGetInt32(shardStateDatum);
	shardPlacement->nodeName = TextDatumGetCString(nodeNameDatum);
	shardPlacement->nodePort = DatumGetInt32(nodePortDatum);

	return shardPlacement;
}


/*
 * InsertPartitionRow opens the partition metadata table and inserts a new row
 * with the given values.
 */
void
InsertPartitionRow(Oid distributedTableId, char partitionType, text *partitionKeyText)
{
	Relation partitionRelation = NULL;
	RangeVar *partitionRangeVar = NULL;
	TupleDesc tupleDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	Datum values[PARTITION_TABLE_ATTRIBUTE_COUNT];
	bool isNulls[PARTITION_TABLE_ATTRIBUTE_COUNT];

	/* form new partition tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[ATTR_NUM_PARTITION_RELATION_ID - 1] = ObjectIdGetDatum(distributedTableId);
	values[ATTR_NUM_PARTITION_TYPE - 1] = CharGetDatum(partitionType);

	/* CitusDB stores representation of underlying Var node, not just column name */
	if (UseCitusMetadata)
	{
		char *partitionColumnName = text_to_cstring(partitionKeyText);
		Var *partitionColumn = ColumnNameToColumn(distributedTableId,
		                                          partitionColumnName);
		char *partitionKeyString = nodeToString(partitionColumn);

		values[ATTR_NUM_PARTITION_KEY - 1] = CStringGetTextDatum(partitionKeyString);
	}
	else
	{
		values[ATTR_NUM_PARTITION_KEY - 1] = PointerGetDatum(partitionKeyText);
	}

	/* open the partition relation and insert new tuple */
	partitionRangeVar = makeRangeVar(MetadataSchemaName, PartitionTableName, -1);
	partitionRelation = heap_openrv(partitionRangeVar, RowExclusiveLock);

	tupleDescriptor = RelationGetDescr(partitionRelation);
	heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	simple_heap_insert(partitionRelation, heapTuple);
	CatalogUpdateIndexes(partitionRelation, heapTuple);
	CommandCounterIncrement();

	/* close relation */
	relation_close(partitionRelation, RowExclusiveLock);
}


/*
 * InsertShardRow opens the shard metadata table and inserts a new row with
 * the given values into that table. Note that we allow the user to pass in
 * null min/max values.
 */
void
InsertShardRow(Oid distributedTableId, uint64 shardId, char shardStorage,
			   text *shardMinValue, text *shardMaxValue)
{
	Relation shardRelation = NULL;
	RangeVar *shardRangeVar = NULL;
	TupleDesc tupleDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	Datum values[ShardTableAttributeCount]; /* TODO: tupleDescriptor->nattrs ? */
	bool isNulls[ShardTableAttributeCount];

	/* form new shard tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[AttrNumShardId - 1] = Int64GetDatum(shardId);
	values[AttrNumShardRelationId - 1] = ObjectIdGetDatum(distributedTableId);
	values[AttrNumShardStorage - 1] = CharGetDatum(shardStorage);

	/* check if shard min/max values are null */
	if (shardMinValue != NULL && shardMaxValue != NULL)
	{
		values[AttrNumShardMinValue - 1] = PointerGetDatum(shardMinValue);
		values[AttrNumShardMaxValue - 1] = PointerGetDatum(shardMaxValue);
	}
	else
	{
		isNulls[AttrNumShardMinValue - 1] = true;
		isNulls[AttrNumShardMaxValue - 1] = true;
	}

	/* TODO: set shardalias to NULL for CitusDB */

	/* open shard relation and insert new tuple */
	shardRangeVar = makeRangeVar(MetadataSchemaName, ShardTableName, -1);
	shardRelation = heap_openrv(shardRangeVar, RowExclusiveLock);

	tupleDescriptor = RelationGetDescr(shardRelation);
	heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	simple_heap_insert(shardRelation, heapTuple);
	CatalogUpdateIndexes(shardRelation, heapTuple);
	CommandCounterIncrement();

	/* close relation */
	heap_close(shardRelation, RowExclusiveLock);
}


/*
 * InsertShardPlacementRow opens the shard placement metadata table and inserts
 * a row with the given values into the table.
 *
 * TODO: Need to preserve shardlength; add parameter?
 */
void
InsertShardPlacementRow(uint64 shardPlacementId, uint64 shardId,
						ShardState shardState, char *nodeName, uint32 nodePort)
{
	Relation shardPlacementRelation = NULL;
	RangeVar *shardPlacementRangeVar = NULL;
	TupleDesc tupleDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	Datum values[ShardPlacementTableAttributeCount]; /* TODO: tupleDescriptor->nattrs ? */
	bool isNulls[ShardPlacementTableAttributeCount];

	/* form new shard placement tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	/* TODO: Do not set shard placement id in CitusDB */
	values[AttrNumShardPlacementId - 1] = Int64GetDatum(shardPlacementId);
	values[AttrNumShardPlacementShardId - 1] = Int64GetDatum(shardId);
	values[AttrNumShardPlacementShardState - 1] = UInt32GetDatum(shardState);

	/* TODO: set shard length when using CitusDB */
	values[AttrNumShardPlacementNodeName - 1] = CStringGetTextDatum(nodeName);
	values[AttrNumShardPlacementNodePort - 1] = UInt32GetDatum(nodePort);

	/* open shard placement relation and insert new tuple */
	shardPlacementRangeVar = makeRangeVar(MetadataSchemaName,
										  ShardPlacementTableName, -1);
	shardPlacementRelation = heap_openrv(shardPlacementRangeVar, RowExclusiveLock);

	tupleDescriptor = RelationGetDescr(shardPlacementRelation);
	heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	simple_heap_insert(shardPlacementRelation, heapTuple);
	CatalogUpdateIndexes(shardPlacementRelation, heapTuple);
	CommandCounterIncrement();

	/* close relation */
	heap_close(shardPlacementRelation, RowExclusiveLock);
}


/*
 * DeleteShardPlacementRow removes the row corresponding to the provided shard
 * placement identifier, erroring out if it cannot find such a row.
 */
void
DeleteShardPlacementRow(uint64 shardPlacementId)
{
	RangeVar *heapRangeVar = NULL;
	RangeVar *indexRangeVar = NULL;
	Relation heapRelation = NULL;
	Relation indexRelation = NULL;
	IndexScanDesc indexScanDesc = NULL;
	const int scanKeyCount = 1;
	ScanKeyData scanKey[scanKeyCount];
	HeapTuple heapTuple = NULL;

	/* TODO: Error out if shardPlacementId > highest Oid (CitusDB) */

	heapRangeVar = makeRangeVar(MetadataSchemaName, ShardPlacementTableName, -1);
	indexRangeVar = makeRangeVar(MetadataSchemaName, ShardPlacementPkeyIndexName, -1);

	heapRelation = relation_openrv(heapRangeVar, RowExclusiveLock);
	indexRelation = relation_openrv(indexRangeVar, AccessShareLock);

	/* TODO: Use F_OIDEQ/ObjectIdGetDatum when using CitusDB */
	ScanKeyInit(&scanKey[0], 1, BTEqualStrategyNumber, F_INT8EQ,
				Int64GetDatum(shardPlacementId));

	indexScanDesc = index_beginscan(heapRelation, indexRelation, SnapshotSelf,
									scanKeyCount, 0);
	index_rescan(indexScanDesc, scanKey, scanKeyCount, NULL, 0);

	heapTuple = index_getnext(indexScanDesc, ForwardScanDirection);
	if (HeapTupleIsValid(heapTuple))
	{
		simple_heap_delete(heapRelation, &heapTuple->t_self);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("shard placement with ID " INT64_FORMAT " does not exist",
							   shardPlacementId)));
	}

	index_endscan(indexScanDesc);
	index_close(indexRelation, AccessShareLock);
	relation_close(heapRelation, RowExclusiveLock);
}


/*
 * NextSequenceId allocates and returns a new unique id generated from the given
 * sequence name.
 */
uint64
NextSequenceId(char *sequenceName)
{
	RangeVar *sequenceRangeVar = makeRangeVar(MetadataSchemaName,
											  sequenceName, -1);
	bool failOk = false;
	Oid sequenceRelationId = RangeVarGetRelid(sequenceRangeVar, NoLock, failOk);
	Datum sequenceRelationIdDatum = ObjectIdGetDatum(sequenceRelationId);

	/* generate new and unique id from sequence */
	Datum sequenceIdDatum = DirectFunctionCall1(nextval_oid, sequenceRelationIdDatum);
	uint64 nextSequenceId = (uint64) DatumGetInt64(sequenceIdDatum);

	return nextSequenceId;
}


/*
 * LockShard returns after acquiring a lock for the specified shard, blocking
 * indefinitely if required. Only the ExclusiveLock and ShareLock modes are
 * supported: all others will trigger an error. Locks acquired with this method
 * are automatically released at transaction end.
 */
void
LockShard(int64 shardId, LOCKMODE lockMode)
{
	/* locks use 32-bit identifier fields, so split shardId */
	uint32 keyUpperHalf = (uint32) (shardId >> 32);
	uint32 keyLowerHalf = (uint32) shardId;

	LOCKTAG lockTag;
	memset(&lockTag, 0, sizeof(LOCKTAG));

	SET_LOCKTAG_ADVISORY(lockTag, MyDatabaseId, keyUpperHalf, keyLowerHalf, 0);

	if (lockMode == ExclusiveLock || lockMode == ShareLock)
	{
		bool sessionLock = false;   /* we want a transaction lock */
		bool dontWait = false;      /* block indefinitely until acquired */

		(void) LockAcquire(&lockTag, lockMode, sessionLock, dontWait);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("lockMode must be one of: ExclusiveLock, ShareLock")));
	}
}
