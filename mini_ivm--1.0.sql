CREATE FUNCTION mini_ivm_maintain()
RETURNS trigger
AS 'MODULE_PATHNAME', 'mini_ivm_maintain'
LANGUAGE C;

CREATE FUNCTION create_mini_ivm(source_table text, view_table text, group_cols text[])
RETURNS void
AS 'MODULE_PATHNAME', 'create_mini_ivm'
LANGUAGE C;