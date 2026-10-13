\set device_id random(1, 100)
\set event_type random(1, 5)
\set val random(10, 1000)
\set op random(1, 10)

DO $$
DECLARE
    v_op INT := :op;
    v_id INT;
BEGIN
    IF v_op <= 7 THEN
        -- 70% chance of INSERT
        INSERT INTO test_events (device_id, event_type, val) 
        VALUES (:device_id, 'type_' || :event_type, :val);
    ELSIF v_op <= 9 THEN
        -- 20% chance of UPDATE (pick a random row for this device to avoid deadlocks where possible)
        SELECT id INTO v_id FROM test_events WHERE device_id = :device_id LIMIT 1;
        IF FOUND THEN
            UPDATE test_events SET val = :val WHERE id = v_id;
        END IF;
    ELSE
        -- 10% chance of DELETE
        SELECT id INTO v_id FROM test_events WHERE device_id = :device_id LIMIT 1;
        IF FOUND THEN
            DELETE FROM test_events WHERE id = v_id;
        END IF;
    END IF;
END $$;
