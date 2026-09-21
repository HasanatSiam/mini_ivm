/*
 * mini_ivm_utils.c
 *
 * Low-level utility helpers shared across mini_ivm modules:
 *   - Datum comparison
 *   - String/identifier helpers
 *   - SPI tuple value extraction
 *   - Column type lookup
 *   - CSV splitting
 *   - Base table extraction from AST
 */

#include "mini_ivm.h"

/*
 * datum_equal - compare two Datums of a given type, NULL-aware.
 */
bool
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

/*
 * is_ident_char - returns true if c is a valid SQL identifier character.
 */
bool
is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/*
 * replace_table_name_in_query
 *
 * Returns a new string with whole-word occurrences of old_table replaced by
 * new_table (case-insensitive, word-boundary aware).
 */
char *
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

/*
 * get_text_value - fetch a column value from a SPI tuple as a C string.
 * Returns NULL if the column is null.
 */
char *
get_text_value(HeapTuple tuple, TupleDesc tupdesc, int attnum)
{
    bool isnull;
    (void) SPI_getbinval(tuple, tupdesc, attnum, &isnull);
    if (isnull)
        return NULL;
    return SPI_getvalue(tuple, tupdesc, attnum);
}

/*
 * get_column_type_name_multi
 *
 * Looks up the type name of `column` by searching each table in `tables`
 * (a List of C strings). Returns the first match via SPI.
 */
char *
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

/*
 * extract_base_tables
 *
 * Recursively walks a FROM-clause node and collects unique base table names
 * into *tables (a List of pstrdup'd C strings).
 */
void
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

/*
 * split_csv
 *
 * Splits a comma-separated string into an array of palloc'd strings.
 * *out_count is set to the number of tokens returned.
 */
char **
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
