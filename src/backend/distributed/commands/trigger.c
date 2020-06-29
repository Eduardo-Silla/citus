/*-------------------------------------------------------------------------
 * trigger.c
 *
 * This file contains functions to create and process trigger objects on
 * citus tables.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "distributed/pg_version_constants.h"

#include "access/genam.h"
#if PG_VERSION_NUM >= PG_VERSION_12
#include "access/table.h"
#else
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#endif
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/commands.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/create_citus_local_table.h"
#include "distributed/listutils.h"
#include "distributed/metadata_cache.h"
#include "distributed/namespace_utils.h"
#include "distributed/shard_utils.h"
#include "distributed/worker_protocol.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* local function forward declarations */
static void ErrorIfUnsupportedTriggerCommand(Node *parseTree, LOCKMODE lockMode);
static RangeVar * GetDropTriggerStmtRelation(DropStmt *dropTriggerStmt);
static bool IsCreateCitusTruncateTriggerStmt(CreateTrigStmt *createTriggerStmt);
static List * CitusLocalTableTriggerCommandDDLJob(Oid relationId,
												  const char *commandString);


/*
 * GetExplicitTriggerCommandList returns the list of DDL commands to create
 * triggers that are explicitly created for the table with relationId. See
 * comment of GetExplicitTriggerIdList function.
 */
List *
GetExplicitTriggerCommandList(Oid relationId)
{
	List *createTriggerCommandList = NIL;

	PushOverrideEmptySearchPath(CurrentMemoryContext);

	List *triggerIdList = GetExplicitTriggerIdList(relationId);

	Oid triggerId = InvalidOid;
	foreach_oid(triggerId, triggerIdList)
	{
		char *createTriggerCommand = pg_get_triggerdef_command(triggerId);

		createTriggerCommandList = lappend(createTriggerCommandList,
										   createTriggerCommand);
	}

	/* revert back to original search_path */
	PopOverrideSearchPath();

	return createTriggerCommandList;
}


/*
 * GetExplicitTriggerNameList returns a list of trigger names that are explicitly
 * created for the table with relationId. See comment of GetExplicitTriggerIdList
 * function.
 */
List *
GetExplicitTriggerNameList(Oid relationId)
{
	List *triggerNameList = NIL;

	List *triggerIdList = GetExplicitTriggerIdList(relationId);

	Oid triggerId = InvalidOid;
	foreach_oid(triggerId, triggerIdList)
	{
		char *triggerHame = GetTriggerNameById(triggerId);
		triggerNameList = lappend(triggerNameList, triggerHame);
	}

	return triggerNameList;
}


/*
 * GetTriggerNameById returns name of the trigger identified by triggerId if it
 * exists. Otherwise, returns NULL.
 */
char *
GetTriggerNameById(Oid triggerId)
{
	char *triggerName = NULL;

	HeapTuple heapTuple = GetTriggerTupleById(triggerId);
	if (HeapTupleIsValid(heapTuple))
	{
		Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(heapTuple);
		triggerName = pstrdup(NameStr(triggerForm->tgname));
		heap_freetuple(heapTuple);
	}

	return triggerName;
}


/*
 * GetTriggerTupleById returns copy of the heap tuple from pg_trigger for
 * the trigger with triggerId.
 */
HeapTuple
GetTriggerTupleById(Oid triggerId)
{
	Relation pgTrigger = heap_open(TriggerRelationId, AccessShareLock);

	int scanKeyCount = 1;
	ScanKeyData scanKey[1];

#if PG_VERSION_NUM >= PG_VERSION_12
	AttrNumber attrNumber = Anum_pg_trigger_oid;
#else
	AttrNumber attrNumber = ObjectIdAttributeNumber;
#endif

	ScanKeyInit(&scanKey[0], attrNumber, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(triggerId));

	bool useIndex = true;
	SysScanDesc scanDescriptor = systable_beginscan(pgTrigger, TriggerOidIndexId,
													useIndex, NULL, scanKeyCount,
													scanKey);

	HeapTuple targetHeapTuple = NULL;

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		targetHeapTuple = heap_copytuple(heapTuple);
	}

	systable_endscan(scanDescriptor);
	heap_close(pgTrigger, NoLock);

	return targetHeapTuple;
}


