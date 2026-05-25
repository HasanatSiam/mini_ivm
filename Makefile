EXTENSION = mini_ivm
MODULES = mini_ivm
DATA = mini_ivm--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)