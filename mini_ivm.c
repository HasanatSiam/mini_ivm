#include "postgres.h"
#include "fmgr.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

/* ================================
 * Extract column by name
 * ================================ */
static char *
get_column_value(HeapTuple tuple, TupleDesc tupdesc, const char *colname)
{
    bool isnull;
    int attnum = SPI_fnumber(tupdesc, colname);
    Datum val;

    if (attnum == SPI_ERROR_NOATTRIBUTE)
        elog(ERROR, "Column \"%s\" not found", colname);

    val = SPI_getbinval(tuple, tupdesc, attnum, &isnull);

    if (isnull)
        return NULL;

    return TextDatumGetCString(val);
}

/* ================================
 * Apply Delta Dynamically
 * ================================ */
static void
apply_delta_generic(const char *view_table, int num_cols, char **col_names, Datum *col_datums, char *nulls, int delta)
{
    StringInfoData query;
    StringInfoData del_query;
    SPIPlanPtr plan;
    Oid *argtypes;
    Datum *values;
    char *exec_nulls;
    int i;

    initStringInfo(&query);
    appendStringInfo(&query, "INSERT INTO %s(", view_table);
    for (i = 0; i < num_cols; i++)
    {
        appendStringInfo(&query, "%s, ", col_names[i]);
    }
    appendStringInfo(&query, "total_count) VALUES(");
    for (i = 0; i < num_cols; i++)
    {
        appendStringInfo(&query, "$%d, ", i + 1);
    }
    appendStringInfo(&query, "$%d) ON CONFLICT (", num_cols + 1);
    for (i = 0; i < num_cols; i++)
    {
        appendStringInfo(&query, "%s%s", col_names[i], (i == num_cols - 1) ? "" : ", ");
    }
    appendStringInfo(&query, ") DO UPDATE SET total_count = %s.total_count + $%d", view_table, num_cols + 1);

    /* Prepare arguments */
    argtypes = (Oid *) palloc((num_cols + 1) * sizeof(Oid));
    values = (Datum *) palloc((num_cols + 1) * sizeof(Datum));
    exec_nulls = (char *) palloc((num_cols + 1) * sizeof(char));

    for (i = 0; i < num_cols; i++)
    {
        argtypes[i] = TEXTOID;
        values[i] = col_datums[i];
        exec_nulls[i] = nulls[i];
    }
    argtypes[num_cols] = INT4OID;
    values[num_cols] = Int32GetDatum(delta);
    exec_nulls[num_cols] = ' ';

    plan = SPI_prepare(query.data, num_cols + 1, argtypes);
    if (plan == NULL)
        elog(ERROR, "SPI_prepare failed for upsert: %s", query.data);

    SPI_execute_plan(plan, values, exec_nulls, false, 0);

    /* Garbage Collection if decrement operation happened */
    if (delta < 0)
    {
        SPIPlanPtr delete_plan;
        Oid *delete_argtypes;
        Datum *delete_vals;
        char *delete_nulls;

        initStringInfo(&del_query);
        appendStringInfo(&del_query, "DELETE FROM %s WHERE ", view_table);
        for (i = 0; i < num_cols; i++)
        {
            appendStringInfo(&del_query, "%s = $%d AND ", col_names[i], i + 1);
        }
        appendStringInfo(&del_query, "total_count <= 0");

        delete_argtypes = (Oid *) palloc(num_cols * sizeof(Oid));
        delete_vals = (Datum *) palloc(num_cols * sizeof(Datum));
        delete_nulls = (char *) palloc(num_cols * sizeof(char));

        for (i = 0; i < num_cols; i++)
        {
            delete_argtypes[i] = TEXTOID;
            delete_vals[i] = col_datums[i];
            delete_nulls[i] = nulls[i];
        }

        delete_plan = SPI_prepare(del_query.data, num_cols, delete_argtypes);
        if (delete_plan != NULL)
        {
            SPI_execute_plan(delete_plan, delete_vals, delete_nulls, false, 0);
        }
    }
}

/* ================================
 * Trigger Function
 * ================================ */
PG_FUNCTION_INFO_V1(mini_ivm_maintain);

