# mini_ivm: Incremental Materialized View Maintenance for PostgreSQL

`mini_ivm` is a lightweight **PostgreSQL C Extension** that incrementally maintains materialized views using database triggers. Instead of requiring a full `REFRESH MATERIALIZED VIEW`, it updates the underlying aggregation table on every `INSERT`, `UPDATE`, or `DELETE` on the source table.

Supports **SUM**, **COUNT**, **MIN**, and **MAX** aggregates with automatic AST-based parsing of the materialized view definition.

---

## Features

- **SQL-based setup:** Create a standard `MATERIALIZED VIEW`, then call `create_incremental_mv('view_name')` — the extension parses the view definition automatically.
- **SUM, COUNT, MIN, MAX:** Full incremental maintenance for all four aggregate types.
- **Smart UPDATE handling:** Grouping column changes trigger DELETE+INSERT; value-only changes update aggregates in-place; no-op changes are skipped.
- **MIN/MAX recomputation:** When the min or max row is deleted, the extension automatically recomputes from the source data.
- **Garbage collection:** Zero-count rows are pruned automatically.

---

## Installation & Build

### Prerequisites
PostgreSQL development headers (e.g., `postgresql-server-dev-16` or equivalent).

### 1. Compile
```bash
make
```

### 2. Install
```bash
sudo make install
```

### 3. Verify library location

After `sudo make install`, verify the shared library is in PostgreSQL's library directory:

```bash
ls -la $(pg_config --pkglibdir)/mini_ivm.so
```

If missing (e.g., after a manual build), copy it there:

```bash
sudo cp mini_ivm.so $(pg_config --pkglibdir)/
```

> **Note:** You need to re-copy the `.so` after every `make` (recompilation). A server **restart is not required** — just `DROP EXTENSION mini_ivm; CREATE EXTENSION mini_ivm;`.

### 4. Enable in database
```sql
CREATE EXTENSION mini_ivm;
```

---

## Usage

### 1. Create a base table and a standard materialized view
```sql
CREATE TABLE orders (
    product  TEXT,
    category TEXT,
    amount   NUMERIC
);

CREATE MATERIALIZED VIEW order_summary AS
SELECT
    product,
    category,
    SUM(amount) AS total_amount,
    COUNT(*)    AS num_orders,
    MIN(amount) AS min_amount,
    MAX(amount) AS max_amount
FROM orders
GROUP BY product, category;
```

### 2. Enable incremental maintenance
```sql
SELECT create_incremental_mv('order_summary');
```

This creates an internal tracking table (`imv_order_summary`), backfills it with current data, and attaches triggers to the source table.

### 3. Use normally — the view stays up to date

```sql
-- Initial state (empty)
SELECT * FROM imv_order_summary ORDER BY product, category;
--  product | category | total_amount | num_orders | min_amount | max_amount
-- ---------+----------+--------------+------------+------------+------------
-- (0 rows)

-- INSERT
INSERT INTO orders VALUES ('Laptop', 'Electronics', 1000);
INSERT INTO orders VALUES ('Laptop', 'Electronics', 1200);
INSERT INTO orders VALUES ('Phone', 'Electronics', 800);

SELECT * FROM imv_order_summary ORDER BY product, category;
--  product |  category   | total_amount | num_orders | min_amount | max_amount
-- ---------+-------------+--------------+------------+------------+------------
--  Laptop  | Electronics |         2200 |          2 |       1000 |       1200
--  Phone   | Electronics |          800 |          1 |        800 |        800

-- UPDATE value column (old MIN=1000 becomes 1500, MIN recomputes)
UPDATE orders SET amount = 1500 WHERE amount = 1000;

SELECT * FROM imv_order_summary ORDER BY product, category;
--  product |  category   | total_amount | num_orders | min_amount | max_amount
-- ---------+-------------+--------------+------------+------------+------------
--  Laptop  | Electronics |         2700 |          2 |       1200 |       1500
--  Phone   | Electronics |          800 |          1 |        800 |        800

-- UPDATE group column (category change — DELETE+INSERT)
UPDATE orders SET category = 'Office' WHERE amount = 1200;

SELECT * FROM imv_order_summary ORDER BY product, category;
--  product |  category   | total_amount | num_orders | min_amount | max_amount
-- ---------+-------------+--------------+------------+------------+------------
--  Laptop  | Electronics |         1500 |          1 |       1500 |       1500
--  Laptop  | Office      |         1200 |          1 |       1200 |       1200
--  Phone   | Electronics |          800 |          1 |        800 |        800

-- DELETE
DELETE FROM orders WHERE amount = 800;

SELECT * FROM imv_order_summary ORDER BY product, category;
--  product |  category   | total_amount | num_orders | min_amount | max_amount
-- ---------+-------------+--------------+------------+------------+------------
--  Laptop  | Electronics |         1500 |          1 |       1500 |       1500
--  Laptop  | Office      |         1200 |          1 |       1200 |       1200
```

### 4. Drop incremental tracking
```sql
SELECT drop_incremental_mv('order_summary');
```

This removes the trigger and internal tracking table. The original materialized view is preserved.

---

## Running Tests

```bash
make installcheck
```

Test inputs: `sql/mini_ivm_test.sql` · Expected output: `expected/mini_ivm_test.out`
