/*
 * mini_ivm_hook.c
 *
 * PostgreSQL extension entry points and ProcessUtility hook:
 *   - PG_MODULE_MAGIC         – required once per shared library
 *   - mini_ivm_ProcessUtility – chains to previous hook or standard handler
 *   - _PG_init()              – installs the hook on extension load
 *   - _PG_fini()              – restores the previous hook on unload
 */

#include "mini_ivm.h"

PG_MODULE_MAGIC;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/*
 * mini_ivm_ProcessUtility
 *
 * Hook into PostgreSQL's utility statement processing pipeline.
 * Currently a transparent passthrough; extend here to intercept DDL such as
 * "CREATE INCREMENTAL MATERIALIZED VIEW ..." (Phase 3 roadmap item).
 */
static void
mini_ivm_ProcessUtility(PlannedStmt *pstmt,
                        const char *queryString,
                        bool read_only_tree,
                        ProcessUtilityContext context,
                        ParamListInfo params,
                        QueryEnvironment *queryEnv,
                        DestReceiver *dest,
                        QueryCompletion *qc)
{
    if (prev_ProcessUtility)
        prev_ProcessUtility(pstmt, queryString, read_only_tree,
                            context, params, queryEnv, dest, qc);
    else
        standard_ProcessUtility(pstmt, queryString, read_only_tree,
                                context, params, queryEnv, dest, qc);
}

void _PG_init(void);
void _PG_fini(void);

/*
 * _PG_init
 *
 * Called once when the shared library is first loaded.
 * Saves the existing ProcessUtility hook and installs ours.
 */
void
_PG_init(void)
{
    prev_ProcessUtility = ProcessUtility_hook;
    ProcessUtility_hook = mini_ivm_ProcessUtility;
}

/*
 * _PG_fini
 *
 * Called when the extension is unloaded.
 * Restores the original ProcessUtility hook.
 */
void
_PG_fini(void)
{
    ProcessUtility_hook = prev_ProcessUtility;
}
