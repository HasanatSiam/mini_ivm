/*
 * mini_ivm_config.c
 *
 * Parsing and loading of MvConfig structures:
 *   - parse_trigger_args()       – parse legacy 4-arg trigger style
 *   - load_mv_config_from_catalog() – load from mini_ivm_catalog table
 *   - free_mv_config()           – release an MvConfig
 */

#include "mini_ivm.h"

/*
 * parse_trigger_args
 *
 * Parses a 4-argument trigger arg string into a MvConfig.
 * Legacy format used before catalog-based metadata storage.
 *
 * args[0] = agg_table
 * args[1] = source_table
 * args[2] = group cols:  "col1:type1,col2:type2,..."
 * args[3] = agg defs:    "FUNC:src:alias:type,..."
 */
MvConfig *
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

/*
 * free_mv_config
 *
 * Currently a no-op (palloc memory is freed with the memory context).
 * Kept as a hook for future explicit cleanup.
 */
void
free_mv_config(MvConfig *cfg)
{
    (void) cfg;
}

/*
 * load_mv_config_from_catalog
 *
 * Reads the mini_ivm_catalog table for the given agg_table_name and
 * reconstructs a full MvConfig from the stored metadata.
 */
MvConfig *
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
