/*
 * mini_ivm_hook.c
 *
 * PostgreSQL extension entry points and ProcessUtility hook:
 *   - PG_MODULE_MAGIC         – required once per shared library
 *   - mini_ivm_ProcessUtility – intercepts IMMV DDL, chains to prev/standard
 *   - _PG_init()              – installs the hook on extension load
 *   - _PG_fini()              – restores the previous hook on unload
 *
 * Intercepted syntax
 * ------------------
 *   CREATE INCREMENTAL MATERIALIZED VIEW <name> AS <query>;
 *     We detect this by scanning the raw queryString for the token sequence
 *     CREATE INCREMENTAL MATERIALIZED VIEW before the AST is dispatched.
 *     When matched we:
 *       1. Rewrite the query to a plain CREATE MATERIALIZED VIEW and let
 *          standard_ProcessUtility build the base matview.
 *       2. Extract the view name and call create_incremental_mv() via SPI
 *          to build the agg table, populate it, and install the triggers.
 *
 *   DROP INCREMENTAL MATERIALIZED VIEW <name>;
 *     When matched we:
 *       1. Call drop_incremental_mv() via SPI to remove triggers, agg table,
 *          and catalog row.
 *       2. Issue a plain DROP MATERIALIZED VIEW to remove the underlying
 *          PostgreSQL matview.
 *
 * All other utility statements are passed through transparently.
 */

#include "mini_ivm.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "tcop/tcopprot.h"

PG_MODULE_MAGIC;

/* Saved hook pointer so we can chain correctly. */
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

/*
 * skip_whitespace
 *
 * Advances *p past any ASCII whitespace characters.
 */
static void
skip_whitespace(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
        (*p)++;
}

/*
 * match_keyword
 *
 * Case-insensitive comparison of the next token at *p against keyword.
 * Returns true and advances *p past the keyword (and trailing whitespace)
 * if matched; the character immediately after the keyword must not be an
 * identifier character (word boundary check).
 */
static bool
match_keyword(const char **p, const char *keyword)
{
    size_t klen = strlen(keyword);

    if (pg_strncasecmp(*p, keyword, klen) == 0 && !is_ident_char((*p)[klen]))
    {
        *p += klen;
        skip_whitespace(p);
        return true;
    }
    return false;
}

/*
 * extract_identifier
 *
 * Reads an unquoted SQL identifier (or a double-quoted one) from *p,
 * advances *p past it (and trailing whitespace), and returns a palloc'd
 * copy of the identifier text (without quotes for the quoted form).
 * Returns NULL if no identifier is found.
 */
static char *
extract_identifier(const char **p)
{
    const char *start = *p;
    char       *result;

    if (**p == '"')
    {
        /* Quoted identifier */
        (*p)++;
        start = *p;
        while (**p != '\0' && **p != '"')
            (*p)++;
        if (**p == '\0')
            return NULL;
        result = pnstrdup(start, *p - start);
        (*p)++;                     /* skip closing quote */
    }
    else if (is_ident_char(**p))
    {
        while (is_ident_char(**p))
            (*p)++;
        result = pnstrdup(start, *p - start);
    }
    else
    {
        return NULL;
    }

    skip_whitespace(p);
    return result;
}

/*
 * detect_create_immv
 *
 * Scans queryString for the pattern:
 *   CREATE [OR REPLACE] INCREMENTAL MATERIALIZED VIEW [IF NOT EXISTS] <name>
 *
 * If matched, *mv_name_out is set to a palloc'd copy of the view name and
 * true is returned.
 */
static bool
detect_create_immv(const char *queryString, char **mv_name_out)
{
    const char *p = queryString;

    skip_whitespace(&p);
    if (!match_keyword(&p, "CREATE"))
        return false;

    /* Optional OR REPLACE */
    if (pg_strncasecmp(p, "OR", 2) == 0 && !is_ident_char(p[2]))
    {
        p += 2;
        skip_whitespace(&p);
        if (!match_keyword(&p, "REPLACE"))
            return false;
    }

    if (!match_keyword(&p, "MATERIALIZED"))
        return false;
    if (!match_keyword(&p, "VIEW"))
        return false;

    /* Optional IF NOT EXISTS */
    if (pg_strncasecmp(p, "IF", 2) == 0 && !is_ident_char(p[2]))
    {
        const char *save = p;
        p += 2;
        skip_whitespace(&p);
        if (match_keyword(&p, "NOT") && match_keyword(&p, "EXISTS"))
            ; /* consumed */
        else
            p = save;
    }

    *mv_name_out = extract_identifier(&p);
    return (*mv_name_out != NULL);
}

/*
 * detect_drop_immv
 *
 * Scans queryString for:
 *   DROP INCREMENTAL MATERIALIZED VIEW [IF EXISTS] <name>
 *
 * Returns true and sets *mv_name_out when matched.
 */
