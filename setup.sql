DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS imv_order_summary CASCADE;
DROP MATERIALIZED VIEW IF EXISTS order_summary CASCADE;
CREATE TABLE orders (product TEXT, category TEXT, amount NUMERIC);
CREATE MATERIALIZED VIEW order_summary AS 
SELECT product, category, SUM(amount) AS total_amount, COUNT(*) AS num_orders, MIN(amount) AS min_amount, MAX(amount) AS max_amount 
FROM orders GROUP BY product, category;