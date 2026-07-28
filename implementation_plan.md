# Make mini_ivm Multi-Table Supported

This plan outlines the steps to support Incremental View Maintenance (IVM) for Materialized Views with multiple base tables (JOINs).

## Background
Currently, `mini_ivm` only supports a single base table. It uses `FOR EACH ROW` triggers and extracts column values directly from the `NEW`/`OLD` tuples via `SPI_getvalue`. 
When dealing with multiple tables (e.g., `A JOIN B`), an `INSERT` on `A` only provides a tuple for `A`. We cannot compute the aggregated values (which might depend on columns in `B`) without joining the `NEW` tuple of `A` with the current state of table `B`.

## Proposed Changes

To support multiple tables efficiently and correctly, we need to implement **Delta Processing** using **Transition Tables** and **Statement-Level Triggers** (as outlined in Phase 2 and 5 of the ROADMAP).

### 1. AST Parsing for Multiple Tables
- Update the parser in `create_incremental_mv` to handle `JoinExpr` and multiple `RangeVar`s in the `FROM` clause.
- Extract a list of all base tables involved in the view.

### 2. Transition Tables & Statement-Level Triggers
- Instead of `FOR EACH ROW` triggers, attach `FOR EACH STATEMENT` triggers to **all** base tables.
- Use `REFERENCING NEW TABLE AS ivm_new OLD TABLE AS ivm_old`.
- Pass the original view query (or a modified version) to the trigger function.

### 3. Delta Calculation (The Hard Part)
When a base table `T` is modified, we need to compute the delta by replacing `T` in the original query with the transition table (`ivm_new` or `ivm_old`).
- **Option A (AST Rewrite):** In the trigger, parse the original view query, locate the `RangeVar` for the triggered table, and change its name to `ivm_new` (or `ivm_old`). Then, deparse it back to SQL and execute it to get the delta tuples.
- **Option B (Temporary View/Rule):** Create delta views during `create_incremental_mv`.

### 4. Apply Deltas
Apply the computed delta (which now contains the fully joined and grouped rows) to the `imv_*` aggregation table using `INSERT ... ON CONFLICT DO UPDATE`.

## User Review Required

> [!WARNING]
> Implementing full multi-table IVM with JOIN deltas in a C extension is highly complex and requires significant rewrites of `mini_ivm.c`. 
> 
> Specifically, modifying the AST to swap base tables with transition tables and deparsing it back to a query string requires advanced usage of PostgreSQL internal APIs (like `pg_analyze_and_rewrite` and `deparse_query`). 

## Open Questions

1. **Scope of "Multi-Table":** Do you want full Incremental View Maintenance for JOINs (computing deltas via transition tables), or are you looking for a simpler approach (e.g., full refresh on change, or only specific types of multi-table queries)?
2. **Transition Tables:** Are you comfortable with moving from `FOR EACH ROW` to `FOR EACH STATEMENT` triggers? This is necessary because row-level triggers cannot easily access the joined state of other tables without issuing separate queries per row.

## Verification Plan

- Add tests in `sql/mini_ivm_test.sql` with a `JOIN` between two tables.
- Verify `INSERT`, `UPDATE`, and `DELETE` on both base tables incrementally update the view correctly.
