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

PG_MODULE_MAGIC;

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

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

void
_PG_init(void)
{
    prev_ProcessUtility = ProcessUtility_hook;
    ProcessUtility_hook = mini_ivm_ProcessUtility;
}

void
_PG_fini(void)
{
    ProcessUtility_hook = prev_ProcessUtility;
}



static bool
datum_equal(Datum val1, bool null1, Datum val2, bool null2, Oid type_oid)
{
    TypeCacheEntry *typentry;

    if (null1 && null2)
        return true;
    if (null1 || null2)
        return false;

    typentry = lookup_type_cache(type_oid, TYPECACHE_EQ_OPR_FINFO);
    if (OidIsValid(typentry->eq_opr) && typentry->eq_opr_finfo.fn_oid != InvalidOid)
    {
        return DatumGetBool(FunctionCall2Coll(&typentry->eq_opr_finfo,
                                              DEFAULT_COLLATION_OID,
                                              val1, val2));
    }

    return datumIsEqual(val1, val2, typentry->typbyval, typentry->typlen);
}


typedef enum { AGG_SUM, AGG_COUNT, AGG_MIN, AGG_MAX, AGG_AVG } AggFuncType;

typedef struct {
    AggFuncType func_type;
    char       *source_column;
    char       *target_column;
    char       *type_name;
    bool        is_star;
} AggDef;

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

static void
extract_base_tables(Node *node, List **tables)
{
    if (node == NULL)
        return;

    if (IsA(node, RangeVar))
    {
        RangeVar *rv = (RangeVar *) node;
        ListCell *lc;
        bool found = false;
        foreach (lc, *tables)
        {
            if (strcmp((char *) lfirst(lc), rv->relname) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            *tables = lappend(*tables, pstrdup(rv->relname));
    }
    else if (IsA(node, JoinExpr))
    {
        JoinExpr *je = (JoinExpr *) node;
        extract_base_tables(je->larg, tables);
        extract_base_tables(je->rarg, tables);
    }
}

static char *
get_column_type_name_multi(List *tables, const char *column)
{
    ListCell *lc;
    foreach (lc, tables)
    {
        char *tbl_name = (char *) lfirst(lc);
        StringInfoData query;
        initStringInfo(&query);
        appendStringInfo(&query,
            "SELECT a.atttypid::regtype::text "
            "FROM pg_catalog.pg_attribute a "
            "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
            "WHERE c.relname = '%s' AND a.attname = '%s' AND a.attnum > 0",
            tbl_name, column);

        if (SPI_execute(query.data, true, 1) == SPI_OK_SELECT && SPI_processed > 0)
        {
            char *res = pstrdup(SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
            SPI_freetuptable(SPI_tuptable);
            return res;
        }
        SPI_freetuptable(SPI_tuptable);
    }
    elog(ERROR, "Column \"%s\" not found in any base table", column);
    return NULL;
}

static bool
is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static char *
replace_table_name_in_query(const char *query, const char *old_table, const char *new_table)
{
    StringInfoData buf;
    int old_len = strlen(old_table);
    const char *p = query;

    initStringInfo(&buf);
    while (*p)
    {
        if (pg_strncasecmp(p, old_table, old_len) == 0)
        {
            bool before_ok = (p == query) || !is_ident_char(*(p - 1));
            bool after_ok = !is_ident_char(*(p + old_len));

            if (before_ok && after_ok)
            {
                appendStringInfoString(&buf, new_table);
                p += old_len;
                continue;
            }
        }
        appendStringInfoChar(&buf, *p);
        p++;
    }
    return buf.data;
}

static char *
get_text_value(HeapTuple tuple, TupleDesc tupdesc, int attnum)
{
    bool isnull;
    (void) SPI_getbinval(tuple, tupdesc, attnum, &isnull);
    if (isnull)
        return NULL;
    return SPI_getvalue(tuple, tupdesc, attnum);
}

static MvConfig *
parse_trigger_args(int nargs, char **args)
{
    MvConfig *cfg;
    char *copy;
    char *tok;
    int i;
    int count;

    if (nargs != 4)
        elog(ERROR, "mini_ivm_maintain requires 4 trigger arguments");

    cfg = palloc0(sizeof(MvConfig));
    cfg->agg_table   = pstrdup(args[0]);
    cfg->source_table = pstrdup(args[1]);

    copy = pstrdup(args[2]);
    count = 1;
    for (char *p = copy; *p; p++)
        if (*p == ',') count++;
    cfg->n_group_cols = count;
    cfg->group_cols = palloc(count * sizeof(char *));
    cfg->group_type_names = palloc(count * sizeof(char *));
    tok = strtok(copy, ",");
    i = 0;
    while (tok && i < count)
    {
        char *colon = strchr(tok, ':');
        if (!colon)
            elog(ERROR, "Invalid group column format: %s", tok);
        *colon = '\0';
        cfg->group_cols[i] = pstrdup(tok);
        cfg->group_type_names[i] = pstrdup(colon + 1);
        i++;
        tok = strtok(NULL, ",");
    }
    pfree(copy);

    copy = pstrdup(args[3]);
    count = 1;
    for (char *p = copy; *p; p++)
        if (*p == ',') count++;
    cfg->n_aggs = count;
    cfg->aggs = palloc(count * sizeof(AggDef));
    tok = strtok(copy, ",");
    i = 0;
    while (tok && i < count)
    {
        char *func_s, *col_s, *alias_s, *type_s;
        func_s = tok;
        col_s  = strchr(func_s, ':');
        if (!col_s) elog(ERROR, "Invalid agg def: %s", tok);
        *col_s++ = '\0';
        alias_s = strchr(col_s, ':');
        if (!alias_s) elog(ERROR, "Invalid agg def: %s", tok);
        *alias_s++ = '\0';
        type_s = strchr(alias_s, ':');
        if (!type_s) elog(ERROR, "Invalid agg def: %s", tok);
        *type_s++ = '\0';

        if (pg_strcasecmp(func_s, "SUM") == 0)
            cfg->aggs[i].func_type = AGG_SUM;
        else if (pg_strcasecmp(func_s, "COUNT") == 0)
            cfg->aggs[i].func_type = AGG_COUNT;
        else if (pg_strcasecmp(func_s, "MIN") == 0)
            cfg->aggs[i].func_type = AGG_MIN;
        else if (pg_strcasecmp(func_s, "MAX") == 0)
            cfg->aggs[i].func_type = AGG_MAX;
        else if (pg_strcasecmp(func_s, "AVG") == 0)
            cfg->aggs[i].func_type = AGG_AVG;
        else
            elog(ERROR, "Unknown aggregate: %s", func_s);

        cfg->aggs[i].source_column = pstrdup(col_s);
        cfg->aggs[i].target_column = pstrdup(alias_s);
        cfg->aggs[i].type_name     = pstrdup(type_s);
        cfg->aggs[i].is_star       = (strcmp(col_s, "*") == 0);
        i++;
        tok = strtok(NULL, ",");
    }
    pfree(copy);

    return cfg;
}

static void
free_mv_config(MvConfig *cfg)
{
    (void) cfg;
}

static char **
split_csv(const char *input, int *out_count)
{
    char **tokens;
    char *copy, *p, *start;
    int count = 1, i = 0;

    if (!input || !*input)
    {
        *out_count = 0;
        return NULL;
    }

    for (p = (char *)input; *p; p++)
        if (*p == ',') count++;

    tokens = palloc0(count * sizeof(char *));
    copy = pstrdup(input);
    start = copy;

    for (p = copy; *p != '\0'; p++)
    {
        if (*p == ',')
        {
            *p = '\0';
            if (i < count)
                tokens[i++] = pstrdup(start);
            start = p + 1;
        }
    }
    if (i < count && start)
        tokens[i++] = pstrdup(start);

    pfree(copy);
    *out_count = i;
    return tokens;
}

static MvConfig *
load_mv_config_from_catalog(const char *agg_table_name)
{
    StringInfoData query;
    MvConfig *cfg;
    char *src_table, *g_cols_raw, *a_def_raw, *v_query;
    char **g_tokens, **a_tokens;
    int n_g, n_a, i;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT source_table, group_cols, aggs_def, view_query "
        "FROM mini_ivm_catalog WHERE agg_table = '%s'",
        agg_table_name);

    if (SPI_execute(query.data, true, 1) != SPI_OK_SELECT || SPI_processed == 0)
        elog(ERROR, "Catalog entry for IMMV table \"%s\" not found", agg_table_name);

    {
        char *v1 = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        char *v2 = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2);
        char *v3 = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3);
        char *v4 = (SPI_tuptable->tupdesc->natts >= 4) ? SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 4) : NULL;

        if (!v1 || !v2 || !v3)
            elog(ERROR, "Catalog tuple for \"%s\" contains NULL values", agg_table_name);

        src_table  = pstrdup(v1);
        g_cols_raw = pstrdup(v2);
        a_def_raw  = pstrdup(v3);
        v_query    = v4 ? pstrdup(v4) : NULL;
    }
    SPI_freetuptable(SPI_tuptable);

    cfg = palloc0(sizeof(MvConfig));
    cfg->agg_table     = pstrdup(agg_table_name);
    cfg->source_table  = src_table;
    cfg->source_tables = split_csv(src_table, &cfg->n_source_tables);
    cfg->view_query    = v_query;

    g_tokens = split_csv(g_cols_raw, &n_g);
    cfg->n_group_cols = n_g;
    cfg->group_cols = palloc0(n_g * sizeof(char *));
    cfg->group_type_names = palloc0(n_g * sizeof(char *));

    for (i = 0; i < n_g; i++)
    {
        char *tok = g_tokens[i];
        char *colon = strchr(tok, ':');
        if (!colon)
            elog(ERROR, "Invalid group col in catalog: %s", tok);
        *colon = '\0';
        cfg->group_cols[i] = pstrdup(tok);
        cfg->group_type_names[i] = pstrdup(colon + 1);
    }

    a_tokens = split_csv(a_def_raw, &n_a);
    cfg->n_aggs = n_a;
    cfg->aggs = palloc0(n_a * sizeof(AggDef));

    for (i = 0; i < n_a; i++)
    {
        char *func_s, *col_s, *alias_s, *type_s;
        char *p1, *p2, *p3;
        char *tok = a_tokens[i];

        p1 = strchr(tok, ':');
        p2 = p1 ? strchr(p1 + 1, ':') : NULL;
        p3 = p2 ? strchr(p2 + 1, ':') : NULL;

        if (!p1 || !p2 || !p3)
            elog(ERROR, "Invalid agg def token in catalog: \"%s\" (raw aggs_def=\"%s\")", tok, a_def_raw);

        *p1 = '\0';
        *p2 = '\0';
        *p3 = '\0';

        func_s  = tok;
        col_s   = p1 + 1;
        alias_s = p2 + 1;
        type_s  = p3 + 1;

        if (strcasecmp(func_s, "SUM") == 0)
            cfg->aggs[i].func_type = AGG_SUM;
        else if (strcasecmp(func_s, "COUNT") == 0)
            cfg->aggs[i].func_type = AGG_COUNT;
        else if (strcasecmp(func_s, "MIN") == 0)
            cfg->aggs[i].func_type = AGG_MIN;
        else if (strcasecmp(func_s, "MAX") == 0)
            cfg->aggs[i].func_type = AGG_MAX;
        else if (strcasecmp(func_s, "AVG") == 0)
            cfg->aggs[i].func_type = AGG_AVG;
        else
            elog(ERROR, "Unknown aggregate in catalog: %s", func_s);

        cfg->aggs[i].is_star       = (col_s != NULL && strcmp(col_s, "*") == 0);
        cfg->aggs[i].source_column = pstrdup(col_s);
        cfg->aggs[i].target_column = pstrdup(alias_s);
        cfg->aggs[i].type_name     = pstrdup(type_s);
    }

    for (i = 0; i < n_g; i++)
        if (g_tokens[i]) pfree(g_tokens[i]);
    if (g_tokens) pfree(g_tokens);
    pfree(g_cols_raw);

    for (i = 0; i < n_a; i++)
        if (a_tokens[i]) pfree(a_tokens[i]);
    if (a_tokens) pfree(a_tokens);
    pfree(a_def_raw);

    return cfg;
}

