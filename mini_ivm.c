#include "postgres.h"
#include "fmgr.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"

PG_MODULE_MAGIC;

/* Cached SPI Plan */
static SPIPlanPtr upsert_plan = NULL;

/* ================================
 * Prepare SPI Plan
 * ================================ */
static void
prepare_plan(void)
{
    if (upsert_plan != NULL)
        return;

    Oid argtypes[3] = {TEXTOID, TEXTOID, INT4OID};

    upsert_plan = SPI_prepare(
        "INSERT INTO mini_ivm(product, category, total_count) "
        "VALUES($1, $2, $3) "
        "ON CONFLICT (product, category) DO UPDATE "
        "SET total_count = mini_ivm.total_count + $3",
        3,
        argtypes
    );

    if (upsert_plan == NULL)
        elog(ERROR, "SPI_prepare failed");

    SPI_keepplan(upsert_plan);
}

/* ================================
 * Apply Delta (+1 / -1)
 * ================================ */
static void
apply_delta(char *product, char *category, int delta)
{
    Datum values[3];
    bool nulls[3] = {false, false, false};

    values[0] = CStringGetTextDatum(product);
    values[1] = CStringGetTextDatum(category);
    values[2] = Int32GetDatum(delta);

    SPI_execute_plan(upsert_plan, values, nulls, false, 0);

    /* Remove zero-count rows */
    SPI_execute(
        "DELETE FROM mini_ivm WHERE total_count <= 0",
        false, 0
    );
}

/* ================================
 * Extract column by name
 * ================================ */
static char *
get_column_value(HeapTuple tuple, TupleDesc tupdesc, const char *colname)
{
    bool isnull;
    int attnum = SPI_fnumber(tupdesc, colname);

    if (attnum == SPI_ERROR_NOATTRIBUTE)
        elog(ERROR, "Column \"%s\" not found", colname);

    Datum val = SPI_getbinval(tuple, tupdesc, attnum, &isnull);

    if (isnull)
        return NULL;

    return TextDatumGetCString(val);
}

/* ================================
 * Trigger Function
 * ================================ */
PG_FUNCTION_INFO_V1(mini_ivm_maintain);

Datum
mini_ivm_maintain(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;

    if (!CALLED_AS_TRIGGER(fcinfo))
        elog(ERROR, "Not called as trigger");

    SPI_connect();
    prepare_plan();

    TupleDesc tupdesc = trigdata->tg_relation->rd_att;

    elog(INFO, "mini_ivm trigger fired");

    /* INSERT */
    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
    {
        HeapTuple newtuple = trigdata->tg_trigtuple;

        char *product = get_column_value(newtuple, tupdesc, "product");
        char *category = get_column_value(newtuple, tupdesc, "category");

        if (product && category)
            apply_delta(product, category, +1);
    }

    /* DELETE */
    else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
    {
        HeapTuple oldtuple = trigdata->tg_trigtuple;

        char *product = get_column_value(oldtuple, tupdesc, "product");
        char *category = get_column_value(oldtuple, tupdesc, "category");

        if (product && category)
            apply_delta(product, category, -1);
    }

    /* UPDATE */
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
    {
        HeapTuple oldtuple = trigdata->tg_trigtuple;
        HeapTuple newtuple = trigdata->tg_newtuple;

        char *old_product = get_column_value(oldtuple, tupdesc, "product");
        char *old_category = get_column_value(oldtuple, tupdesc, "category");

        char *new_product = get_column_value(newtuple, tupdesc, "product");
        char *new_category = get_column_value(newtuple, tupdesc, "category");

        if (old_product && old_category)
            apply_delta(old_product, old_category, -1);

        if (new_product && new_category)
            apply_delta(new_product, new_category, +1);
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
    SPI_connect();

    /* Create IMMV table */
    SPI_execute(
        "CREATE TABLE IF NOT EXISTS mini_ivm ("
        "product TEXT, "
        "category TEXT, "
        "total_count INT, "
        "PRIMARY KEY (product, category))",
        false, 0
    );

    /* Drop existing trigger */
    SPI_execute(
        "DROP TRIGGER IF EXISTS mini_ivm_trigger ON orders",
        false, 0
    );

    /* Create trigger */
    SPI_execute(
        "CREATE TRIGGER mini_ivm_trigger "
        "AFTER INSERT OR UPDATE OR DELETE ON orders "
        "FOR EACH ROW EXECUTE FUNCTION mini_ivm_maintain()",
        false, 0
    );

    SPI_finish();

    PG_RETURN_VOID();
}