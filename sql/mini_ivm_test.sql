-- mini_ivm regression test

SET client_min_messages TO warning;
CREATE EXTENSION mini_ivm;
LOAD 'mini_ivm';

-- Cleanup existing leftover objects if any
DROP TABLE IF EXISTS mini_ivm_catalog CASCADE;
DROP TABLE IF EXISTS imv_order_summary CASCADE;
DROP TABLE IF EXISTS imv_sensor_summary CASCADE;
DROP TABLE IF EXISTS orders CASCADE;

-- Create base table
CREATE TABLE orders (
    product TEXT,
    category TEXT,
    amount NUMERIC
);

-- Create materialized view with all 5 aggregate types
CREATE MATERIALIZED VIEW order_summary AS
SELECT product, category,
       SUM(amount) AS total_amount,
       COUNT(*) AS num_orders,
       AVG(amount) AS avg_amount,
       MIN(amount) AS min_amount,
       MAX(amount) AS max_amount
FROM orders
GROUP BY product, category;

-- Check initial state (empty)
SELECT * FROM imv_order_summary ORDER BY product, category;

-- Test INSERT
INSERT INTO orders VALUES ('Laptop', 'Electronics', 1000);
INSERT INTO orders VALUES ('Laptop', 'Electronics', 1200);
INSERT INTO orders VALUES ('Phone', 'Electronics', 800);
SELECT 'INSERT' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test UPDATE non-group column (old value was the MIN, must recompute)
UPDATE orders SET amount = 1500 WHERE amount = 1000;
SELECT 'UPDATE_VAL' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test UPDATE group column (category change, DELETE+INSERT)
UPDATE orders SET category = 'Office' WHERE amount = 1200;
SELECT 'UPDATE_GROUP' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test DELETE
DELETE FROM orders WHERE amount = 800;
SELECT 'DELETE' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test multiple inserts into same group
INSERT INTO orders VALUES ('Laptop', 'Office', 100);
INSERT INTO orders VALUES ('Laptop', 'Office', 500);
INSERT INTO orders VALUES ('Laptop', 'Office', 300);
SELECT 'MULTI_INSERT' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test DELETE of MIN (forces recompute)
DELETE FROM orders WHERE amount = 100;
SELECT 'DELETE_MIN' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test DELETE of MAX (forces recompute)
DELETE FROM orders WHERE amount = 1500;
SELECT 'DELETE_MAX' as test, * FROM imv_order_summary ORDER BY product, category;

-- Test Generic Data Types (INT and TIMESTAMP)
CREATE TABLE sensor_data (
    device_id INT,
    recorded_at TIMESTAMP,
    reading NUMERIC
);

CREATE MATERIALIZED VIEW sensor_summary AS
SELECT device_id, recorded_at,
       SUM(reading) AS total_reading,
       COUNT(*) AS cnt,
       MIN(reading) AS min_reading,
       MAX(reading) AS max_reading
FROM sensor_data
GROUP BY device_id, recorded_at;

INSERT INTO sensor_data VALUES (1, '2026-01-01 10:00:00', 42.5);
INSERT INTO sensor_data VALUES (1, '2026-01-01 10:00:00', 10.0);
SELECT 'TYPES_INSERT' as test, * FROM imv_sensor_summary ORDER BY device_id, recorded_at;

UPDATE sensor_data SET reading = 50.0 WHERE reading = 10.0;
SELECT 'TYPES_UPDATE' as test, * FROM imv_sensor_summary ORDER BY device_id, recorded_at;

DELETE FROM sensor_data WHERE reading = 50.0;
SELECT 'TYPES_DELETE' as test, * FROM imv_sensor_summary ORDER BY device_id, recorded_at;

DROP MATERIALIZED VIEW sensor_summary;
DROP TABLE sensor_data;

-- Test Multi-Table JOIN IMMV
CREATE TABLE categories (
    cat_id INT PRIMARY KEY,
    cat_name TEXT
);

CREATE TABLE items (
    item_id INT PRIMARY KEY,
    category_id INT,
    price NUMERIC
);

INSERT INTO categories VALUES (1, 'Electronics'), (2, 'Books');
INSERT INTO items VALUES (101, 1, 500), (102, 1, 300), (103, 2, 50);

CREATE MATERIALIZED VIEW category_sales AS
SELECT c.cat_name,
       SUM(i.price) AS total_sales,
       AVG(i.price) AS avg_price,
       COUNT(*) AS item_count
FROM items i JOIN categories c ON i.category_id = c.cat_id
GROUP BY c.cat_name;

SELECT 'JOIN_INITIAL' as test, * FROM imv_category_sales ORDER BY cat_name;

-- Test INSERT into items (base table 1)
INSERT INTO items VALUES (104, 1, 200);
SELECT 'JOIN_INSERT_ITEM' as test, * FROM imv_category_sales ORDER BY cat_name;

-- Test INSERT into categories (base table 2) & items
INSERT INTO categories VALUES (3, 'Clothing');
INSERT INTO items VALUES (105, 3, 100);
SELECT 'JOIN_INSERT_CAT' as test, * FROM imv_category_sales ORDER BY cat_name;

-- Test UPDATE item price
UPDATE items SET price = 600 WHERE item_id = 101;
SELECT 'JOIN_UPDATE_ITEM' as test, * FROM imv_category_sales ORDER BY cat_name;

-- Test DELETE item
DELETE FROM items WHERE item_id = 102;
SELECT 'JOIN_DELETE_ITEM' as test, * FROM imv_category_sales ORDER BY cat_name;

-- Test UPDATE category name
UPDATE categories SET cat_name = 'Tech & Gadgets' WHERE cat_id = 1;
SELECT 'JOIN_UPDATE_CAT' as test, * FROM imv_category_sales ORDER BY cat_name;

-- Test DELETE category
DELETE FROM items WHERE category_id = 2;
DELETE FROM categories WHERE cat_id = 2;
SELECT 'JOIN_DELETE_CAT' as test, * FROM imv_category_sales ORDER BY cat_name;

DROP MATERIALIZED VIEW category_sales;
DROP TABLE items;
DROP TABLE categories;

-- Cleanup
DROP MATERIALIZED VIEW order_summary;
DROP TABLE orders;


