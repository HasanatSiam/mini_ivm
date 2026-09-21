/*
 * mini_ivm_ddl.c
 *
 * DDL management for incremental materialized views:
 *   - AST parsing helpers (aggregate detection, column ref resolution)
 *   - create_incremental_mv() – parse MV definition, create agg table, register triggers
 *   - drop_incremental_mv()   – drop triggers, agg table, and catalog entry
 */

#include "mini_ivm.h"

/* ----------------------------------------------------------------
 * AST parsing helpers
 * ---------------------------------------------------------------- */

/*
 * is_agg_func_name - returns true if name is a supported aggregate function.
 */
static bool
is_agg_func_name(const char *name)
{
    return pg_strcasecmp(name, "sum") == 0 ||
           pg_strcasecmp(name, "count") == 0 ||
           pg_strcasecmp(name, "min")  == 0 ||
           pg_strcasecmp(name, "max")  == 0 ||
           pg_strcasecmp(name, "avg")  == 0;
}

/*
 * agg_func_from_name - maps an aggregate function name to AggFuncType.
 */
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

/*
 * resolve_target_name
 *
 * Determines the output column name for a ResTarget in a SELECT list.
 * Uses the alias if present, otherwise falls back to column/function name.
 */
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

/*
 * get_column_ref_name
 *
 * Serialises a ColumnRef (possibly qualified) back to a dotted name string,
 * e.g. "orders.amount" or just "amount".
 */
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

/*
 * extract_from_clause_sql
 *
 * Scans the raw SQL string and returns the substring starting at the first
 * top-level FROM keyword (i.e., not inside parens or quotes), with trailing
 * whitespace/semicolons stripped.
 */
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

/* ----------------------------------------------------------------
 * DDL entry points
 * ---------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(create_incremental_mv);

/*
 * create_incremental_mv
 *
 * SQL-callable function: given an existing materialized view name, parses
 * its definition, creates a backing aggregate table, populates it, stores
 * metadata in mini_ivm_catalog, and registers statement-level triggers on
 * all referenced base tables.
 *
 * Usage: SELECT create_incremental_mv('my_mv');
 */
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

/*
 * drop_incremental_mv
 *
 * SQL-callable function: drops the triggers, aggregate table, and catalog
 * entry for a previously created incremental materialized view.
 *
 * Usage: SELECT drop_incremental_mv('my_mv');
 */
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
