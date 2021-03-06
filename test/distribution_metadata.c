/*-------------------------------------------------------------------------
 *
 * test/distribution_metadata.c
 *
 * This file contains functions to exercise distributed table metadata
 * functionality within pg_shard.
 *
 * Copyright (c) 2014-2015, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "fmgr.h"
#include "postgres_ext.h"

#include "distribution_metadata.h"
#include "test/test_helper_functions.h" /* IWYU pragma: keep */

#include <stddef.h>
#include <stdint.h>

#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/palloc.h"


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(load_shard_id_array);
PG_FUNCTION_INFO_V1(load_shard_interval_array);
PG_FUNCTION_INFO_V1(load_shard_placement_array);
PG_FUNCTION_INFO_V1(partition_column_id);
PG_FUNCTION_INFO_V1(insert_hash_partition_row);
PG_FUNCTION_INFO_V1(insert_monolithic_shard_row);
PG_FUNCTION_INFO_V1(insert_healthy_local_shard_placement_row);
PG_FUNCTION_INFO_V1(delete_shard_placement_row);
PG_FUNCTION_INFO_V1(next_shard_id);
PG_FUNCTION_INFO_V1(acquire_shared_shard_lock);


/*
 * load_shard_id_array returns the shard identifiers for a particular
 * distributed table as a bigint array. Uses pg_shard's shard interval
 * cache if the second parameter is true, otherwise eagerly loads the
 * shard intervals from the backing table.
 */
