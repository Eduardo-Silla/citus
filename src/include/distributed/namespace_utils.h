/*-------------------------------------------------------------------------
 *
 * namespace_utils.h
 *	  Utility function declarations related to namespace changes.
 *
 * Copyright (c) Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#ifndef NAMESPACE_UTILS_H
#define NAMESPACE_UTILS_H

extern void PushOverrideEmptySearchPath(MemoryContext memoryContext);
extern List * MakeQualifiedNameListFromRelationId(Oid relationId);

#endif /* NAMESPACE_UTILS_H */