static void
build_group_where(StringInfo buf, HeapTuple tuple, TupleDesc tupdesc, MvConfig *cfg)
{
    int i;

    for (i = 0; i < cfg->n_group_cols; i++)
    {
        int attnum = SPI_fnumber(tupdesc, cfg->group_cols[i]);
        char *val;

        if (attnum == SPI_ERROR_NOATTRIBUTE)
            elog(ERROR, "Column \"%s\" not found", cfg->group_cols[i]);
        val = get_text_value(tuple, tupdesc, attnum);
        if (i > 0) appendStringInfoString(buf, " AND ");
        if (val == NULL)
            appendStringInfo(buf, "%s IS NULL", cfg->group_cols[i]);
        else
        {
            char *quoted = quote_literal_cstr(val);
            appendStringInfo(buf, "%s = CAST(%s AS %s)",
                           cfg->group_cols[i], quoted, cfg->group_type_names[i]);
        }
    }
}

static void
build_group_values_list(StringInfo buf, HeapTuple tuple, TupleDesc tupdesc, MvConfig *cfg)
{
    int i;

    for (i = 0; i < cfg->n_group_cols; i++)
    {
        int attnum = SPI_fnumber(tupdesc, cfg->group_cols[i]);
        char *val;

        if (attnum == SPI_ERROR_NOATTRIBUTE)
            elog(ERROR, "Column \"%s\" not found", cfg->group_cols[i]);
        val = get_text_value(tuple, tupdesc, attnum);
        if (i > 0) appendStringInfoString(buf, ", ");
        if (val == NULL)
            appendStringInfoString(buf, "NULL");
        else
        {
            char *quoted = quote_literal_cstr(val);
            appendStringInfo(buf, "CAST(%s AS %s)", quoted, cfg->group_type_names[i]);
        }
    }
}

static void
build_agg_values_list(StringInfo buf, HeapTuple tuple, TupleDesc tupdesc, MvConfig *cfg)
{
    int i;

    for (i = 0; i < cfg->n_aggs; i++)
    {
        AggDef *agg = &cfg->aggs[i];
        if (i > 0) appendStringInfoString(buf, ", ");
        if (agg->func_type == AGG_COUNT && agg->is_star)
        {
            appendStringInfoString(buf, "1");
        }
        else if (agg->func_type == AGG_AVG)
        {
            int attnum = SPI_fnumber(tupdesc, agg->source_column);
            char *val;

            if (attnum == SPI_ERROR_NOATTRIBUTE)
                elog(ERROR, "Column \"%s\" not found", agg->source_column);
            val = get_text_value(tuple, tupdesc, attnum);
            if (val == NULL)
                appendStringInfoString(buf, "NULL, NULL, 0");
            else
            {
                char *quoted = quote_literal_cstr(val);
                appendStringInfo(buf, "CAST(%s AS %s), CAST(%s AS %s), 1",
                                quoted, agg->type_name, quoted, agg->type_name);
            }
        }
        else
        {
            int attnum = SPI_fnumber(tupdesc, agg->source_column);
            char *val;

            if (attnum == SPI_ERROR_NOATTRIBUTE)
                elog(ERROR, "Column \"%s\" not found", agg->source_column);
            val = get_text_value(tuple, tupdesc, attnum);
            if (val == NULL)
                appendStringInfoString(buf, "NULL");
            else
            {
                char *quoted = quote_literal_cstr(val);
                appendStringInfo(buf, "CAST(%s AS %s)", quoted, agg->type_name);
            }
        }
    }
}

