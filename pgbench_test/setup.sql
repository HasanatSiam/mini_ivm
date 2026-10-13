DROP EXTENSION IF EXISTS mini_ivm CASCADE;
CREATE EXTENSION mini_ivm;

DROP TABLE IF EXISTS test_events CASCADE;
CREATE TABLE test_events (
    id SERIAL PRIMARY KEY,
    device_id INT,
    event_type TEXT,
    val NUMERIC
);

CREATE MATERIALIZED VIEW mv_events_summary AS
SELECT device_id, event_type,
       COUNT(*) as event_count,
       SUM(val) as total_val,
       AVG(val) as avg_val
FROM test_events
GROUP BY device_id, event_type;

SELECT create_incremental_mv('mv_events_summary');
