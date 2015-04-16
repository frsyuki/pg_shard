/*-------------------------------------------------------------------------
 *
 * citus_metadata_sync.c
 *
 * This file contains functions to sync pg_shard metadata to the CitusDB
 * metadata tables.
 *
 * Copyright (c) 2014-2015, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "postgres_ext.h"

#include "citus_metadata_sync.h"
#include "distribution_metadata.h"

#include <stddef.h>

#include "access/attnum.h"
#include "nodes/nodes.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(partition_column_to_node_string);
PG_FUNCTION_INFO_V1(column_name_to_column);
PG_FUNCTION_INFO_V1(column_to_column_name);


/*
 * partition_column_to_node_string is an internal UDF to obtain the textual
 * representation of a partition column node (Var), suitable for use within
 * CitusDB's metadata tables. This function expects an Oid identifying a table
 * previously distributed using pg_shard and will raise an ERROR if the Oid
 * is NULL, or does not identify a pg_shard-distributed table.
 */
Datum
partition_column_to_node_string(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = InvalidOid;
	Var *partitionColumn = NULL;
	char *partitionColumnString = NULL;
	text *partitionColumnText = NULL;

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						errmsg("table_oid must not be null")));
	}

	distributedTableId = PG_GETARG_OID(0);
	partitionColumn = PartitionColumn(distributedTableId);
	partitionColumnString = nodeToString(partitionColumn);
	partitionColumnText = cstring_to_text(partitionColumnString);

	PG_RETURN_TEXT_P(partitionColumnText);
}


/*
 * column_name_to_column is an internal UDF to obtain a textual representation
 * of a particular column node (Var), given a relation identifier and column
 * name. There is no requirement that the table be distributed; this function
 * simply returns the textual representation of a Var representing a column.
 * This function will raise an ERROR if no such column can be found or if the
 * provided name refers to a system column.
 */
Datum
column_name_to_column(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);
	text *columnText = PG_GETARG_TEXT_P(1);
	char *columnName = text_to_cstring(columnText);
	Var *column = NULL;
	char *columnNodeString = NULL;
	text *columnNodeText = NULL;

	column = ColumnNameToColumn(relationId, columnName);
	columnNodeString = nodeToString(column);
	columnNodeText = cstring_to_text(columnNodeString);

	PG_RETURN_TEXT_P(columnNodeText);
}


/*
 * column_to_column_name is an internal UDF to obtain the human-readable name
 * of a column given a relation identifier and the column's internal textual
 * (Var) representation. This function will raise an ERROR if no such column
 * can be found or if the provided Var refers to a system column.
 */
Datum
column_to_column_name(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);
	text *columnNodeText = PG_GETARG_TEXT_P(1);
	char *columnNodeString = text_to_cstring(columnNodeText);
	Node *columnNode = NULL;
	Var *column = NULL;
	AttrNumber columnNumber = InvalidAttrNumber;
	char *columnName = NULL;
	text *columnText = NULL;

	columnNode = stringToNode(columnNodeString);

	Assert(IsA(columnNode, Var));
	column = (Var *) columnNode;

	columnNumber = column->varattno;
	if (!AttrNumberIsForUserDefinedAttr(columnNumber))
	{
		char *relationName = get_rel_name(relationId);

		ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						errmsg("attribute %d of relation \"%s\" is a system column",
							   columnNumber, relationName)));
	}

	columnName = get_attname(relationId, column->varattno);
	if (columnName == NULL)
	{
		char *relationName = get_rel_name(relationId);

		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("attribute %d of relation \"%s\" does not exist",
							   columnNumber, relationName)));
	}

	columnText = cstring_to_text(columnName);

	PG_RETURN_TEXT_P(columnText);
}