static void
apply_insert(HeapTuple tuple, TupleDesc tupdesc, MvConfig *cfg)
{
    StringInfoData query;
    int i;

    initStringInfo(&query);
    appendStringInfo(&query, "INSERT INTO %s (", cfg->agg_table);
    for (i = 0; i < cfg->n_group_cols; i++)
        appendStringInfo(&query, "%s, ", cfg->group_cols[i]);
    for (i = 0; i < cfg->n_aggs; i++)
    {
        if (cfg->aggs[i].func_type == AGG_AVG)
        {
            appendStringInfo(&query, "%s, __sum_%s, __count_%s%s",
                            cfg->aggs[i].target_column,
                            cfg->aggs[i].target_column,
                            cfg->aggs[i].target_column,
                            (i == cfg->n_aggs - 1) ? "" : ", ");
        }
        else
        {
            appendStringInfo(&query, "%s%s", cfg->aggs[i].target_column,
                            (i == cfg->n_aggs - 1) ? "" : ", ");
        }
    }
    appendStringInfoString(&query, ") VALUES (");

    build_group_values_list(&query, tuple, tupdesc, cfg);
    appendStringInfoString(&query, ", ");
    build_agg_values_list(&query, tuple, tupdesc, cfg);
    appendStringInfoString(&query, ") ON CONFLICT (");

    for (i = 0; i < cfg->n_group_cols; i++)
        appendStringInfo(&query, "%s%s", cfg->group_cols[i],
                        (i == cfg->n_group_cols - 1) ? "" : ", ");
    appendStringInfoString(&query, ") DO UPDATE SET ");

    for (i = 0; i < cfg->n_aggs; i++)
    {
        AggDef *agg = &cfg->aggs[i];
        if (i > 0) appendStringInfoString(&query, ", ");

        switch (agg->func_type)
        {
            case AGG_SUM:
                appendStringInfo(&query, "%s = %s.%s + EXCLUDED.%s",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_COUNT:
                appendStringInfo(&query, "%s = %s.%s + EXCLUDED.%s",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MIN:
                appendStringInfo(&query, "%s = LEAST(%s.%s, EXCLUDED.%s)",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MAX:
                appendStringInfo(&query, "%s = GREATEST(%s.%s, EXCLUDED.%s)",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_AVG:
                appendStringInfo(&query,
                    "__sum_%s = CASE WHEN %s.__sum_%s IS NULL AND EXCLUDED.__sum_%s IS NULL THEN NULL ELSE COALESCE(%s.__sum_%s, 0) + COALESCE(EXCLUDED.__sum_%s, 0) END, "
                    "__count_%s = COALESCE(%s.__count_%s, 0) + EXCLUDED.__count_%s, "
                    "%s = CASE WHEN (COALESCE(%s.__count_%s, 0) + EXCLUDED.__count_%s) = 0 THEN NULL ELSE (COALESCE(%s.__sum_%s, 0) + COALESCE(EXCLUDED.__sum_%s, 0)) / (COALESCE(%s.__count_%s, 0) + EXCLUDED.__count_%s) END",
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column,
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column,
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
        }
    }

    SPI_execute(query.data, false, 0);
}

static void
apply_delete(HeapTuple tuple, TupleDesc tupdesc, MvConfig *cfg)
{
    StringInfoData where;
    StringInfoData set_clause;
    StringInfoData query;
    int i;
    bool deleted = false;

    initStringInfo(&where);
    build_group_where(&where, tuple, tupdesc, cfg);

    initStringInfo(&set_clause);
    for (i = 0; i < cfg->n_aggs; i++)
    {
        AggDef *agg = &cfg->aggs[i];
        if (i > 0) appendStringInfoString(&set_clause, ", ");

        switch (agg->func_type)
        {
            case AGG_SUM:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *val = get_text_value(tuple, tupdesc, attnum);
                appendStringInfo(&set_clause, "%s = ", agg->target_column);
                if (val == NULL)
                    appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                else
                {
                    char *quoted = quote_literal_cstr(val);
                    appendStringInfo(&set_clause, "%s.%s - CAST(%s AS %s)",
                                    cfg->agg_table, agg->target_column, quoted, agg->type_name);
                }
                break;
            }
            case AGG_COUNT:
                appendStringInfo(&set_clause, "%s = %s.%s - 1",
                                agg->target_column, cfg->agg_table, agg->target_column);
                break;
            case AGG_MIN:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *val = get_text_value(tuple, tupdesc, attnum);
                appendStringInfo(&set_clause, "%s = ", agg->target_column);
                if (val == NULL)
                    appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                else
                {
                    char *quoted = quote_literal_cstr(val);
                    appendStringInfo(&set_clause,
                        "CASE WHEN CAST(%s AS %s) = %s.%s "
                        "THEN (SELECT COALESCE(MIN(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                        "ELSE %s.%s END",
                        quoted, agg->type_name,
                        cfg->agg_table, agg->target_column,
                        agg->source_column, agg->type_name,
                        cfg->source_table, where.data,
                        cfg->agg_table, agg->target_column);
                }
                break;
            }
            case AGG_MAX:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *val = get_text_value(tuple, tupdesc, attnum);
                appendStringInfo(&set_clause, "%s = ", agg->target_column);
                if (val == NULL)
                    appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                else
                {
                    char *quoted = quote_literal_cstr(val);
                    appendStringInfo(&set_clause,
                        "CASE WHEN CAST(%s AS %s) = %s.%s "
                        "THEN (SELECT COALESCE(MAX(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                        "ELSE %s.%s END",
                        quoted, agg->type_name,
                        cfg->agg_table, agg->target_column,
                        agg->source_column, agg->type_name,
                        cfg->source_table, where.data,
                        cfg->agg_table, agg->target_column);
                }
                break;
            }
            case AGG_AVG:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *val = get_text_value(tuple, tupdesc, attnum);
                if (val == NULL)
                {
                    appendStringInfo(&set_clause,
                        "__sum_%s = %s.__sum_%s, __count_%s = %s.__count_%s, %s = %s.%s",
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column);
                }
                else
                {
                    char *quoted = quote_literal_cstr(val);
                    appendStringInfo(&set_clause,
                        "__sum_%s = CASE WHEN (%s.__count_%s - 1) <= 0 THEN NULL ELSE %s.__sum_%s - CAST(%s AS %s) END, "
                        "__count_%s = %s.__count_%s - 1, "
                        "%s = CASE WHEN (%s.__count_%s - 1) <= 0 THEN NULL ELSE (%s.__sum_%s - CAST(%s AS %s)) / (%s.__count_%s - 1) END",
                        agg->target_column, cfg->agg_table, agg->target_column, cfg->agg_table, agg->target_column, quoted, agg->type_name,
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column, cfg->agg_table, agg->target_column, quoted, agg->type_name, cfg->agg_table, agg->target_column);
                }
                break;
            }
        }
    }

    initStringInfo(&query);
    appendStringInfo(&query, "UPDATE %s SET %s WHERE %s",
                    cfg->agg_table, set_clause.data, where.data);
    SPI_execute(query.data, false, 0);

    for (i = 0; i < cfg->n_aggs; i++)
    {
        if (cfg->aggs[i].func_type == AGG_COUNT)
        {
            resetStringInfo(&query);
            appendStringInfo(&query, "DELETE FROM %s WHERE %s AND %s <= 0",
                            cfg->agg_table, where.data, cfg->aggs[i].target_column);
            SPI_execute(query.data, false, 0);
            deleted = true;
            break;
        }
        else if (cfg->aggs[i].func_type == AGG_AVG)
        {
            resetStringInfo(&query);
            appendStringInfo(&query, "DELETE FROM %s WHERE %s AND __count_%s <= 0",
                            cfg->agg_table, where.data, cfg->aggs[i].target_column);
            SPI_execute(query.data, false, 0);
            deleted = true;
            break;
        }
    }

    if (!deleted)
    {
        resetStringInfo(&query);
        appendStringInfo(&query, "DELETE FROM %s WHERE %s AND (SELECT COUNT(*) FROM %s WHERE %s) = 0",
                        cfg->agg_table, where.data, cfg->source_table, where.data);
        SPI_execute(query.data, false, 0);
    }
}

static void
apply_update(HeapTuple oldtuple, HeapTuple newtuple, TupleDesc tupdesc, MvConfig *cfg)
{
    bool group_changed = false;
    bool value_changed = false;
    StringInfoData where;
    StringInfoData set_clause;
    StringInfoData query;
    int i;

    for (i = 0; i < cfg->n_group_cols; i++)
    {
        int attnum = SPI_fnumber(tupdesc, cfg->group_cols[i]);
        Datum old_val, new_val;
        bool old_null, new_null;
        Oid type_oid;

        if (attnum == SPI_ERROR_NOATTRIBUTE)
            elog(ERROR, "Column \"%s\" not found", cfg->group_cols[i]);

        type_oid = TupleDescAttr(tupdesc, attnum - 1)->atttypid;
        old_val = SPI_getbinval(oldtuple, tupdesc, attnum, &old_null);
        new_val = SPI_getbinval(newtuple, tupdesc, attnum, &new_null);

        if (!datum_equal(old_val, old_null, new_val, new_null, type_oid))
        {
            group_changed = true;
            break;
        }
    }

    if (group_changed)
    {
        apply_delete(oldtuple, tupdesc, cfg);
        apply_insert(newtuple, tupdesc, cfg);
        return;
    }

    for (i = 0; i < cfg->n_aggs; i++)
    {
        AggDef *agg = &cfg->aggs[i];
        int attnum;
        Datum old_val, new_val;
        bool old_null, new_null;
        Oid type_oid;

        if (agg->func_type == AGG_COUNT)
            continue;
        attnum = SPI_fnumber(tupdesc, agg->source_column);
        if (attnum == SPI_ERROR_NOATTRIBUTE)
            continue;

        type_oid = TupleDescAttr(tupdesc, attnum - 1)->atttypid;
        old_val = SPI_getbinval(oldtuple, tupdesc, attnum, &old_null);
        new_val = SPI_getbinval(newtuple, tupdesc, attnum, &new_null);

        if (!datum_equal(old_val, old_null, new_val, new_null, type_oid))
        {
            value_changed = true;
            break;
        }
    }

    if (!value_changed)
        return;

    initStringInfo(&where);
    build_group_where(&where, newtuple, tupdesc, cfg);

    initStringInfo(&set_clause);
    for (i = 0; i < cfg->n_aggs; i++)
    {
        AggDef *agg = &cfg->aggs[i];
        if (i > 0) appendStringInfoString(&set_clause, ", ");

        switch (agg->func_type)
        {
            case AGG_SUM:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *old_val = get_text_value(oldtuple, tupdesc, attnum);
                char *new_val = get_text_value(newtuple, tupdesc, attnum);
                appendStringInfo(&set_clause, "%s = ", agg->target_column);
                if (old_val == NULL && new_val == NULL)
                    appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                else if (old_val == NULL)
                {
                    char *quoted = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause, "%s.%s + CAST(%s AS %s)",
                                    cfg->agg_table, agg->target_column, quoted, agg->type_name);
                }
                else if (new_val == NULL)
                {
                    char *quoted = quote_literal_cstr(old_val);
                    appendStringInfo(&set_clause, "%s.%s - CAST(%s AS %s)",
                                    cfg->agg_table, agg->target_column, quoted, agg->type_name);
                }
                else
                {
                    char *o = quote_literal_cstr(old_val);
                    char *n = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "%s.%s + (CAST(%s AS %s) - CAST(%s AS %s))",
                        cfg->agg_table, agg->target_column,
                        n, agg->type_name, o, agg->type_name);
                }
                break;
            }
            case AGG_COUNT:
                appendStringInfo(&set_clause, "%s = %s.%s", agg->target_column, cfg->agg_table, agg->target_column);
                break;
            case AGG_MIN:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *old_val = get_text_value(oldtuple, tupdesc, attnum);
                char *new_val = get_text_value(newtuple, tupdesc, attnum);
                appendStringInfo(&set_clause, "%s = ", agg->target_column);
                if (old_val == NULL && new_val == NULL)
                    appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                else if (old_val == NULL)
                {
                    char *quoted = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "LEAST(%s.%s, CAST(%s AS %s))",
                        cfg->agg_table, agg->target_column, quoted, agg->type_name);
                }
                else if (new_val == NULL)
                {
                    char *old_q = quote_literal_cstr(old_val);
                    appendStringInfo(&set_clause,
                        "CASE WHEN CAST(%s AS %s) = %s.%s "
                        "THEN (SELECT COALESCE(MIN(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                        "ELSE %s.%s END",
                        old_q, agg->type_name,
                        cfg->agg_table, agg->target_column,
                        agg->source_column, agg->type_name,
                        cfg->source_table, where.data,
                        cfg->agg_table, agg->target_column);
                }
                else
                {
                    char *old_q = quote_literal_cstr(old_val);
                    char *new_q = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "CASE WHEN CAST(%s AS %s) = %s.%s "
                        "THEN (SELECT COALESCE(MIN(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                        "ELSE LEAST(%s.%s, CAST(%s AS %s)) END",
                        old_q, agg->type_name,
                        cfg->agg_table, agg->target_column,
                        agg->source_column, agg->type_name,
                        cfg->source_table, where.data,
                        cfg->agg_table, agg->target_column, new_q, agg->type_name);
                }
                break;
            }
            case AGG_MAX:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *old_val = get_text_value(oldtuple, tupdesc, attnum);
                char *new_val = get_text_value(newtuple, tupdesc, attnum);
                appendStringInfo(&set_clause, "%s = ", agg->target_column);
                if (old_val == NULL && new_val == NULL)
                    appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                else if (old_val == NULL)
                {
                    char *quoted = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "GREATEST(%s.%s, CAST(%s AS %s))",
                        cfg->agg_table, agg->target_column, quoted, agg->type_name);
                }
                else if (new_val == NULL)
                {
                    char *old_q = quote_literal_cstr(old_val);
                    appendStringInfo(&set_clause,
                        "CASE WHEN CAST(%s AS %s) = %s.%s "
                        "THEN (SELECT COALESCE(MAX(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                        "ELSE %s.%s END",
                        old_q, agg->type_name,
                        cfg->agg_table, agg->target_column,
                        agg->source_column, agg->type_name,
                        cfg->source_table, where.data,
                        cfg->agg_table, agg->target_column);
                }
                else
                {
                    char *old_q = quote_literal_cstr(old_val);
                    char *new_q = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "CASE WHEN CAST(%s AS %s) = %s.%s "
                        "THEN (SELECT COALESCE(MAX(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                        "ELSE GREATEST(%s.%s, CAST(%s AS %s)) END",
                        old_q, agg->type_name,
                        cfg->agg_table, agg->target_column,
                        agg->source_column, agg->type_name,
                        cfg->source_table, where.data,
                        cfg->agg_table, agg->target_column, new_q, agg->type_name);
                }
                break;
            }
            case AGG_AVG:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *old_val = get_text_value(oldtuple, tupdesc, attnum);
                char *new_val = get_text_value(newtuple, tupdesc, attnum);

                if (old_val == NULL && new_val == NULL)
                {
                    appendStringInfo(&set_clause,
                        "__sum_%s = %s.__sum_%s, __count_%s = %s.__count_%s, %s = %s.%s",
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column);
                }
                else if (old_val == NULL)
                {
                    char *quoted = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "__sum_%s = COALESCE(%s.__sum_%s, 0) + CAST(%s AS %s), "
                        "__count_%s = %s.__count_%s + 1, "
                        "%s = (COALESCE(%s.__sum_%s, 0) + CAST(%s AS %s)) / (%s.__count_%s + 1)",
                        agg->target_column, cfg->agg_table, agg->target_column, quoted, agg->type_name,
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column, quoted, agg->type_name, cfg->agg_table, agg->target_column);
                }
                else if (new_val == NULL)
                {
                    char *quoted = quote_literal_cstr(old_val);
                    appendStringInfo(&set_clause,
                        "__sum_%s = CASE WHEN (%s.__count_%s - 1) <= 0 THEN NULL ELSE %s.__sum_%s - CAST(%s AS %s) END, "
                        "__count_%s = %s.__count_%s - 1, "
                        "%s = CASE WHEN (%s.__count_%s - 1) <= 0 THEN NULL ELSE (%s.__sum_%s - CAST(%s AS %s)) / (%s.__count_%s - 1) END",
                        agg->target_column, cfg->agg_table, agg->target_column, cfg->agg_table, agg->target_column, quoted, agg->type_name,
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column, cfg->agg_table, agg->target_column, quoted, agg->type_name, cfg->agg_table, agg->target_column);
                }
                else
                {
                    char *o = quote_literal_cstr(old_val);
                    char *n = quote_literal_cstr(new_val);
                    appendStringInfo(&set_clause,
                        "__sum_%s = %s.__sum_%s + (CAST(%s AS %s) - CAST(%s AS %s)), "
                        "__count_%s = %s.__count_%s, "
                        "%s = CASE WHEN %s.__count_%s <= 0 THEN NULL ELSE (%s.__sum_%s + (CAST(%s AS %s) - CAST(%s AS %s))) / %s.__count_%s END",
                        agg->target_column, cfg->agg_table, agg->target_column, n, agg->type_name, o, agg->type_name,
                        agg->target_column, cfg->agg_table, agg->target_column,
                        agg->target_column, cfg->agg_table, agg->target_column, cfg->agg_table, agg->target_column, n, agg->type_name, o, agg->type_name, cfg->agg_table, agg->target_column);
                }
                break;
            }
        }
    }

    initStringInfo(&query);
    appendStringInfo(&query, "UPDATE %s SET %s WHERE %s",
                    cfg->agg_table, set_clause.data, where.data);
    SPI_execute(query.data, false, 0);
}

