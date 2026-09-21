/*
 * mini_ivm_maintain.c
 *
 * Core incremental view maintenance (IVM) delta logic:
 *
 *   Row-level handlers (FOR EACH ROW triggers):
 *     - build_group_where()
 *     - build_group_values_list()
 *     - build_agg_values_list()
 *     - apply_insert()
 *     - apply_delete()
 *     - apply_update()
 *
 *   Statement-level handlers (FOR EACH STATEMENT + transition tables):
 *     - apply_statement_insert_delta()
 *     - apply_statement_delete_delta()
 *     - apply_statement_update_delta()
 *
 *   Trigger entry point:
 *     - mini_ivm_maintain()
 */

#include "mini_ivm.h"

/* ----------------------------------------------------------------
 * Internal query builder helpers
 * ---------------------------------------------------------------- */

/*
 * build_group_where
 *
 * Appends a WHERE clause fragment matching the group-by columns of `tuple`
 * to `buf`.  Used to locate the matching row in the aggregate table.
 */
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

/*
 * build_group_values_list
 *
 * Appends the VALUES list for group-by columns, e.g. CAST('val' AS type), ...
 */
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

/*
 * build_agg_values_list
 *
 * Appends the VALUES list for aggregate columns from a single source tuple.
 * AVG expands to three sub-columns: avg_val, __sum_col, __count_col.
 */
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

/* ----------------------------------------------------------------
 * Row-level delta handlers  (FOR EACH ROW)
 * ---------------------------------------------------------------- */

/*
 * apply_insert
 *
 * Handles a single inserted row: upserts the aggregate table using
 * INSERT ... ON CONFLICT ... DO UPDATE.
 */
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

/*
 * apply_delete
 *
 * Handles a single deleted row: decrements aggregates and removes rows
 * from the aggregate table when the source row count drops to zero.
 */
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

/*
 * apply_update
 *
 * Handles a single updated row.  If the group-by key changed, delegates to
 * apply_delete (old) + apply_insert (new).  Otherwise applies incremental
 * aggregate adjustments in-place.
 */
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

/* ----------------------------------------------------------------
 * Statement-level delta handlers  (FOR EACH STATEMENT + transition tables)
 * ---------------------------------------------------------------- */

/*
 * apply_statement_insert_delta
 *
 * Processes a batch INSERT using the `new_table` transition relation.
 * Runs the view query against new_table to compute the delta, then upserts.
 */
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

/*
 * apply_statement_delete_delta
 *
 * Processes a batch DELETE using the `old_table` transition relation.
 * Computes aggregate delta from old_table and subtracts from aggregate table.
 */
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

/*
 * apply_statement_update_delta
 *
 * Handles a batch UPDATE by applying a delete delta then an insert delta.
 */
static void
apply_statement_update_delta(MvConfig *cfg, const char *triggered_table)
{
    apply_statement_delete_delta(cfg, triggered_table);
    apply_statement_insert_delta(cfg, triggered_table);
}

/* ----------------------------------------------------------------
 * Trigger entry point
 * ---------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(mini_ivm_maintain);

/*
 * mini_ivm_maintain
 *
 * The trigger function called by PostgreSQL on INSERT/UPDATE/DELETE.
 * Dispatches to either statement-level or row-level handlers based on
 * how the trigger was fired.
 */
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