Datum
mini_ivm_maintain(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    TupleDesc tupdesc;
    Trigger *trigger;
    const char *view_table;
    int num_cols;
    char **col_names;
    int i;

    if (!CALLED_AS_TRIGGER(fcinfo))
        elog(ERROR, "Not called as trigger");

    SPI_connect();

    tupdesc = trigdata->tg_relation->rd_att;
    trigger = trigdata->tg_trigger;

    if (trigger->tgnargs < 2)
        elog(ERROR, "mini_ivm_maintain trigger requires at least 2 arguments: view_table and group columns");

    view_table = trigger->tgargs[0];
    num_cols = trigger->tgnargs - 1;
    col_names = (char **) palloc(num_cols * sizeof(char *));
    for (i = 0; i < num_cols; i++)
    {
        col_names[i] = trigger->tgargs[i + 1];
    }

    elog(INFO, "mini_ivm trigger fired for view %s", view_table);

    /* INSERT */
    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
    {
        HeapTuple newtuple = trigdata->tg_trigtuple;
        Datum *col_datums = (Datum *) palloc(num_cols * sizeof(Datum));
        char *nulls = (char *) palloc(num_cols * sizeof(char));
        bool any_null = false;

        for (i = 0; i < num_cols; i++)
        {
            char *val = get_column_value(newtuple, tupdesc, col_names[i]);
            if (val == NULL)
            {
                any_null = true;
                break;
            }
            col_datums[i] = CStringGetTextDatum(val);
            nulls[i] = ' ';
        }

        if (!any_null)
            apply_delta_generic(view_table, num_cols, col_names, col_datums, nulls, +1);
    }

    /* DELETE */
    else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
    {
        HeapTuple oldtuple = trigdata->tg_trigtuple;
        Datum *col_datums = (Datum *) palloc(num_cols * sizeof(Datum));
        char *nulls = (char *) palloc(num_cols * sizeof(char));
        bool any_null = false;

        for (i = 0; i < num_cols; i++)
        {
            char *val = get_column_value(oldtuple, tupdesc, col_names[i]);
            if (val == NULL)
            {
                any_null = true;
                break;
            }
            col_datums[i] = CStringGetTextDatum(val);
            nulls[i] = ' ';
        }

        if (!any_null)
            apply_delta_generic(view_table, num_cols, col_names, col_datums, nulls, -1);
    }

    /* UPDATE */
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
    {
        HeapTuple oldtuple = trigdata->tg_trigtuple;
        HeapTuple newtuple = trigdata->tg_newtuple;
        bool group_changed = false;
        Datum *old_datums = (Datum *) palloc(num_cols * sizeof(Datum));
        Datum *new_datums = (Datum *) palloc(num_cols * sizeof(Datum));
        char *old_nulls = (char *) palloc(num_cols * sizeof(char));
        char *new_nulls = (char *) palloc(num_cols * sizeof(char));
        bool old_any_null = false;
        bool new_any_null = false;

        for (i = 0; i < num_cols; i++)
        {
            char *old_val = get_column_value(oldtuple, tupdesc, col_names[i]);
            char *new_val = get_column_value(newtuple, tupdesc, col_names[i]);

            if (old_val == NULL) old_any_null = true;
            else
            {
                old_datums[i] = CStringGetTextDatum(old_val);
                old_nulls[i] = ' ';
            }

            if (new_val == NULL) new_any_null = true;
            else
            {
                new_datums[i] = CStringGetTextDatum(new_val);
                new_nulls[i] = ' ';
            }

            if ((old_val == NULL && new_val != NULL) ||
                (old_val != NULL && new_val == NULL) ||
                (old_val != NULL && new_val != NULL && strcmp(old_val, new_val) != 0))
            {
                group_changed = true;
            }
        }

        if (group_changed)
        {
            if (!old_any_null)
                apply_delta_generic(view_table, num_cols, col_names, old_datums, old_nulls, -1);

            if (!new_any_null)
                apply_delta_generic(view_table, num_cols, col_names, new_datums, new_nulls, +1);
        }
    }

    SPI_finish();

    return PointerGetDatum(NULL);
}

/* ================================
 * Setup Function
 * ================================ */
PG_FUNCTION_INFO_V1(create_mini_ivm);

Datum
create_mini_ivm(PG_FUNCTION_ARGS)
{
    text *source_table_text = PG_GETARG_TEXT_PP(0);
    text *view_table_text = PG_GETARG_TEXT_PP(1);
    ArrayType *group_cols_arr = PG_GETARG_ARRAYTYPE_P(2);
    char *source_table = text_to_cstring(source_table_text);
    char *view_table = text_to_cstring(view_table_text);
    
    Datum *group_cols_datums;
    bool *group_cols_nulls;
    int num_cols;
    int i;
    StringInfoData query;

    SPI_connect();

    /* Extract elements from grouping columns text array */
    deconstruct_array(group_cols_arr, TEXTOID, -1, false, typalign_of(TEXTOID),
                      &group_cols_datums, &group_cols_nulls, &num_cols);

    if (num_cols <= 0)
        elog(ERROR, "At least one grouping column must be specified");

    /* 1. Create dynamic IMMV table */
    initStringInfo(&query);
    appendStringInfo(&query, "CREATE TABLE IF NOT EXISTS %s (", view_table);
    for (i = 0; i < num_cols; i++)
    {
        char *col = TextDatumGetCString(group_cols_datums[i]);
        appendStringInfo(&query, "%s TEXT, ", col);
    }
    appendStringInfo(&query, "total_count INT, PRIMARY KEY (");
    for (i = 0; i < num_cols; i++)
    {
        char *col = TextDatumGetCString(group_cols_datums[i]);
        appendStringInfo(&query, "%s%s", col, (i == num_cols - 1) ? "" : ", ");
    }
    appendStringInfo(&query, "))");
    SPI_execute(query.data, false, 0);

    /* 2. Drop existing trigger if any */
    resetStringInfo(&query);
    appendStringInfo(&query, "DROP TRIGGER IF EXISTS mini_ivm_trigger_%s ON %s", view_table, source_table);
    SPI_execute(query.data, false, 0);

    /* 3. Create generic trigger executing mini_ivm_maintain */
    resetStringInfo(&query);
    appendStringInfo(&query, "CREATE TRIGGER mini_ivm_trigger_%s AFTER INSERT OR UPDATE OR DELETE ON %s "
                             "FOR EACH ROW EXECUTE FUNCTION mini_ivm_maintain('%s'", view_table, source_table, view_table);
    for (i = 0; i < num_cols; i++)
    {
        char *col = TextDatumGetCString(group_cols_datums[i]);
        appendStringInfo(&query, ", '%s'", col);
    }
    appendStringInfo(&query, ")");
    SPI_execute(query.data, false, 0);

    SPI_finish();

    PG_RETURN_VOID();
}