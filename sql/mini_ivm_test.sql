-- test/sql/mini_ivm_test.sql
-- Setup extension
CREATE EXTENSION IF NOT EXISTS mini_ivm;

-- Create source table
CREATE TABLE test_orders (
    product TEXT,
    category TEXT,
    location TEXT,
    amount NUMERIC
);

-- Initialize generic IVM with 2 group columns
SELECT create_mini_ivm('test_orders', 'mv_product_category', ARRAY['product', 'category']);

-- Initialize a second IVM on the same source table with 3 group columns
SELECT create_mini_ivm('test_orders', 'mv_full_summary', ARRAY['product', 'category', 'location']);

-- Test 1: Single Inserts
INSERT INTO test_orders VALUES ('Laptop', 'Electronics', 'USA', 1000);
INSERT INTO test_orders VALUES ('Laptop', 'Electronics', 'USA', 1200);
INSERT INTO test_orders VALUES ('Shirt', 'Apparel', 'UK', 25);
INSERT INTO test_orders VALUES ('Shirt', 'Apparel', 'USA', 25);

SELECT 'Table: mv_product_category' AS test_1_mv1;
SELECT * FROM mv_product_category ORDER BY product, category;

SELECT 'Table: mv_full_summary' AS test_1_mv2;
SELECT * FROM mv_full_summary ORDER BY product, category, location;

-- Test 2: Updates (Not changing group columns)
UPDATE test_orders SET amount = 1100 WHERE product = 'Laptop' AND amount = 1000;

SELECT 'Table: mv_product_category (after non-group update)' AS test_2_mv1;
SELECT * FROM mv_product_category ORDER BY product, category;

-- Test 3: Updates (Changing group columns)
UPDATE test_orders SET category = 'Office' WHERE product = 'Laptop' AND amount = 1100;

SELECT 'Table: mv_product_category (after group update)' AS test_3_mv1;
SELECT * FROM mv_product_category ORDER BY product, category;

-- Test 4: Deletes
DELETE FROM test_orders WHERE product = 'Shirt';

SELECT 'Table: mv_product_category (after deletes)' AS test_4_mv1;
SELECT * FROM mv_product_category ORDER BY product, category;
SELECT 'Table: mv_full_summary (after deletes)' AS test_4_mv2;
SELECT * FROM mv_full_summary ORDER BY product, category, location;