/*
 * FilterTriggerIdListByEvent filters the given triggerId list by their firing
 * events and returns a new for filtered trigger ids.
 */
List *
FilterTriggerIdListByEvent(List *triggerIdList, int16 events)
{
	Assert(events & TRIGGER_TYPE_EVENT_MASK);

	List *filteredTriggerIdList = NIL;

	Oid triggerId = InvalidOid;
	foreach_oid(triggerId, triggerIdList)
	{
		HeapTuple heapTuple = GetTriggerTupleById(triggerId);

		Assert(HeapTupleIsValid(heapTuple));
		Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(heapTuple);

		if (triggerForm->tgtype & events)
		{
			filteredTriggerIdList = lappend_oid(filteredTriggerIdList, triggerId);
		}

		heap_freetuple(heapTuple);
	}

	return filteredTriggerIdList;
}


/*
 * GetExplicitTriggerIdList returns a list of OIDs corresponding to the triggers
 * that are explicitly created on the relation with relationId. That means,
 * this function discards internal triggers implicitly created by postgres for
 * foreign key constraint validation and the citus_truncate_trigger.
 */
List *
GetExplicitTriggerIdList(Oid relationId)
{
	List *triggerIdList = NIL;

	Relation pgTrigger = heap_open(TriggerRelationId, AccessShareLock);

	int scanKeyCount = 1;
	ScanKeyData scanKey[1];

	ScanKeyInit(&scanKey[0], Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ, relationId);

	bool useIndex = true;
	SysScanDesc scanDescriptor = systable_beginscan(pgTrigger, TriggerRelidNameIndexId,
													useIndex, NULL, scanKeyCount,
													scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(heapTuple);

		/*
		 * Note that we mark truncate trigger that we create on citus tables as
		 * internal. Hence, below we discard citus_truncate_trigger as well as
		 * the implicit triggers created by postgres for foreign key validation.
		 */
		if (!triggerForm->tgisinternal)
		{
			Oid triggerId = get_relation_trigger_oid_compat(heapTuple);
			triggerIdList = lappend_oid(triggerIdList, triggerId);
		}

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	heap_close(pgTrigger, NoLock);

	return triggerIdList;
}


/*
 * get_relation_trigger_oid_compat returns OID of the trigger represented
 * by the constraintForm, which is passed as an heapTuple. OID of the
 * trigger is already stored in the triggerForm struct if major PostgreSQL
 * version is 12. However, in the older versions, we should utilize
 * HeapTupleGetOid to deduce that OID with no cost.
 */
Oid
get_relation_trigger_oid_compat(HeapTuple heapTuple)
{
	Assert(HeapTupleIsValid(heapTuple));

	Oid triggerOid = InvalidOid;

#if PG_VERSION_NUM >= PG_VERSION_12
	Form_pg_trigger triggerForm = (Form_pg_trigger) GETSTRUCT(heapTuple);
	triggerOid = triggerForm->oid;
#else
	triggerOid = HeapTupleGetOid(heapTuple);
#endif

	return triggerOid;
}


/*
 * ErrorIfUnsupportedTriggerCommand errors out for unsupported CREATE/ALTER/DROP
 * trigger commands.
 */
static void
ErrorIfUnsupportedTriggerCommand(Node *parseTree, LOCKMODE lockMode)
{
	bool checkTableType = false;
	RangeVar *relation = NULL;

	if (IsA(parseTree, CreateTrigStmt))
	{
		/* CREATE TRIGGER triggerName ON relationName */

		CreateTrigStmt *createTriggerStmt = castNode(CreateTrigStmt, parseTree);
		if (!IsCreateCitusTruncateTriggerStmt(createTriggerStmt))
		{
			checkTableType = true;
			relation = createTriggerStmt->relation;
		}
	}
	else if (IsA(parseTree, RenameStmt))
	{
		/* ALTER TRIGGER oldName ON relationName RENAME TO newName */

		RenameStmt *renameTriggerStmt = castNode(RenameStmt, parseTree);
		if (renameTriggerStmt->renameType == OBJECT_TRIGGER)
		{
			checkTableType = true;
			relation = renameTriggerStmt->relation;
		}
	}
	else if (IsA(parseTree, AlterObjectDependsStmt))
	{
		/* ALTER TRIGGER triggerName ON relationName DEPENDS ON extensionName */

		AlterObjectDependsStmt *alterTriggerDependsStmt =
			castNode(AlterObjectDependsStmt, parseTree);
		if (alterTriggerDependsStmt->objectType == OBJECT_TRIGGER)
		{
			checkTableType = true;
			relation = alterTriggerDependsStmt->relation;
		}
	}
	else if (IsA(parseTree, DropStmt))
	{
		/* DROP TRIGGER triggerName ON relationName */

		DropStmt *dropTriggerStmt = castNode(DropStmt, parseTree);
		if (dropTriggerStmt->removeType == OBJECT_TRIGGER)
		{
			checkTableType = true;
			relation = GetDropTriggerStmtRelation(dropTriggerStmt);
		}
	}

	if (!checkTableType)
	{
		return;
	}

	bool missingOk = false;
	Oid relationId = RangeVarGetRelid(relation, lockMode, missingOk);

	if (!IsCitusTable(relationId))
	{
		return;
	}

	if (!IsCitusLocalTable(relationId))
	{
		const char *relationName = relation->relname;
		ereport(ERROR, (errmsg("cannot execute CREATE/ALTER/DROP trigger command for "
							   "relation \"%s\" because it is either a distributed "
							   "table or a reference table", relationName)));
	}

	EnsureCoordinator();
}


/*
 * GetDropTriggerStmtRelation takes a DropStmt for a trigger
 * object and returns RangeVar for the relation that owns the trigger.
 */
static RangeVar *
GetDropTriggerStmtRelation(DropStmt *dropTriggerStmt)
{
	Assert(dropTriggerStmt->removeType == OBJECT_TRIGGER);

	List *targetObjectList = dropTriggerStmt->objects;

	/* drop trigger commands can only drop one at a time */
	Assert(list_length(targetObjectList) == 1);
	List *triggerObjectNameList = linitial(targetObjectList);

	List *relationNameList = NIL;
	SplitLastPointerElement(triggerObjectNameList, &relationNameList, NULL);

	return makeRangeVarFromNameList(relationNameList);
}


/*
 * PostprocessCreateTriggerStmt is called after a CREATE TRIGGER command has
 * been executed by standard process utility. This function errors out for
 * unsupported commands or creates ddl job for supported CREATE TRIGGER commands.
 */
List *
PostprocessCreateTriggerStmt(Node *node, const char *commandString)
{
	CreateTrigStmt *createTriggerStmt = castNode(CreateTrigStmt, node);

	RangeVar *relation = createTriggerStmt->relation;

	bool missingOk = false;
	LOCKMODE lockMode = ShareRowExclusiveLock;
	Oid relationId = RangeVarGetRelid(relation, lockMode, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedTriggerCommand((Node *) createTriggerStmt, lockMode);

	if (IsCreateCitusTruncateTriggerStmt(createTriggerStmt))
	{
		return NIL;
	}

	if (IsCitusLocalTable(relationId))
	{
		return CitusLocalTableTriggerCommandDDLJob(relationId, commandString);
	}

	return NIL;
}


/*
 * IsCreateCitusTruncateTriggerStmt returns true if given createTriggerStmt
 * creates citus_truncate_trigger.
 */
static bool
IsCreateCitusTruncateTriggerStmt(CreateTrigStmt *createTriggerStmt)
{
	List *functionNameList = createTriggerStmt->funcname;
	RangeVar *functionRangeVar = makeRangeVarFromNameList(functionNameList);
	char *functionName = functionRangeVar->relname;
	if (strncmp(functionName, CITUS_TRUNCATE_TRIGGER_NAME, NAMEDATALEN) == 0)
	{
		return true;
	}

	return false;
}


/*
 * CreateTriggerEventExtendNames extends relation name and trigger name name
 * with shardId, and sets schema name in given CreateTrigStmt.
 */
void
CreateTriggerEventExtendNames(CreateTrigStmt *createTriggerStmt, char *schemaName,
							  uint64 shardId)
{
	RangeVar *relation = createTriggerStmt->relation;

	char **relationName = &(relation->relname);
	AppendShardIdToName(relationName, shardId);

	char **triggerName = &(createTriggerStmt->trigname);
	AppendShardIdToName(triggerName, shardId);

	char **relationSchemaName = &(relation->schemaname);
	SetSchemaNameIfNotExist(relationSchemaName, schemaName);
}


/*
 * PostprocessAlterTriggerRenameStmt is called after a ALTER TRIGGER RENAME
 * command has been executed by standard process utility. This function errors
 * out for unsupported commands or creates ddl job for supported ALTER TRIGGER
 * RENAME commands.
 */
List *
PostprocessAlterTriggerRenameStmt(Node *node, const char *commandString)
{
	RenameStmt *renameTriggerStmt = castNode(RenameStmt, node);
	Assert(renameTriggerStmt->renameType == OBJECT_TRIGGER);

	RangeVar *relation = renameTriggerStmt->relation;

	bool missingOk = false;
	LOCKMODE lockMode = AccessExclusiveLock;
	Oid relationId = RangeVarGetRelid(relation, lockMode, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedTriggerCommand((Node *) renameTriggerStmt, lockMode);

	if (IsCitusLocalTable(relationId))
	{
		return CitusLocalTableTriggerCommandDDLJob(relationId, commandString);
	}

	return NIL;
}


/*
 * AlterTriggerRenameEventExtendNames extends relation name, old and new trigger
 * name with shardId, and sets schema name in given RenameStmt.
 */
void
AlterTriggerRenameEventExtendNames(RenameStmt *renameTriggerStmt, char *schemaName,
								   uint64 shardId)
{
	Assert(renameTriggerStmt->renameType == OBJECT_TRIGGER);

	RangeVar *relation = renameTriggerStmt->relation;

	char **relationName = &(relation->relname);
	AppendShardIdToName(relationName, shardId);

	char **triggerOldName = &(renameTriggerStmt->subname);
	AppendShardIdToName(triggerOldName, shardId);

	char **triggerNewName = &(renameTriggerStmt->newname);
	AppendShardIdToName(triggerNewName, shardId);

	char **relationSchemaName = &(relation->schemaname);
	SetSchemaNameIfNotExist(relationSchemaName, schemaName);
}


/*
 * PostprocessAlterTriggerDependsStmt is called after a ALTER TRIGGER DEPENDS ON
 * command has been executed by standard process utility. This function errors out
 * for unsupported commands or creates ddl job for supported ALTER TRIGGER DEPENDS
 * ON commands.
 */
List *
PostprocessAlterTriggerDependsStmt(Node *node, const char *commandString)
{
	AlterObjectDependsStmt *alterTriggerDependsStmt =
		castNode(AlterObjectDependsStmt, node);
	Assert(alterTriggerDependsStmt->objectType == OBJECT_TRIGGER);

	RangeVar *relation = alterTriggerDependsStmt->relation;

	bool missingOk = false;
	LOCKMODE lockMode = AccessExclusiveLock;
	Oid relationId = RangeVarGetRelid(relation, lockMode, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedTriggerCommand((Node *) alterTriggerDependsStmt, lockMode);

	if (IsCitusLocalTable(relationId))
	{
		return CitusLocalTableTriggerCommandDDLJob(relationId, commandString);
	}

	return NIL;
}


/*
 * AlterTriggerDependsEventExtendNames extends relation name and trigger name
 * with shardId, and sets schema name in given AlterObjectDependsStmt.
 */
void
AlterTriggerDependsEventExtendNames(AlterObjectDependsStmt *alterTriggerDependsStmt,
									char *schemaName, uint64 shardId)
{
	Assert(alterTriggerDependsStmt->objectType == OBJECT_TRIGGER);

	RangeVar *relation = alterTriggerDependsStmt->relation;

	char **relationName = &(relation->relname);
	AppendShardIdToName(relationName, shardId);

	List *triggerObjectNameList = (List *) alterTriggerDependsStmt->object;
	Value *triggerNameValue = llast(triggerObjectNameList);
	AppendShardIdToName(&strVal(triggerNameValue), shardId);

	char **relationSchemaName = &(relation->schemaname);
	SetSchemaNameIfNotExist(relationSchemaName, schemaName);
}


/*
 * PostprocessDropTriggerStmt is called after a DROP TRIGGER command has been
 * executed by standard process utility. This function errors out for
 * unsupported commands or creates ddl job for supported DROP TRIGGER commands.
 */
List *
PostprocessDropTriggerStmt(Node *node, const char *commandString)
{
	DropStmt *dropTriggerStmt = castNode(DropStmt, node);
	Assert(dropTriggerStmt->removeType == OBJECT_TRIGGER);

	RangeVar *relation = GetDropTriggerStmtRelation(dropTriggerStmt);

	bool missingOk = false;
	LOCKMODE lockMode = AccessExclusiveLock;
	Oid relationId = RangeVarGetRelid(relation, lockMode, missingOk);

	if (!IsCitusTable(relationId))
	{
		return NIL;
	}

	ErrorIfUnsupportedTriggerCommand((Node *) dropTriggerStmt, lockMode);

	if (IsCitusLocalTable(relationId))
	{
		return CitusLocalTableTriggerCommandDDLJob(relationId, commandString);
	}

	return NIL;
}


/*
 * DropTriggerEventExtendNames extends relation name and trigger name with
 * shardId, and sets schema name in given DropStmt by recreating "objects"
 * list.
 */
void
DropTriggerEventExtendNames(DropStmt *dropTriggerStmt, char *schemaName, uint64 shardId)
{
	Assert(dropTriggerStmt->removeType == OBJECT_TRIGGER);

	List *targetObjectList = dropTriggerStmt->objects;

	/* drop trigger commands can only drop one at a time */
	Assert(list_length(targetObjectList) == 1);
	List *triggerObjectNameList = linitial(targetObjectList);

	List *relationNameList = NIL;
	Value *triggerNameValue = NULL;
	SplitLastPointerElement(triggerObjectNameList, &relationNameList,
							(void **) &triggerNameValue);
	AppendShardIdToName(&strVal(triggerNameValue), shardId);

	Value *relationNameValue = llast(relationNameList);
	AppendShardIdToName(&strVal(relationNameValue), shardId);

	Value *schemaNameValue = makeString(pstrdup(schemaName));

	List *shardTriggerNameList =
		list_make3(schemaNameValue, relationNameValue, triggerNameValue);
	dropTriggerStmt->objects = list_make1(shardTriggerNameList);
}


/*
 * CitusLocalTableTriggerCommandDDLJob creates a ddl job to execute given
 * commandString on shell relation(s) in mx worker(s) and to execute necessary
 * ddl task to on citus local table shard.
 */
static List *
CitusLocalTableTriggerCommandDDLJob(Oid relationId, const char *commandString)
{
	DDLJob *ddlJob = palloc0(sizeof(DDLJob));
	ddlJob->targetRelationId = relationId;
	ddlJob->commandString = commandString;
	ddlJob->taskList = DDLTaskList(relationId, commandString);

	return list_make1(ddlJob);
}
