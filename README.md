# mini_ivm: Generic Incremental Materialized View builder

`mini_ivm` is a lightweight **PostgreSQL C Extension** designed to dynamically construct and incrementally maintain materialized summary tables (views) from any base table. 

Unlike traditional PostgreSQL materialized views that require a full table rewrite (`REFRESH MATERIALIZED VIEW`), `mini_ivm` updates row counts incrementally on modification (`INSERT`, `UPDATE`, `DELETE`) using row-level database triggers.

---

## Features

- **Fully Generic:** Works with any source table, dynamic destination (view) table, and custom lists of text-based grouping columns.
- **Optimized UPDATE Handling:** Trigger logic ignores column updates that don't affect the grouping columns (e.g., updating a `price` or `amount` column does not execute redundant database writes).
- **Targeted Garbage Collection:** Rows with `total_count <= 0` are deleted target-wise (using indexed parameters) rather than querying a slow, table-wide scan delete.
- **Standard Extension Testing:** Includes standard `pg_regress` integration tests.

---

## Installation & Build

### Prerequisites
Make sure you have the PostgreSQL development headers installed (e.g., `postgresql-server-dev-16` or equivalent).

### 1. Compile the extension
Run `make` to compile the C source code:
```bash
make
```

### 2. Install to PostgreSQL library directory
Run `make install` (requires sudo permissions to copy files into your PostgreSQL path):
```bash
sudo make install
```

---

## How to Use

### 1. Enable the extension
In your PostgreSQL client:
```sql
CREATE EXTENSION mini_ivm;
```

### 2. Initialize an Incremental Materialized View
Invoke the `create_mini_ivm` setup function:
```sql
SELECT create_mini_ivm(
    'source_table_name', 
    'materialized_view_name', 
    ARRAY['group_column_1', 'group_column_2']
);
```

#### What happens behind the scenes?
- A new table named `materialized_view_name` is created containing your grouping columns (with a composite `PRIMARY KEY`) and a `total_count` column.
- An `AFTER INSERT OR UPDATE OR DELETE` row-level trigger is attached to your source table, invoking the C-backed `mini_ivm_maintain` function to update counts on mutations.

---

## Example Usage

Let's assume you have a base table named `orders`:
```sql
CREATE TABLE orders (
    product TEXT,
    category TEXT,
    location TEXT,
    amount NUMERIC
);

-- Set up dynamic count tracking for product + category
SELECT create_mini_ivm('orders', 'mv_product_category', ARRAY['product', 'category']);
```

### Insert Operations
Inserting rows into `orders` automatically populates counts:
```sql
INSERT INTO orders VALUES ('Laptop', 'Electronics', 'USA', 1000);
INSERT INTO orders VALUES ('Laptop', 'Electronics', 'USA', 1200);

SELECT * FROM mv_product_category;
-- Outputs:
--  product |  category   | total_count 
-- ---------+-------------+-------------
--  Laptop  | Electronics |           2
```

### Update Operations
Updating non-group columns (e.g., `amount`) triggers no writes on `mv_product_category`:
```sql
UPDATE orders SET amount = 1100 WHERE amount = 1000; -- No delta recalculations!
```

Updating grouping columns correctly relocates the summary:
```sql
UPDATE orders SET category = 'Office' WHERE amount = 1100;

SELECT * FROM mv_product_category;
-- Outputs:
--  product |  category   | total_count 
-- ---------+-------------+-------------
--  Laptop  | Electronics |           1
--  Laptop  | Office      |           1
```

---

## Running the Tests
This project includes a regression suite integrated with `pg_regress`. 

Once the extension is installed, run:
```bash
make installcheck
```
The test inputs are located in [sql/mini_ivm_test.sql](sql/mini_ivm_test.sql) and matched against the expected outputs in [expected/mini_ivm_test.out](expected/mini_ivm_test.out).
