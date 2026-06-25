# Roadmap: From `mini_ivm` to a Production-Grade IVM Extension

This document outlines a step-by-step development plan to evolve `mini_ivm` from a basic, trigger-based counter into a robust Incremental Materialized View (IVM) engine similar to `pg_ivm`. It also serves as a learning path for mastering PostgreSQL C extension development.

---

## Phase 1: Mastering PostgreSQL Internals & C Fundamentals

Before building complex features, you need a solid grasp of how PostgreSQL works under the hood.

**Learning Goals:**
- Understand PostgreSQL Memory Contexts (`palloc`, `pfree`, `CurrentMemoryContext`). Memory leaks in C will crash your database!
- Learn how PostgreSQL represents data (`Datum`, `Oid`, `TupleDesc`, `HeapTuple`).
- Understand the PostgreSQL Error Reporting system (`elog`, `ereport`).

**Action Items for `mini_ivm`:**
- [ ] **Memory Management Audit:** Review `mini_ivm.c` to ensure all dynamically allocated memory (`palloc`ed strings/arrays) is properly managed, or rely correctly on the per-tuple memory context being reset.
- [ ] **Data Type Handling:** Currently, `mini_ivm` assumes all grouping columns can be cast to/from `TEXT`. Enhance it to use PostgreSQL's type cache to handle any data type (e.g., `INT`, `UUID`, `TIMESTAMP`) generically without converting everything to C-strings first.

---

## Phase 2: Statement-Level Processing & Bulk Operations

Currently, `mini_ivm` uses `FOR EACH ROW` triggers. If a user runs `UPDATE orders SET amount = amount + 10` on 1 million rows, your C function runs 1 million times, executing 1 million individual `SPI_execute` calls. This is too slow for production.

**Learning Goals:**
- Statement-level triggers (`FOR EACH STATEMENT`).
- Transition Tables (`REFERENCING NEW TABLE AS new_table OLD TABLE AS old_table`).

**Action Items for `mini_ivm`:**
- [ ] **Switch to Transition Tables:** Modify `create_mini_ivm` to create a statement-level trigger.
- [ ] **Batch Processing:** Inside the C function, instead of reading one `HeapTuple`, execute a query against the transition tables to aggregate all changes in the statement into a single internal "delta" set.
- [ ] **Batch Upsert:** Apply the entire delta set to the materialized view in a single SQL operation (e.g., `INSERT ... ON CONFLICT ...`) rather than looping.

---

## Phase 3: Moving Beyond SPI (Server Programming Interface)

SPI is the easiest way to execute SQL from C, but it adds overhead because it goes through the SQL parser and planner every time (even with prepared statements). `pg_ivm` and built-in features use lower-level APIs.

**Learning Goals:**
- The PostgreSQL Executor (`ExecInsert`, `ExecUpdate`, `ExecDelete`).
- Table Access Methods (TAM) (`table_open`, `table_beginscan`, `heap_insert`).
- Index lookups from C (`index_beginscan`, `index_rescan`).

**Action Items for `mini_ivm`:**
- [ ] **Direct Table Access:** Replace `SPI_execute` for garbage collection (`DELETE FROM ... WHERE total_count <= 0`) with a direct index scan to find the tuple, and `simple_heap_delete` to remove it.
- [ ] **Direct Upserts:** Replace the `SPI_execute_plan` upsert with lower-level executor calls to modify the view table directly.

---

## Phase 4: AST Parsing and Real View Definitions

Real IVM extensions don't ask users to pass array columns to a setup function. Users write standard SQL: `CREATE INCREMENTAL MATERIALIZED VIEW my_view AS SELECT a, b, count(*) FROM table GROUP BY a, b;`.

**Learning Goals:**
- PostgreSQL Parser (`raw_parser`).
- Process Utility Hook (`ProcessUtility_hook`) to intercept commands like `CREATE MATERIALIZED VIEW`.
- Abstract Syntax Trees (AST) in PostgreSQL (`Query`, `TargetEntry`, `Aggref`).

**Action Items for `mini_ivm`:**
- [ ] **Hook `ProcessUtility`:** Intercept SQL commands. Look for a custom syntax or intercept standard view creation.
- [ ] **Parse the Query Tree:** When a user creates a view, analyze the `SELECT` query tree to automatically determine the base table(s), the grouping columns, and the aggregates used.
- [ ] **Store Metadata:** Create a catalog table (e.g., `mini_ivm_views`) to store this parsed metadata so the triggers know exactly how to maintain the view.

---

## Phase 5: Supporting Complex Aggregates and Joins

Currently, `mini_ivm` only counts. A real IVM needs `SUM`, `AVG`, `MIN`, `MAX`, and the ability to join multiple base tables.

**Learning Goals:**
- Aggregate state transition functions.
- The mathematics of incremental view maintenance (e.g., how do you incrementally maintain a `MAX()` if the current maximum value is deleted? You need to keep a count of all values).
- Delta rules for Joins (e.g., `Δ(A ⨝ B) = (ΔA ⨝ B) ∪ (A ⨝ ΔB) ∪ (ΔA ⨝ ΔB)`).

**Action Items for `mini_ivm`:**
- [ ] **Implement `SUM`:** Add support for tracking `SUM(column)`.
- [ ] **Implement `AVG`:** Add support for `AVG` by tracking both `SUM` and `COUNT` internally.
- [ ] **Single-table `MIN`/`MAX`:** Implement algorithms to maintain extrema efficiently.
- [ ] **Multi-table Joins:** Update the AST parser to understand `JOIN`s. Create triggers on *all* underlying base tables. When Table A changes, use the transition tables of A joined against the current state of Table B to calculate the delta for the view.

---

## Recommended Learning Resources

1. **The Source Code:**
   - [PostgreSQL Source Tree (`src/backend/`)] - The ultimate source of truth. Read the comments in `src/include/executor/executor.h` and `src/include/access/heapam.h`.
   - [pg_ivm Source Code](https://github.com/sraoss/pg_ivm) - Study how they intercept utility commands and calculate deltas.
   - [Citus Source Code](https://github.com/citusdata/citus) - Excellent examples of hooking into the planner and executor.

2. **Books & Documentation:**
   - [The Internals of PostgreSQL](https://www.interdb.jp/pg/) - Absolutely essential reading for understanding PostgreSQL architecture.
   - [PostgreSQL Extension Development Documentation](https://www.postgresql.org/docs/current/extend.html).

3. **Community:**
   - `pgsql-hackers` Mailing List - Where PostgreSQL development happens. Search the archives for discussions on IVM.
