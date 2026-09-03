-- mini_ivm regression test

SET client_min_messages TO warning;
SET dynamic_library_path TO '/tmp';

CREATE OR REPLACE FUNCTION mini_ivm_maintain()
RETURNS trigger
AS '/tmp/mini_ivm', 'mini_ivm_maintain'
LANGUAGE C;

CREATE OR REPLACE FUNCTION create_incremental_mv(mv_name text)
RETURNS void
AS '/tmp/mini_ivm', 'create_incremental_mv'
LANGUAGE C;

CREATE OR REPLACE FUNCTION drop_incremental_mv(mv_name text)
RETURNS void
AS '/tmp/mini_ivm', 'drop_incremental_mv'
LANGUAGE C;

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

-- Create materialized view with all 4 aggregate types
CREATE MATERIALIZED VIEW order_summary AS
SELECT product, category,
       SUM(amount) AS total_amount,
       COUNT(*) AS num_orders,
       MIN(amount) AS min_amount,
       MAX(amount) AS max_amount
FROM orders
GROUP BY product, category;

-- Create the incremental materialized view
SELECT create_incremental_mv('order_summary');

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

SELECT create_incremental_mv('sensor_summary');

INSERT INTO sensor_data VALUES (1, '2026-01-01 10:00:00', 42.5);
INSERT INTO sensor_data VALUES (1, '2026-01-01 10:00:00', 10.0);
SELECT 'TYPES_INSERT' as test, * FROM imv_sensor_summary ORDER BY device_id, recorded_at;

UPDATE sensor_data SET reading = 50.0 WHERE reading = 10.0;
SELECT 'TYPES_UPDATE' as test, * FROM imv_sensor_summary ORDER BY device_id, recorded_at;

DELETE FROM sensor_data WHERE reading = 50.0;
SELECT 'TYPES_DELETE' as test, * FROM imv_sensor_summary ORDER BY device_id, recorded_at;

SELECT drop_incremental_mv('sensor_summary');
DROP MATERIALIZED VIEW sensor_summary;
DROP TABLE sensor_data;

-- Cleanup
SELECT drop_incremental_mv('order_summary');
DROP MATERIALIZED VIEW order_summary;
DROP TABLE orders;