static void
apply_statement_insert_delta(MvConfig *cfg, const char *triggered_table)
{
    StringInfoData query;
    char *delta_subquery;
    int j;

    if (!cfg->view_query)
        return;

    delta_subquery = replace_table_name_in_query(cfg->view_query, triggered_table, "new_table");

    initStringInfo(&query);
    appendStringInfo(&query, "INSERT INTO %s (", cfg->agg_table);
    for (j = 0; j < cfg->n_group_cols; j++)
        appendStringInfo(&query, "%s, ", cfg->group_cols[j]);
    for (j = 0; j < cfg->n_aggs; j++)
    {
        if (cfg->aggs[j].func_type == AGG_AVG)
        {
            appendStringInfo(&query, "%s, __sum_%s, __count_%s%s",
                            cfg->aggs[j].target_column,
                            cfg->aggs[j].target_column,
                            cfg->aggs[j].target_column,
                            (j == cfg->n_aggs - 1) ? "" : ", ");
        }
        else
        {
            appendStringInfo(&query, "%s%s", cfg->aggs[j].target_column,
                            (j == cfg->n_aggs - 1) ? "" : ", ");
        }
    }

    appendStringInfoString(&query, ") SELECT ");
    for (j = 0; j < cfg->n_group_cols; j++)
        appendStringInfo(&query, "delta.%s, ", cfg->group_cols[j]);
    for (j = 0; j < cfg->n_aggs; j++)
    {
        AggDef *agg = &cfg->aggs[j];
        if (agg->func_type == AGG_AVG)
        {
            appendStringInfo(&query, "delta.%s, delta.__sum_%s, delta.__count_%s%s",
                            agg->target_column, agg->target_column, agg->target_column,
                            (j == cfg->n_aggs - 1) ? "" : ", ");
        }
        else
        {
            appendStringInfo(&query, "delta.%s%s", agg->target_column,
                            (j == cfg->n_aggs - 1) ? "" : ", ");
        }
    }
    appendStringInfo(&query, " FROM (%s) AS delta ", delta_subquery);
    appendStringInfoString(&query, "ON CONFLICT (");
    for (j = 0; j < cfg->n_group_cols; j++)
        appendStringInfo(&query, "%s%s", cfg->group_cols[j],
                        (j == cfg->n_group_cols - 1) ? "" : ", ");
    appendStringInfoString(&query, ") DO UPDATE SET ");

    for (j = 0; j < cfg->n_aggs; j++)
    {
        AggDef *agg = &cfg->aggs[j];
        if (j > 0) appendStringInfoString(&query, ", ");
        switch (agg->func_type)
        {
            case AGG_SUM:
            case AGG_COUNT:
                appendStringInfo(&query, "%s = %s.%s + EXCLUDED.%s",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MIN:
                appendStringInfo(&query, "%s = LEAST(%s.%s, EXCLUDED.%s)",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MAX:
                appendStringInfo(&query, "%s = GREATEST(%s.%s, EXCLUDED.%s)",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_AVG:
                appendStringInfo(&query,
                    "__sum_%s = CASE WHEN %s.__sum_%s IS NULL AND EXCLUDED.__sum_%s IS NULL THEN NULL ELSE COALESCE(%s.__sum_%s, 0) + COALESCE(EXCLUDED.__sum_%s, 0) END, "
                    "__count_%s = COALESCE(%s.__count_%s, 0) + EXCLUDED.__count_%s, "
                    "%s = CASE WHEN (COALESCE(%s.__count_%s, 0) + EXCLUDED.__count_%s) = 0 THEN NULL ELSE (COALESCE(%s.__sum_%s, 0) + COALESCE(EXCLUDED.__sum_%s, 0)) / (COALESCE(%s.__count_%s, 0) + EXCLUDED.__count_%s) END",
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column,
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column,
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
        }
    }

    SPI_execute(query.data, false, 0);
    pfree(delta_subquery);
}

static void
apply_statement_delete_delta(MvConfig *cfg, const char *triggered_table)
{
    StringInfoData query;
    StringInfoData where_match;
    char *delta_subquery;
    int j;

    if (!cfg->view_query)
        return;

    delta_subquery = replace_table_name_in_query(cfg->view_query, triggered_table, "old_table");

    initStringInfo(&where_match);
    for (j = 0; j < cfg->n_group_cols; j++)
    {
        if (j > 0) appendStringInfoString(&where_match, " AND ");
        appendStringInfo(&where_match, "%s.%s = delta.%s",
                        cfg->agg_table, cfg->group_cols[j], cfg->group_cols[j]);
    }

    initStringInfo(&query);
    appendStringInfo(&query, "UPDATE %s SET ", cfg->agg_table);
    for (j = 0; j < cfg->n_aggs; j++)
    {
        AggDef *agg = &cfg->aggs[j];
        if (j > 0) appendStringInfoString(&query, ", ");

        switch (agg->func_type)
        {
            case AGG_SUM:
            case AGG_COUNT:
                appendStringInfo(&query, "%s = %s.%s - delta.%s",
                                agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MIN:
            {
                StringInfoData sub_where;
                initStringInfo(&sub_where);
                for (int k = 0; k < cfg->n_group_cols; k++)
                {
                    if (k > 0) appendStringInfoString(&sub_where, " AND ");
                    appendStringInfo(&sub_where, "%s = %s.%s",
                                    cfg->group_cols[k], cfg->agg_table, cfg->group_cols[k]);
                }
                appendStringInfo(&query,
                    "%s = CASE WHEN delta.%s = %s.%s "
                    "THEN (SELECT COALESCE(MIN(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                    "ELSE %s.%s END",
                    agg->target_column, agg->target_column, cfg->agg_table, agg->target_column,
                    agg->source_column, agg->type_name,
                    cfg->source_table, sub_where.data,
                    cfg->agg_table, agg->target_column);
                break;
            }
            case AGG_MAX:
            {
                StringInfoData sub_where;
                initStringInfo(&sub_where);
                for (int k = 0; k < cfg->n_group_cols; k++)
                {
                    if (k > 0) appendStringInfoString(&sub_where, " AND ");
                    appendStringInfo(&sub_where, "%s = %s.%s",
                                    cfg->group_cols[k], cfg->agg_table, cfg->group_cols[k]);
                }
                appendStringInfo(&query,
                    "%s = CASE WHEN delta.%s = %s.%s "
                    "THEN (SELECT COALESCE(MAX(%s), CAST(0 AS %s)) FROM %s WHERE %s) "
                    "ELSE %s.%s END",
                    agg->target_column, agg->target_column, cfg->agg_table, agg->target_column,
                    agg->source_column, agg->type_name,
                    cfg->source_table, sub_where.data,
                    cfg->agg_table, agg->target_column);
                break;
            }
            case AGG_AVG:
                appendStringInfo(&query,
                    "__sum_%s = CASE WHEN (%s.__count_%s - delta.__count_%s) <= 0 THEN NULL ELSE %s.__sum_%s - delta.__sum_%s END, "
                    "__count_%s = %s.__count_%s - delta.__count_%s, "
                    "%s = CASE WHEN (%s.__count_%s - delta.__count_%s) <= 0 THEN NULL ELSE (%s.__sum_%s - delta.__sum_%s) / (%s.__count_%s - delta.__count_%s) END",
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column,
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column,
                    agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column, cfg->agg_table, agg->target_column, agg->target_column);
                break;
        }
    }

    appendStringInfo(&query, " FROM (%s) AS delta WHERE %s", delta_subquery, where_match.data);
    SPI_execute(query.data, false, 0);

    /* Delete rows where count aggregate <= 0 */
    for (j = 0; j < cfg->n_aggs; j++)
    {
        if (cfg->aggs[j].func_type == AGG_COUNT)
        {
            resetStringInfo(&query);
            appendStringInfo(&query, "DELETE FROM %s WHERE %s <= 0",
                            cfg->agg_table, cfg->aggs[j].target_column);
            SPI_execute(query.data, false, 0);
            break;
        }
        else if (cfg->aggs[j].func_type == AGG_AVG)
        {
            resetStringInfo(&query);
            appendStringInfo(&query, "DELETE FROM %s WHERE __count_%s <= 0",
                            cfg->agg_table, cfg->aggs[j].target_column);
            SPI_execute(query.data, false, 0);
            break;
        }
    }

    pfree(delta_subquery);
}

static void
apply_statement_update_delta(MvConfig *cfg, const char *triggered_table)
{
    apply_statement_delete_delta(cfg, triggered_table);
    apply_statement_insert_delta(cfg, triggered_table);
}

PG_FUNCTION_INFO_V1(mini_ivm_maintain);

Datum
mini_ivm_maintain(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata;
    Trigger *trigger;
    MvConfig *cfg;
    const char *triggered_table;

    if (!CALLED_AS_TRIGGER(fcinfo))
        elog(ERROR, "mini_ivm_maintain must be called as a trigger");

    trigdata = (TriggerData *) fcinfo->context;
    trigger = trigdata->tg_trigger;
    triggered_table = trigdata->tg_relation->rd_rel->relname.data;

    SPI_connect();

    if (trigger->tgnargs == 1)
        cfg = load_mv_config_from_catalog(trigger->tgargs[0]);
    else
        cfg = parse_trigger_args(trigger->tgnargs, trigger->tgargs);

    if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
    {
        SPI_register_trigger_data(trigdata);

        if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
            apply_statement_insert_delta(cfg, triggered_table);
        else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
            apply_statement_delete_delta(cfg, triggered_table);
        else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
            apply_statement_update_delta(cfg, triggered_table);

        SPI_finish();
        free_mv_config(cfg);
        PG_RETURN_POINTER(NULL);
    }

    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
        apply_insert(trigdata->tg_trigtuple, trigdata->tg_relation->rd_att, cfg);
    else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
        apply_delete(trigdata->tg_trigtuple, trigdata->tg_relation->rd_att, cfg);
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
        apply_update(trigdata->tg_trigtuple, trigdata->tg_newtuple, trigdata->tg_relation->rd_att, cfg);

    SPI_finish();

    free_mv_config(cfg);

    if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
        PG_RETURN_POINTER(trigdata->tg_newtuple);
    else
        PG_RETURN_POINTER(trigdata->tg_trigtuple);
}

static bool
is_agg_func_name(const char *name)
{
    return pg_strcasecmp(name, "sum") == 0 ||
           pg_strcasecmp(name, "count") == 0 ||
           pg_strcasecmp(name, "min")  == 0 ||
           pg_strcasecmp(name, "max")  == 0 ||
           pg_strcasecmp(name, "avg")  == 0;
}

static AggFuncType
agg_func_from_name(const char *name)
{
    if (pg_strcasecmp(name, "sum")   == 0) return AGG_SUM;
    if (pg_strcasecmp(name, "count") == 0) return AGG_COUNT;
    if (pg_strcasecmp(name, "min")   == 0) return AGG_MIN;
    if (pg_strcasecmp(name, "max")   == 0) return AGG_MAX;
    if (pg_strcasecmp(name, "avg")   == 0) return AGG_AVG;
    elog(ERROR, "Unknown aggregate: %s", name);
    return AGG_SUM;
}

static char *
resolve_target_name(ResTarget *rt)
{
    if (rt->name && strlen(rt->name) > 0)
        return pstrdup(rt->name);
    if (IsA(rt->val, ColumnRef))
    {
        ColumnRef *cr = (ColumnRef *) rt->val;
        if (list_length(cr->fields) > 0)
        {
            Node *field = (Node *) llast(cr->fields);
            if (IsA(field, String))
                return pstrdup(strVal(field));
        }
    }
    if (IsA(rt->val, FuncCall))
    {
        FuncCall *fc = (FuncCall *) rt->val;
        char *fname = strVal(linitial(fc->funcname));
        if (fc->agg_star)
            return psprintf("%s_*", fname);
        if (list_length(fc->args) > 0)
        {
            Node *arg = (Node *) linitial(fc->args);
            if (IsA(arg, ColumnRef))
            {
                ColumnRef *cr = (ColumnRef *) arg;
                char *cname = strVal(llast(cr->fields));
                return psprintf("%s_%s", fname, cname);
            }
        }
        return pstrdup(fname);
    }
    return pstrdup("expr");
}



static char *
get_column_ref_name(ColumnRef *cr)
{
    StringInfoData buf;
    ListCell *lc;
    int idx = 0;

    initStringInfo(&buf);
    foreach (lc, cr->fields)
    {
        Node *field = (Node *) lfirst(lc);
        if (IsA(field, String))
        {
            if (idx > 0) appendStringInfoChar(&buf, '.');
            appendStringInfoString(&buf, strVal(field));
            idx++;
        }
        else if (IsA(field, A_Star))
        {
            if (idx > 0) appendStringInfoChar(&buf, '.');
            appendStringInfoChar(&buf, '*');
            idx++;
        }
    }
    return buf.data;
}

static char *
extract_from_clause_sql(const char *sql)
{
    const char *p = sql;
    int paren_depth = 0;
    bool in_single_quote = false;
    bool in_double_quote = false;

    while (*p != '\0')
    {
        if (*p == '\'' && !in_double_quote)
            in_single_quote = !in_single_quote;
        else if (*p == '"' && !in_single_quote)
            in_double_quote = !in_double_quote;
        else if (!in_single_quote && !in_double_quote)
        {
            if (*p == '(')
                paren_depth++;
            else if (*p == ')')
                paren_depth--;
            else if (paren_depth == 0)
            {
                if (pg_strncasecmp(p, "FROM", 4) == 0)
                {
                    bool before_ok = (p == sql) || !is_ident_char(*(p - 1));
                    bool after_ok = !is_ident_char(*(p + 4));

                    if (before_ok && after_ok)
                    {
                        char *res = pstrdup(p);
                        int len = strlen(res);
                        while (len > 0 && (res[len - 1] == ';' || res[len - 1] == ' ' || res[len - 1] == '\t' || res[len - 1] == '\n' || res[len - 1] == '\r'))
                        {
                            res[len - 1] = '\0';
                            len--;
                        }
                        return res;
                    }
                }
            }
        }
        p++;
    }
    elog(ERROR, "Could not find FROM clause in SQL statement: %s", sql);
    return NULL;
}

PG_FUNCTION_INFO_V1(create_incremental_mv);

Datum
create_incremental_mv(PG_FUNCTION_ARGS)
{
    text *mv_name_text = PG_GETARG_TEXT_PP(0);
    char *mv_name = text_to_cstring(mv_name_text);
    char *agg_table_name = psprintf("imv_%s", mv_name);
    StringInfoData query;
    char *mv_def;
    List *tree;
    RawStmt *raw_stmt;
    SelectStmt *stmt;
    ListCell *lc;
    int n_group = 0, n_agg = 0;
    int total_cols;
    char **group_cols;
    char **group_types;
    AggDef *agg_defs;
    List *tables_list = NIL;
    StringInfoData tables_csv;
    char *from_clause_str;
    StringInfoData view_query_buf;
    StringInfoData group_csv;
    StringInfoData agg_csv;
    int i;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT schemaname, definition FROM pg_catalog.pg_matviews "
        "WHERE matviewname = '%s'", mv_name);
    SPI_execute(query.data, true, 1);
    if (SPI_processed == 0)
        elog(ERROR, "Materialized view \"%s\" not found", mv_name);

    (void) SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    mv_def = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2);

    tree = raw_parser(mv_def, RAW_PARSE_DEFAULT);
    if (list_length(tree) != 1)
        elog(ERROR, "MV definition must be a single SELECT");
    raw_stmt = (RawStmt *) linitial(tree);
    stmt = castNode(SelectStmt, raw_stmt->stmt);

    total_cols = list_length(stmt->targetList);
    group_cols  = palloc(total_cols * sizeof(char *));
    group_types = palloc(total_cols * sizeof(char *));
    agg_defs    = palloc(total_cols * sizeof(AggDef));

    foreach (lc, stmt->fromClause)
    {
        extract_base_tables((Node *) lfirst(lc), &tables_list);
    }
    if (list_length(tables_list) == 0)
        elog(ERROR, "No base tables found in FROM clause");

    initStringInfo(&tables_csv);
    {
        ListCell *tlc;
        int idx = 0;
        foreach (tlc, tables_list)
        {
            char *tname = (char *) lfirst(tlc);
            if (idx > 0) appendStringInfoChar(&tables_csv, ',');
            appendStringInfoString(&tables_csv, tname);
            idx++;
        }
    }

    from_clause_str = extract_from_clause_sql(mv_def);

    foreach (lc, stmt->targetList)
    {
        ResTarget *rt = (ResTarget *) lfirst(lc);
        char *target_name = resolve_target_name(rt);

        if (IsA(rt->val, FuncCall))
        {
            FuncCall *fc = (FuncCall *) rt->val;
            char *fname = strVal(linitial(fc->funcname));

            if (!is_agg_func_name(fname))
                elog(ERROR, "Non-aggregate function not supported: %s", fname);

            agg_defs[n_agg].func_type     = agg_func_from_name(fname);
            agg_defs[n_agg].target_column = target_name;

            if (fc->agg_star)
            {
                agg_defs[n_agg].source_column = pstrdup("*");
                agg_defs[n_agg].is_star       = true;
                agg_defs[n_agg].type_name     = pstrdup("bigint");
            }
            else if (list_length(fc->args) == 1)
            {
                Node *arg = (Node *) linitial(fc->args);
                if (!IsA(arg, ColumnRef))
                    elog(ERROR, "Only column references as aggregate args");
                {
                    ColumnRef *cr = (ColumnRef *) arg;
                    char *bare_col = strVal(llast(cr->fields));
                    agg_defs[n_agg].source_column = get_column_ref_name(cr);

                    if (agg_defs[n_agg].func_type == AGG_COUNT)
                        agg_defs[n_agg].type_name = pstrdup("bigint");
                    else if (agg_defs[n_agg].func_type == AGG_AVG)
                    {
                        char *col_type = get_column_type_name_multi(tables_list, bare_col);
                        if (pg_strcasecmp(col_type, "double precision") == 0 || pg_strcasecmp(col_type, "real") == 0)
                            agg_defs[n_agg].type_name = pstrdup("double precision");
                        else
                            agg_defs[n_agg].type_name = pstrdup("numeric");
                    }
                    else
                        agg_defs[n_agg].type_name = get_column_type_name_multi(tables_list, bare_col);
                }
                agg_defs[n_agg].is_star = false;
            }
            else
                elog(ERROR, "Aggregate with multiple arguments not supported");
            n_agg++;
        }
        else if (IsA(rt->val, ColumnRef))
        {
            ColumnRef *cr = (ColumnRef *) rt->val;
            char *bare_col = strVal(llast(cr->fields));
            group_cols[n_group]  = target_name;
            group_types[n_group] = get_column_type_name_multi(tables_list, bare_col);
            n_group++;
        }
        else
            elog(ERROR, "Only column refs and aggregates supported in SELECT");
    }

    if (n_group == 0)
        elog(ERROR, "At least one group column required");

    initStringInfo(&group_csv);
    for (i = 0; i < n_group; i++)
        appendStringInfo(&group_csv, "%s:%s%s", group_cols[i], group_types[i],
                        (i < n_group - 1) ? "," : "");

    initStringInfo(&agg_csv);
    for (i = 0; i < n_agg; i++)
    {
        char func_name[16];
        switch (agg_defs[i].func_type)
        {
            case AGG_SUM:   strcpy(func_name, "SUM"); break;
            case AGG_COUNT: strcpy(func_name, "COUNT"); break;
            case AGG_MIN:   strcpy(func_name, "MIN"); break;
            case AGG_MAX:   strcpy(func_name, "MAX"); break;
            case AGG_AVG:   strcpy(func_name, "AVG"); break;
        }
        appendStringInfo(&agg_csv, "%s:%s:%s:%s%s",
                        func_name,
                        agg_defs[i].source_column,
                        agg_defs[i].target_column,
                        agg_defs[i].type_name,
                        (i < n_agg - 1) ? "," : "");
    }

    /* Build view_query string */
    initStringInfo(&view_query_buf);
    appendStringInfoString(&view_query_buf, "SELECT ");
    for (i = 0; i < n_group; i++)
        appendStringInfo(&view_query_buf, "%s, ", group_cols[i]);
    for (i = 0; i < n_agg; i++)
    {
        AggDef *agg = &agg_defs[i];
        switch (agg->func_type)
        {
            case AGG_SUM:
                appendStringInfo(&view_query_buf, "SUM(%s) AS %s", agg->source_column, agg->target_column);
                break;
            case AGG_COUNT:
                appendStringInfo(&view_query_buf, "COUNT(%s) AS %s", agg->is_star ? "*" : agg->source_column, agg->target_column);
                break;
            case AGG_MIN:
                appendStringInfo(&view_query_buf, "MIN(%s) AS %s", agg->source_column, agg->target_column);
                break;
            case AGG_MAX:
                appendStringInfo(&view_query_buf, "MAX(%s) AS %s", agg->source_column, agg->target_column);
                break;
            case AGG_AVG:
                appendStringInfo(&view_query_buf, "AVG(%s) AS %s, SUM(%s) AS __sum_%s, COUNT(%s) AS __count_%s",
                                agg->source_column, agg->target_column,
                                agg->source_column, agg->target_column,
                                agg->source_column, agg->target_column);
                break;
        }
        if (i < n_agg - 1)
            appendStringInfoString(&view_query_buf, ", ");
    }
    appendStringInfo(&view_query_buf, " %s", from_clause_str);

    elog(NOTICE, "Creating agg table \"%s\" for MV \"%s\" (%d group cols, %d aggs, %d source tables)",
         agg_table_name, mv_name, n_group, n_agg, list_length(tables_list));

    resetStringInfo(&query);
    appendStringInfo(&query, "CREATE TABLE IF NOT EXISTS %s (", agg_table_name);
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s %s, ", group_cols[i], group_types[i]);
    for (i = 0; i < n_agg; i++)
    {
        if (agg_defs[i].func_type == AGG_AVG)
        {
            char *bare_col = (agg_defs[i].source_column && strcmp(agg_defs[i].source_column, "*") != 0) ? agg_defs[i].source_column : "id";
            char *sum_type;
            if (strrchr(bare_col, '.')) bare_col = strrchr(bare_col, '.') + 1;
            sum_type = get_column_type_name_multi(tables_list, bare_col);
            appendStringInfo(&query, "%s %s, __sum_%s %s, __count_%s bigint%s",
                            agg_defs[i].target_column, agg_defs[i].type_name,
                            agg_defs[i].target_column, sum_type,
                            agg_defs[i].target_column,
                            (i < n_agg - 1) ? ", " : "");
        }
        else
        {
            appendStringInfo(&query, "%s %s%s", agg_defs[i].target_column, agg_defs[i].type_name,
                            (i < n_agg - 1) ? ", " : "");
        }
    }
    appendStringInfoString(&query, ", PRIMARY KEY (");
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s%s", group_cols[i],
                        (i < n_group - 1) ? ", " : "");
    appendStringInfoString(&query, "))");
    SPI_execute(query.data, false, 0);

    /* Initial population */
    resetStringInfo(&query);
    appendStringInfo(&query, "INSERT INTO %s (", agg_table_name);
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s, ", group_cols[i]);
    for (i = 0; i < n_agg; i++)
    {
        if (agg_defs[i].func_type == AGG_AVG)
        {
            appendStringInfo(&query, "%s, __sum_%s, __count_%s%s",
                            agg_defs[i].target_column,
                            agg_defs[i].target_column,
                            agg_defs[i].target_column,
                            (i < n_agg - 1) ? ", " : "");
        }
        else
        {
            appendStringInfo(&query, "%s%s", agg_defs[i].target_column,
                            (i < n_agg - 1) ? ", " : "");
        }
    }
    appendStringInfo(&query, ") %s ON CONFLICT DO NOTHING", view_query_buf.data);
    SPI_execute(query.data, false, 0);

    /* Store metadata into mini_ivm_catalog */
    resetStringInfo(&query);
    appendStringInfoString(&query,
        "CREATE TABLE IF NOT EXISTS mini_ivm_catalog ("
        "    mv_name TEXT PRIMARY KEY,"
        "    agg_table TEXT NOT NULL,"
        "    source_table TEXT NOT NULL,"
        "    group_cols TEXT NOT NULL,"
        "    aggs_def TEXT NOT NULL,"
        "    view_query TEXT"
        ")");
    SPI_execute(query.data, false, 0);

    resetStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM mini_ivm_catalog WHERE mv_name = '%s' OR agg_table = '%s'",
        mv_name, agg_table_name);
    SPI_execute(query.data, false, 0);

    resetStringInfo(&query);
    appendStringInfo(&query,
        "INSERT INTO mini_ivm_catalog (mv_name, agg_table, source_table, group_cols, aggs_def, view_query) "
        "VALUES ('%s', '%s', '%s', '%s', '%s', %s)",
        mv_name, agg_table_name, tables_csv.data, group_csv.data, agg_csv.data,
        quote_literal_cstr(view_query_buf.data));
    SPI_execute(query.data, false, 0);

    /* Register triggers on ALL base tables */
    {
        ListCell *tlc;
        foreach (tlc, tables_list)
        {
            char *tbl = (char *) lfirst(tlc);

            resetStringInfo(&query);
            appendStringInfo(&query, "DROP TRIGGER IF EXISTS mini_ivm_trig_ins_%s ON \"%s\"",
                            agg_table_name, tbl);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query, "DROP TRIGGER IF EXISTS mini_ivm_trig_del_%s ON \"%s\"",
                            agg_table_name, tbl);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query, "DROP TRIGGER IF EXISTS mini_ivm_trig_upd_%s ON \"%s\"",
                            agg_table_name, tbl);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query,
                "CREATE TRIGGER mini_ivm_trig_ins_%s "
                "AFTER INSERT ON \"%s\" "
                "REFERENCING NEW TABLE AS new_table "
                "FOR EACH STATEMENT EXECUTE FUNCTION mini_ivm_maintain('%s')",
                agg_table_name, tbl, agg_table_name);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query,
                "CREATE TRIGGER mini_ivm_trig_del_%s "
                "AFTER DELETE ON \"%s\" "
                "REFERENCING OLD TABLE AS old_table "
                "FOR EACH STATEMENT EXECUTE FUNCTION mini_ivm_maintain('%s')",
                agg_table_name, tbl, agg_table_name);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query,
                "CREATE TRIGGER mini_ivm_trig_upd_%s "
                "AFTER UPDATE ON \"%s\" "
                "REFERENCING OLD TABLE AS old_table NEW TABLE AS new_table "
                "FOR EACH STATEMENT EXECUTE FUNCTION mini_ivm_maintain('%s')",
                agg_table_name, tbl, agg_table_name);
            SPI_execute(query.data, false, 0);
        }
    }

    elog(NOTICE, "Incremental MV \"%s\" created (agg table: %s)", mv_name, agg_table_name);

    SPI_finish();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(drop_incremental_mv);