Datum
load_shard_id_array(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	bool useCache = PG_GETARG_BOOL(1);
	ArrayType *shardIdArrayType = NULL;
	ListCell *shardCell = NULL;
	int shardIdIndex = 0;
	Oid shardIdTypeId = INT8OID;

	List *shardList = NIL;
	int shardIdCount = -1;
	Datum *shardIdDatumArray = NULL;

	if (useCache)
	{
		shardList = LookupShardIntervalList(distributedTableId);
	}
	else
	{
		shardList = LoadShardIntervalList(distributedTableId);
	}

	shardIdCount = list_length(shardList);
	shardIdDatumArray = palloc0(shardIdCount * sizeof(Datum));

	foreach(shardCell, shardList)
	{
		ShardInterval *shardId = (ShardInterval *) lfirst(shardCell);
		Datum shardIdDatum = Int64GetDatum(shardId->id);

		shardIdDatumArray[shardIdIndex] = shardIdDatum;
		shardIdIndex++;
	}

	shardIdArrayType = DatumArrayToArrayType(shardIdDatumArray, shardIdCount,
											 shardIdTypeId);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


/*
 * load_shard_interval_array loads a shard interval using a provided identifier
 * and returns a two-element array consisting of min/max values contained in
 * that shard interval (currently always integer values). If no such interval
 * can be found, this function raises an error instead.
 */
Datum
load_shard_interval_array(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	ShardInterval *shardInterval = LoadShardInterval(shardId);
	Datum shardIntervalArray[] = { shardInterval->minValue, shardInterval->maxValue };
	ArrayType *shardIntervalArrayType = NULL;

	/* for now we expect value type to always be integer (hash output) */
	Assert(shardInterval->valueTypeId == INT4OID);

	shardIntervalArrayType = DatumArrayToArrayType(shardIntervalArray, 2,
												   shardInterval->valueTypeId);

	PG_RETURN_ARRAYTYPE_P(shardIntervalArrayType);
}


/*
 * load_shard_placement_array loads a shard interval using the provided ID
 * and returns an array of strings containing the node name and port for each
 * placement of the specified shard interval. If the second argument is true,
 * only finalized placements are returned; otherwise, all are. If no such shard
 * interval can be found, this function raises an error instead.
 */
Datum
load_shard_placement_array(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	bool onlyFinalized = PG_GETARG_BOOL(1);
	ArrayType *placementArrayType = NULL;
	List *placementList = NIL;
	ListCell *placementCell = NULL;
	int placementCount = -1;
	int placementIndex = 0;
	Datum *placementDatumArray = NULL;
	Oid placementTypeId = TEXTOID;
	StringInfo placementInfo = makeStringInfo();

	if (onlyFinalized)
	{
		placementList = LoadFinalizedShardPlacementList(shardId);
	}
	else
	{
		placementList = LoadShardPlacementList(shardId);
	}

	placementCount = list_length(placementList);
	placementDatumArray = palloc0(placementCount * sizeof(Datum));

	foreach(placementCell, placementList)
	{
		ShardPlacement *placement = (ShardPlacement *) lfirst(placementCell);
		appendStringInfo(placementInfo, "%s:%d", placement->nodeName,
						 placement->nodePort);

		placementDatumArray[placementIndex] = CStringGetTextDatum(placementInfo->data);
		placementIndex++;
		resetStringInfo(placementInfo);
	}

	placementArrayType = DatumArrayToArrayType(placementDatumArray, placementCount,
											   placementTypeId);

	PG_RETURN_ARRAYTYPE_P(placementArrayType);
}


/*
 * partition_column_id simply finds a distributed table using the provided Oid
 * and returns the column_id of its partition column. If the specified table is
 * not distributed, this function raises an error instead.
 */
Datum
partition_column_id(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	Var *partitionColumn = PartitionColumn(distributedTableId);

	PG_RETURN_INT16((int16) partitionColumn->varattno);
}


/*
 * insert_hash_partition_row inserts a partition row using the provided Oid and
 * partition key (a text value). This function raises an error on any failure.
 */
Datum
insert_hash_partition_row(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *partitionKeyText = PG_GETARG_TEXT_P(1);

	InsertPartitionRow(distributedTableId, HASH_PARTITION_TYPE, partitionKeyText);

	PG_RETURN_VOID();
}


/*
 * insert_monolithic_shard_row creates a single shard covering all possible
 * hash values for a given table and inserts a row representing that shard
 * into the backing store.
 */
Datum
insert_monolithic_shard_row(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	uint64 shardId = (uint64) PG_GETARG_INT64(1);
	StringInfo minInfo = makeStringInfo();
	StringInfo maxInfo = makeStringInfo();

	appendStringInfo(minInfo, "%d", INT32_MIN);
	appendStringInfo(maxInfo, "%d", INT32_MAX);

	InsertShardRow(distributedTableId, shardId, SHARD_STORAGE_TABLE,
				   cstring_to_text(minInfo->data), cstring_to_text(maxInfo->data));

	PG_RETURN_VOID();
}


/*
 * insert_healthy_local_shard_placement_row inserts a row representing a
 * finalized placement for localhost (on the default port) into the backing
 * store.
 */
Datum
insert_healthy_local_shard_placement_row(PG_FUNCTION_ARGS)
{
	uint64 shardPlacementId = (uint64) PG_GETARG_INT64(0);
	uint64 shardId = (uint64) PG_GETARG_INT64(1);

	InsertShardPlacementRow(shardPlacementId, shardId, STATE_FINALIZED, "localhost",
							5432);

	PG_RETURN_VOID();
}


/*
 * delete_shard_placement_row removes a shard placement with the specified ID.
 */
Datum
delete_shard_placement_row(PG_FUNCTION_ARGS)
{
	uint64 shardPlacementId = (uint64) PG_GETARG_INT64(0);

	DeleteShardPlacementRow(shardPlacementId);

	PG_RETURN_VOID();
}


/*
 * next_shard_id returns the next value from the shard ID sequence.
 */
Datum
next_shard_id(PG_FUNCTION_ARGS __attribute__((unused)))
{
	int64 shardId = (int64) NextSequenceId(SHARD_ID_SEQUENCE_NAME);

	PG_RETURN_INT64(shardId);
}


/*
 * acquire_shared_shard_lock grabs a shared lock for the specified shard.
 */
Datum
acquire_shared_shard_lock(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);

	LockShard(shardId, ShareLock);

	PG_RETURN_VOID();
}
