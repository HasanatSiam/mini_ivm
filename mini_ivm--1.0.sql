CREATE FUNCTION mini_ivm_maintain()
RETURNS trigger
AS 'MODULE_PATHNAME', 'mini_ivm_maintain'
LANGUAGE C;

CREATE FUNCTION create_incremental_mv(mv_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'create_incremental_mv'
LANGUAGE C;

CREATE FUNCTION drop_incremental_mv(mv_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'drop_incremental_mv'
LANGUAGE C;