Datum
drop_incremental_mv(PG_FUNCTION_ARGS)
{
    text *mv_name_text = PG_GETARG_TEXT_PP(0);
    char *mv_name = text_to_cstring(mv_name_text);
    char *agg_table_name = psprintf("imv_%s", mv_name);
    StringInfoData query;
    char *src_tables_str = NULL;

    SPI_connect();

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT source_table FROM mini_ivm_catalog "
        "WHERE mv_name = '%s' OR agg_table = '%s'",
        mv_name, agg_table_name);
    if (SPI_execute(query.data, true, 1) == SPI_OK_SELECT && SPI_processed > 0)
    {
        char *val = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        if (val)
            src_tables_str = pstrdup(val);
    }
    SPI_freetuptable(SPI_tuptable);

    if (src_tables_str)
    {
        int n_src = 0;
        char **src_tables = split_csv(src_tables_str, &n_src);
        for (int i = 0; i < n_src; i++)
        {
            char *tbl = src_tables[i];

            resetStringInfo(&query);
            appendStringInfo(&query,
                "DROP TRIGGER IF EXISTS mini_ivm_trig_ins_%s ON \"%s\"",
                agg_table_name, tbl);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query,
                "DROP TRIGGER IF EXISTS mini_ivm_trig_del_%s ON \"%s\"",
                agg_table_name, tbl);
            SPI_execute(query.data, false, 0);

            resetStringInfo(&query);
            appendStringInfo(&query,
                "DROP TRIGGER IF EXISTS mini_ivm_trig_upd_%s ON \"%s\"",
                agg_table_name, tbl);
            SPI_execute(query.data, false, 0);
        }
    }

    resetStringInfo(&query);
    appendStringInfo(&query, "DROP TABLE IF EXISTS %s", agg_table_name);
    SPI_execute(query.data, false, 0);

    resetStringInfo(&query);
    appendStringInfo(&query,
        "DELETE FROM mini_ivm_catalog WHERE mv_name = '%s' OR agg_table = '%s'",
        mv_name, agg_table_name);
    SPI_execute(query.data, false, 0);

    elog(NOTICE, "Incremental MV \"%s\" dropped", mv_name);
    SPI_finish();
    PG_RETURN_VOID();
}