static bool
detect_drop_immv(const char *queryString, char **mv_name_out)
{
    const char *p = queryString;

    skip_whitespace(&p);
    if (!match_keyword(&p, "DROP"))
        return false;
    if (!match_keyword(&p, "MATERIALIZED"))
        return false;
    if (!match_keyword(&p, "VIEW"))
        return false;

    /* Optional IF EXISTS */
    if (pg_strncasecmp(p, "IF", 2) == 0 && !is_ident_char(p[2]))
    {
        const char *save = p;
        p += 2;
        skip_whitespace(&p);
        if (match_keyword(&p, "EXISTS"))
            ; /* consumed */
        else
            p = save;
    }

    *mv_name_out = extract_identifier(&p);
    return (*mv_name_out != NULL);
}

/*
 * build_plain_create_mv_query
 *
 * Rewrites a CREATE INCREMENTAL MATERIALIZED VIEW query into a plain
 * CREATE MATERIALIZED VIEW query by blanking out the first occurrence of
 * the word INCREMENTAL (case-insensitive, word-bounded) with spaces.
 * Returns a palloc'd copy of the rewritten string.
 */
static char *
build_plain_create_mv_query(const char *queryString)
{
    return pstrdup(queryString);
}

/*
 * run_spi_command
 *
 * Executes a single SQL command via SPI inside an existing SPI connection.
 * Raises an ERROR on failure.
 */
static void
run_spi_command(const char *sql)
{
    int rc = SPI_execute(sql, false, 0);

    if (rc < 0)
        elog(ERROR, "mini_ivm: SPI_execute failed (rc=%d) for: %s", rc, sql);
}

/* ----------------------------------------------------------------
 * ProcessUtility hook
 * ---------------------------------------------------------------- */

/*
 * mini_ivm_ProcessUtility
 *
 * Intercepts CREATE INCREMENTAL MATERIALIZED VIEW and DROP INCREMENTAL
 * MATERIALIZED VIEW DDL.  All other statements are passed through
 * transparently to the previous hook or standard handler.
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
    char *mv_name = NULL;

    /* ---- CREATE INCREMENTAL MATERIALIZED VIEW ---- */
    if (detect_create_immv(queryString, &mv_name))
    {
        char *plain_query;
        char *spi_cmd;

        elog(NOTICE,
             "mini_ivm: intercepted CREATE INCREMENTAL MATERIALIZED VIEW \"%s\"",
             mv_name);

        /*
         * Step 1 – Create the underlying materialized view using the
         * standard utility path with INCREMENTAL stripped from the query.
         */
        plain_query = build_plain_create_mv_query(queryString);

        if (prev_ProcessUtility)
            prev_ProcessUtility(pstmt, plain_query, read_only_tree,
                                context, params, queryEnv, dest, qc);
        else
            standard_ProcessUtility(pstmt, plain_query, read_only_tree,
                                    context, params, queryEnv, dest, qc);

        pfree(plain_query);

        /*
         * Step 2 – Register the incremental maintenance infrastructure
         * (agg table, triggers, catalog row) via SPI.
         */
        SPI_connect();
        spi_cmd = psprintf("SELECT create_incremental_mv(%s)",
                           quote_literal_cstr(mv_name));
        run_spi_command(spi_cmd);
        pfree(spi_cmd);
        SPI_finish();

        pfree(mv_name);
        return;
    }

    /* ---- DROP INCREMENTAL MATERIALIZED VIEW ---- */
    if (detect_drop_immv(queryString, &mv_name))
    {
        char        *spi_cmd;
        char        *drop_query;
        List        *parsetree_list;

        elog(NOTICE,
             "mini_ivm: intercepted DROP INCREMENTAL MATERIALIZED VIEW \"%s\"",
             mv_name);

        /*
         * Step 1 – Tear down the mini_ivm infrastructure first (triggers,
         * agg table, catalog row).
         */
        SPI_connect();
        spi_cmd = psprintf("SELECT drop_incremental_mv(%s)",
                           quote_literal_cstr(mv_name));
        run_spi_command(spi_cmd);
        pfree(spi_cmd);
        SPI_finish();

        /*
         * Step 2 – Drop the underlying PostgreSQL materialized view by
         * issuing a plain DROP MATERIALIZED VIEW through the standard path.
         */
        drop_query = psprintf("DROP MATERIALIZED VIEW IF EXISTS %s",
                              quote_identifier(mv_name));

        parsetree_list = pg_parse_query(drop_query);
        if (list_length(parsetree_list) == 1)
        {
            RawStmt    *raw       = (RawStmt *) linitial(parsetree_list);
            PlannedStmt *drop_pstmt = makeNode(PlannedStmt);

            drop_pstmt->commandType   = CMD_UTILITY;
            drop_pstmt->canSetTag     = true;
            drop_pstmt->utilityStmt   = raw->stmt;
            drop_pstmt->stmt_location = raw->stmt_location;
            drop_pstmt->stmt_len      = raw->stmt_len;

            if (prev_ProcessUtility)
                prev_ProcessUtility(drop_pstmt, drop_query, read_only_tree,
                                    context, params, queryEnv, dest, qc);
            else
                standard_ProcessUtility(drop_pstmt, drop_query, read_only_tree,
                                        context, params, queryEnv, dest, qc);
        }

        pfree(drop_query);
        pfree(mv_name);
        return;
    }

    /* ---- All other statements: transparent passthrough ---- */
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
