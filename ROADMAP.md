# Roadmap: From `mini_ivm` to `pg_ivm` (Production IVM Extension)

This document outlines a progressive, step-by-step learning and development roadmap to evolve `mini_ivm` from a basic row-level trigger counter into a production-grade Incremental Materialized View (IVM) engine matching the architecture of `pg_ivm`.

---

## Phase 1: Mastering PostgreSQL Internals & C Fundamentals

Before building complex features, you need a solid grasp of how PostgreSQL works under the hood.

**Learning Goals:**
- Understand PostgreSQL Memory Contexts (`palloc`, `pfree`, `CurrentMemoryContext`, `AllocSetContextCreate`).
- Learn how PostgreSQL represents data (`Datum`, `Oid`, `TupleDesc`, `HeapTuple`).
- Understand PostgreSQL Type Cache (`lookup_type_cache`) and generic `Datum` handling.

**Action Items for `mini_ivm`:**
- [x] **Memory Management Audit:** Review `mini_ivm.c` to ensure all dynamically allocated memory (`palloc`ed strings/arrays) is properly managed, using transient memory contexts (`AllocSetContextCreate`/`MemoryContextDelete`) and explicit `pfree` deallocations.
- [ ] **Data Type Handling:** Currently, `mini_ivm` assumes grouping columns can be cast to/from `TEXT`. Enhance it to use PostgreSQL's type cache (`lookup_type_cache`) to handle any data type (e.g., `INT`, `UUID`, `TIMESTAMP`) generically without converting everything to C-strings first.

---

## Phase 2: Transition Tables & Statement-Level Maintenance

Currently, `mini_ivm` uses `FOR EACH ROW` triggers. If a user updates 1 million rows in `orders`, `mini_ivm` runs 1 million times with 1 million SPI executions. Production `pg_ivm` operates on statement-level deltas.

**Learning Goals:**
- Statement-level triggers (`FOR EACH STATEMENT`).
- Ephemeral Transition Tables (`REFERENCING NEW TABLE AS new_table OLD TABLE AS old_table`).
- Algebraic delta calculations over transition table sets.

**Action Items for `mini_ivm`:**
- [ ] **Switch to Transition Tables:** Modify `create_incremental_mv` to create `FOR EACH STATEMENT` triggers.
- [ ] **Batch Processing:** Inside the C trigger, query transition tables (`new_table` and `old_table`) to compute a single aggregated "delta set" for the statement.
- [ ] **Batch Upsert:** Apply the entire aggregated delta set to the materialized view in a single SQL operation (`INSERT ... ON CONFLICT ... DO UPDATE`) rather than looping per tuple.

---

## Phase 3: Catalog Tables, Metadata & ProcessUtility Hook

Real IVM extensions like `pg_ivm` don't require users to pass comma-separated column arrays to setup functions. Users write standard SQL (`CREATE INCREMENTAL MATERIALIZED VIEW ...`).

**Learning Goals:**
- ProcessUtility Hook (`ProcessUtility_hook`) to intercept utility statements.
- Abstract Syntax Trees (AST) in PostgreSQL (`Query`, `TargetEntry`, `Aggref`).
- Extension System Catalog Tables (storing view definitions & tracking metadata).

**Action Items for `mini_ivm`:**
- [x] **Parse the Query Tree:** Analyze the `SELECT` query tree to automatically determine base table(s), grouping columns, and aggregate functions.
- [ ] **Catalog Metadata Storage:** Create an internal catalog table (e.g., `mini_ivm_catalog`) to store parsed view definitions, base table mappings, and grouping column metadata instead of encoding them into trigger argument strings.
- [ ] **Hook `ProcessUtility`:** Intercept SQL commands like `CREATE INCREMENTAL MATERIALIZED VIEW` or custom DDL syntax to automate IMMV creation transparently.

---

## Phase 4: Algebraic Maintenance, Multi-Set Counts & Duplicates

In real relational algebra, IVM requires tracking tuple multiplicities and managing zero-count tuples.

**Learning Goals:**
- Relational algebra for IVM.
- The `__ivm_count__` auxiliary tuple count column pattern used by `pg_ivm`.
- Zero-count tuple garbage collection rules.

**Action Items for `mini_ivm`:**
- [ ] **Add Hidden Count Column (`__ivm_count__`):** Introduce an auxiliary `__ivm_count__ BIGINT` column to the IMMV table to track exact tuple counts across operations and handle duplicates correctly.
- [ ] **Zero-Count Garbage Collection:** Delete rows when `__ivm_count__ <= 0`.

---

## Phase 5: Moving Beyond SPI to Lower-Level Executor APIs

SPI adds SQL parser/planner overhead for every execution. Production `pg_ivm` and PostgreSQL core bypass SPI using lower-level Executor APIs.

**Learning Goals:**
- The PostgreSQL Executor API (`ExecutorStart`, `ExecutorRun`, `ExecutorEnd`).
- Table Access Methods (TAM) (`table_open`, `table_beginscan`, `heap_insert`).
- Direct index lookups from C (`index_beginscan`, `index_rescan`).

**Action Items for `mini_ivm`:**
- [ ] **Direct Index Scanning & Deletes:** Replace SPI garbage collection with direct index scans and `simple_heap_delete`.
- [ ] **Direct Table Modification:** Use lower-level executor routines to modify IMMV tuples directly without SPI SQL re-parsing.

---

## Phase 6: Supporting Complex Aggregates and Multi-Table Joins

Production `pg_ivm` handles `SUM`, `COUNT`, `AVG`, `MIN`, `MAX`, and multi-table `JOIN`s.

**Learning Goals:**
- Delta rules for Joins: `Δ(R ⨝ S) = (ΔR ⨝ S) ∪ (R ⨝ ΔS) ∪ (ΔR ⨝ ΔS)`.
- Aggregate state transition functions and maintaining extrema (`MIN`/`MAX`).

**Action Items for `mini_ivm`:**
- [x] **Implement `SUM`:** Track and update `SUM(column)`.
- [ ] **Implement `AVG`:** Support `AVG` by tracking both `SUM` and `COUNT` internally.
- [x] **Single-Table `MIN`/`MAX`:** Maintain extrema efficiently when rows are deleted or updated.
- [ ] **Multi-Table Joins:** Extend AST parser to understand `JOIN` queries. Create triggers on *all* referenced base tables and evaluate delta join rules against current base table states.

---

## Recommended Learning Resources

1. **The Source Code:**
   - [PostgreSQL Source Tree (`src/backend/`)] - The ultimate source of truth (`src/include/executor/executor.h`, `src/include/access/heapam.h`).
   - [pg_ivm Source Code](https://github.com/sraoss/pg_ivm) - Study `pg_ivm`'s AST analysis, delta query generation, and IMMV catalog management.
   - [Citus Source Code](https://github.com/citusdata/citus) - Excellent examples of `ProcessUtility_hook` and executor hooks.

2. **Books & Documentation:**
   - [The Internals of PostgreSQL](https://www.interdb.jp/pg/) - Essential architectural guide for PostgreSQL internals.
   - [PostgreSQL Extension Development Documentation](https://www.postgresql.org/docs/current/extend.html).

