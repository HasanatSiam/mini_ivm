#include "postgres.h"
#include "fmgr.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "parser/parser.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "nodes/nodes.h"

PG_MODULE_MAGIC;

typedef enum { AGG_SUM, AGG_COUNT, AGG_MIN, AGG_MAX } AggFuncType;

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
    int     n_group_cols;
    char  **group_cols;
    char  **group_type_names;
    int     n_aggs;
    AggDef *aggs;
} MvConfig;

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
        else
            elog(ERROR, "Unknown aggregate: %s", func_s);

        cfg->aggs[i].source_column = pstrdup(col_s);
        cfg->aggs[i].target_column = pstrdup(alias_s);
        cfg->aggs[i].type_name     = pstrdup(type_s);
        cfg->aggs[i].is_star       = (strcmp(col_s, "*") == 0);
        i++;
        tok = strtok(NULL, ",");
    }

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
        appendStringInfo(&query, "%s%s", cfg->aggs[i].target_column,
                        (i == cfg->n_aggs - 1) ? "" : ", ");
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
        appendStringInfo(&query, "%s = ", agg->target_column);

        switch (agg->func_type)
        {
            case AGG_SUM:
                appendStringInfo(&query, "%s.%s + EXCLUDED.%s",
                                cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_COUNT:
                appendStringInfo(&query, "%s.%s + EXCLUDED.%s",
                                cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MIN:
                appendStringInfo(&query, "LEAST(%s.%s, EXCLUDED.%s)",
                                cfg->agg_table, agg->target_column, agg->target_column);
                break;
            case AGG_MAX:
                appendStringInfo(&query, "GREATEST(%s.%s, EXCLUDED.%s)",
                                cfg->agg_table, agg->target_column, agg->target_column);
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

    initStringInfo(&where);
    build_group_where(&where, tuple, tupdesc, cfg);

    initStringInfo(&set_clause);
    for (i = 0; i < cfg->n_aggs; i++)
    {
        AggDef *agg = &cfg->aggs[i];
        if (i > 0) appendStringInfoString(&set_clause, ", ");
        appendStringInfo(&set_clause, "%s = ", agg->target_column);

        switch (agg->func_type)
        {
            case AGG_SUM:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *val = get_text_value(tuple, tupdesc, attnum);
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
                appendStringInfo(&set_clause, "%s.%s - 1",
                                cfg->agg_table, agg->target_column);
                break;
            case AGG_MIN:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *val = get_text_value(tuple, tupdesc, attnum);
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
            break;
        }
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
        char *old_val, *new_val;

        if (attnum == SPI_ERROR_NOATTRIBUTE)
            elog(ERROR, "Column \"%s\" not found", cfg->group_cols[i]);
        old_val = get_text_value(oldtuple, tupdesc, attnum);
        new_val = get_text_value(newtuple, tupdesc, attnum);
        if ((old_val == NULL && new_val != NULL) ||
            (old_val != NULL && new_val == NULL) ||
            (old_val != NULL && new_val != NULL && strcmp(old_val, new_val) != 0))
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
        char *old_val, *new_val;

        if (agg->func_type == AGG_COUNT)
            continue;
        attnum = SPI_fnumber(tupdesc, agg->source_column);
        if (attnum == SPI_ERROR_NOATTRIBUTE)
            continue;
        old_val = get_text_value(oldtuple, tupdesc, attnum);
        new_val = get_text_value(newtuple, tupdesc, attnum);
        if ((old_val == NULL && new_val != NULL) ||
            (old_val != NULL && new_val == NULL) ||
            (old_val != NULL && new_val != NULL && strcmp(old_val, new_val) != 0))
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
        appendStringInfo(&set_clause, "%s = ", agg->target_column);

        switch (agg->func_type)
        {
            case AGG_SUM:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *old_val = get_text_value(oldtuple, tupdesc, attnum);
                char *new_val = get_text_value(newtuple, tupdesc, attnum);
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
                appendStringInfo(&set_clause, "%s.%s", cfg->agg_table, agg->target_column);
                break;
            case AGG_MIN:
            {
                int attnum = SPI_fnumber(tupdesc, agg->source_column);
                char *old_val = get_text_value(oldtuple, tupdesc, attnum);
                char *new_val = get_text_value(newtuple, tupdesc, attnum);
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
        }
    }

    initStringInfo(&query);
    appendStringInfo(&query, "UPDATE %s SET %s WHERE %s",
                    cfg->agg_table, set_clause.data, where.data);
    SPI_execute(query.data, false, 0);
}

PG_FUNCTION_INFO_V1(mini_ivm_maintain);

Datum
mini_ivm_maintain(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata;
    TupleDesc tupdesc;
    Trigger *trigger;
    MvConfig *cfg;

    if (!CALLED_AS_TRIGGER(fcinfo))
        elog(ERROR, "mini_ivm_maintain must be called as a trigger");

    trigdata = (TriggerData *) fcinfo->context;
    tupdesc = trigdata->tg_relation->rd_att;
    trigger = trigdata->tg_trigger;

    cfg = parse_trigger_args(trigger->tgnargs, trigger->tgargs);

    SPI_connect();

    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
        apply_insert(trigdata->tg_trigtuple, tupdesc, cfg);
    else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
        apply_delete(trigdata->tg_trigtuple, tupdesc, cfg);
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
        apply_update(trigdata->tg_trigtuple, trigdata->tg_newtuple, tupdesc, cfg);

    SPI_finish();

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
           pg_strcasecmp(name, "max")  == 0;
}

static AggFuncType
agg_func_from_name(const char *name)
{
    if (pg_strcasecmp(name, "sum")   == 0) return AGG_SUM;
    if (pg_strcasecmp(name, "count") == 0) return AGG_COUNT;
    if (pg_strcasecmp(name, "min")   == 0) return AGG_MIN;
    if (pg_strcasecmp(name, "max")   == 0) return AGG_MAX;
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
get_column_type_name(const char *table, const char *column)
{
    StringInfoData query;
    char *result;

    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT a.atttypid::regtype::text "
        "FROM pg_catalog.pg_attribute a "
        "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
        "WHERE c.relname = '%s' AND a.attname = '%s' AND a.attnum > 0",
        table, column);

    SPI_execute(query.data, true, 1);
    if (SPI_processed == 0)
        elog(ERROR, "Column \"%s\" not found in table \"%s\"", column, table);

    result = pstrdup(SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
    return result;
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
    char *source_table = NULL;
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
        Node *from_node = (Node *) lfirst(lc);
        if (IsA(from_node, RangeVar))
        {
            RangeVar *rv = (RangeVar *) from_node;
            if (source_table != NULL)
                elog(ERROR, "Multiple base tables not supported (found '%s' and '%s')",
                     source_table, rv->relname);
            source_table = pstrdup(rv->relname);
        }
        else
            elog(ERROR, "Only simple table references supported in FROM clause");
    }

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

            agg_defs[n_agg].func_type       = agg_func_from_name(fname);
            agg_defs[n_agg].target_column   = target_name;

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
                    agg_defs[n_agg].source_column = pstrdup(strVal(llast(cr->fields)));
                }

                if (agg_defs[n_agg].func_type == AGG_COUNT)
                    agg_defs[n_agg].type_name = pstrdup("bigint");
                else
                    agg_defs[n_agg].type_name = get_column_type_name(source_table, agg_defs[n_agg].source_column);
                agg_defs[n_agg].is_star = false;
            }
            else
                elog(ERROR, "Aggregate with multiple arguments not supported");
            n_agg++;
        }
        else if (IsA(rt->val, ColumnRef))
        {
            group_cols[n_group]   = target_name;
            group_types[n_group]  = get_column_type_name(source_table, target_name);
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
        }
        appendStringInfo(&agg_csv, "%s:%s:%s:%s%s",
                        func_name,
                        agg_defs[i].source_column,
                        agg_defs[i].target_column,
                        agg_defs[i].type_name,
                        (i < n_agg - 1) ? "," : "");
    }

    elog(NOTICE, "Creating agg table \"%s\" for MV \"%s\" (%d group cols, %d aggs)",
         agg_table_name, mv_name, n_group, n_agg);

    resetStringInfo(&query);
    appendStringInfo(&query, "CREATE TABLE IF NOT EXISTS %s (", agg_table_name);
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s %s, ", group_cols[i], group_types[i]);
    for (i = 0; i < n_agg; i++)
        appendStringInfo(&query, "%s %s%s", agg_defs[i].target_column, agg_defs[i].type_name,
                        (i < n_agg - 1) ? ", " : "");
    appendStringInfoString(&query, ", PRIMARY KEY (");
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s%s", group_cols[i],
                        (i < n_group - 1) ? ", " : "");
    appendStringInfoString(&query, "))");
    SPI_execute(query.data, false, 0);

    resetStringInfo(&query);
    appendStringInfo(&query, "INSERT INTO %s (", agg_table_name);
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s, ", group_cols[i]);
    for (i = 0; i < n_agg; i++)
        appendStringInfo(&query, "%s%s", agg_defs[i].target_column,
                        (i < n_agg - 1) ? ", " : "");
    appendStringInfoString(&query, ") SELECT ");
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s, ", group_cols[i]);
    for (i = 0; i < n_agg; i++)
    {
        AggDef *agg = &agg_defs[i];
        switch (agg->func_type)
        {
            case AGG_SUM:   appendStringInfo(&query, "SUM(%s)", agg->source_column); break;
            case AGG_COUNT: appendStringInfo(&query, "COUNT(%s)", agg->is_star ? "*" : agg->source_column); break;
            case AGG_MIN:   appendStringInfo(&query, "MIN(%s)", agg->source_column); break;
            case AGG_MAX:   appendStringInfo(&query, "MAX(%s)", agg->source_column); break;
        }
        appendStringInfoString(&query, (i < n_agg - 1) ? ", " : "");
    }
    appendStringInfo(&query, " FROM %s GROUP BY ", source_table);
    for (i = 0; i < n_group; i++)
        appendStringInfo(&query, "%s%s", group_cols[i],
                        (i < n_group - 1) ? ", " : "");
    appendStringInfoString(&query, " ON CONFLICT DO NOTHING");
    SPI_execute(query.data, false, 0);

    resetStringInfo(&query);
    appendStringInfo(&query, "DROP TRIGGER IF EXISTS mini_ivm_trigger_%s ON \"%s\"",
                    agg_table_name, source_table);
    SPI_execute(query.data, false, 0);

    resetStringInfo(&query);
    appendStringInfo(&query,
        "CREATE TRIGGER mini_ivm_trigger_%s "
        "AFTER INSERT OR UPDATE OR DELETE ON \"%s\" "
        "FOR EACH ROW EXECUTE FUNCTION mini_ivm_maintain("
        "'%s', '%s', '%s', '%s')",
        agg_table_name, source_table,
        agg_table_name, source_table, group_csv.data, agg_csv.data);
    SPI_execute(query.data, false, 0);

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
    char *mv_schema = NULL;

    SPI_connect();

    /* 1. Create dynamic IMMV table */
    initStringInfo(&query);
    appendStringInfo(&query,
        "SELECT schemaname FROM pg_catalog.pg_matviews "
        "WHERE matviewname = '%s'", mv_name);
    SPI_execute(query.data, true, 1);
    if (SPI_processed > 0)
        mv_schema = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

    if (mv_schema)
    {
        resetStringInfo(&query);
        appendStringInfo(&query,
            "DROP TRIGGER IF EXISTS mini_ivm_trigger_%s ON \"%s\".\"%s\"",
            agg_table_name, mv_schema, mv_name);
        SPI_execute(query.data, false, 0);
    }

    resetStringInfo(&query);
    appendStringInfo(&query, "DROP TABLE IF EXISTS %s", agg_table_name);
    SPI_execute(query.data, false, 0);

    elog(NOTICE, "Incremental MV \"%s\" dropped", mv_name);
    SPI_finish();
    PG_RETURN_VOID();
}
