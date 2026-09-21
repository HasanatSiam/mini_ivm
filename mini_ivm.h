/*
 * mini_ivm.h
 *
 * Shared types, structs, enums, and function prototypes for mini_ivm.
 * All split .c files include this header.
 */

#ifndef MINI_IVM_H
#define MINI_IVM_H

#include "postgres.h"
#include "fmgr.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/datum.h"
#include "catalog/pg_type.h"
#include "catalog/pg_collation.h"
#include "parser/parser.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "nodes/nodes.h"
#include "tcop/utility.h"

/* ----------------------------------------------------------------
 * Aggregate function type enum
 * ---------------------------------------------------------------- */
typedef enum { AGG_SUM, AGG_COUNT, AGG_MIN, AGG_MAX, AGG_AVG } AggFuncType;

/* ----------------------------------------------------------------
 * Single aggregate column definition
 * ---------------------------------------------------------------- */
typedef struct {
    AggFuncType func_type;
    char       *source_column;
    char       *target_column;
    char       *type_name;
    bool        is_star;
} AggDef;

/* ----------------------------------------------------------------
 * Full materialized view configuration
 * ---------------------------------------------------------------- */
typedef struct {
    char   *agg_table;
    char   *source_table;
    int     n_source_tables;
    char  **source_tables;
    int     n_group_cols;
    char  **group_cols;
    char  **group_type_names;
    int     n_aggs;
    AggDef *aggs;
    char   *view_query;
} MvConfig;

/* ----------------------------------------------------------------
 * mini_ivm_utils.c
 * ---------------------------------------------------------------- */
extern bool datum_equal(Datum val1, bool null1, Datum val2, bool null2, Oid type_oid);
extern bool is_ident_char(char c);
extern char *replace_table_name_in_query(const char *query, const char *old_table, const char *new_table);
extern char *get_text_value(HeapTuple tuple, TupleDesc tupdesc, int attnum);
extern char *get_column_type_name_multi(List *tables, const char *column);
extern void  extract_base_tables(Node *node, List **tables);
extern char **split_csv(const char *input, int *out_count);

/* ----------------------------------------------------------------
 * mini_ivm_config.c
 * ---------------------------------------------------------------- */
extern MvConfig *parse_trigger_args(int nargs, char **args);
extern MvConfig *load_mv_config_from_catalog(const char *agg_table_name);
extern void      free_mv_config(MvConfig *cfg);

/* ----------------------------------------------------------------
 * mini_ivm_maintain.c  (PG_FUNCTION_INFO_V1 entry points)
 * ---------------------------------------------------------------- */
extern Datum mini_ivm_maintain(PG_FUNCTION_ARGS);

/* ----------------------------------------------------------------
 * mini_ivm_ddl.c  (PG_FUNCTION_INFO_V1 entry points)
 * ---------------------------------------------------------------- */
extern Datum create_incremental_mv(PG_FUNCTION_ARGS);
extern Datum drop_incremental_mv(PG_FUNCTION_ARGS);

#endif /* MINI_IVM_H */
