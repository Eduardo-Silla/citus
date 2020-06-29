/*-------------------------------------------------------------------------
 *
 * shard_utils.c
 *
 * This file contains functions to perform useful operations on shards.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "utils/lsyscache.h"
#include "distributed/metadata_cache.h"
#include "distributed/namespace_utils.h"
#include "distributed/relay_utility.h"
#include "distributed/shard_utils.h"

/*
 * GetTableLocalShardOid returns the oid of the shard from the given distributed
 * relation with the shardId.
 */
Oid
GetTableLocalShardOid(Oid citusTableOid, uint64 shardId)
{
	const char *citusTableName = get_rel_name(citusTableOid);

	Assert(citusTableName != NULL);

	/* construct shard relation name */
	char *shardRelationName = pstrdup(citusTableName);
	AppendShardIdToName(&shardRelationName, shardId);

	Oid citusTableSchemaOid = get_rel_namespace(citusTableOid);

	Oid shardRelationOid = get_relname_relid(shardRelationName, citusTableSchemaOid);

	return shardRelationOid;
}


/*
 * GetNoneDistTableLocalShardRelationId returns relation id for local shard of
 * DISTRIBUTE_BY_NONE table with noneDistTableId. Caller of this function must
 * ensure that and input relation has a local shard placement in current node.
 */
Oid
GetNoneDistTableLocalShardRelationId(Oid noneDistTableId)
{
	uint64 shardId = GetNoneDistTableShardId(noneDistTableId);
	return GetTableLocalShardOid(noneDistTableId, shardId);
}


/*
 * GetNoneDistTableShardId takes noneDistTableId that identifies a DISTRIBUTE_BY_NONE
 * table and returns the shard id for its one and only shard.
 */
uint64
GetNoneDistTableShardId(Oid noneDistTableId)
{
	const CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(noneDistTableId);

	/* given OID should belong to a valid DISTRIBUTE_BY_NONE table */
	Assert(cacheEntry != NULL && cacheEntry->partitionMethod == DISTRIBUTE_BY_NONE);

	const ShardInterval *shardInterval = cacheEntry->sortedShardIntervalArray[0];
	uint64 shardId = shardInterval->shardId;

	return shardId;
}


/*
 * AppendNoneDistTableShardIdToName appends shard id for DISTRIBUTE_BY_NONE
 * relation with noneDistTableId to "name" string.
 */
void
AppendNoneDistTableShardIdToName(Oid noneDistTableId, char **name)
{
	uint64 shardId = GetNoneDistTableShardId(noneDistTableId);
	AppendShardIdToName(name, shardId);
}


/*
 * MakeRangeVarForNoneDistTableLocalShard returns qualified RangeVar for
 * local shard relation of DISTRIBUTE_BY_NONE table with noneDistTableId.
 */
RangeVar *
MakeRangeVarForNoneDistTableLocalShard(Oid noneDistTableId)
{
	Oid shardRelationId = GetNoneDistTableLocalShardRelationId(noneDistTableId);
	List *qualifiedNameList = MakeQualifiedNameListFromRelationId(shardRelationId);

	return makeRangeVarFromNameList(qualifiedNameList);
}
