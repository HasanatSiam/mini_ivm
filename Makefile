EXTENSION = mini_ivm
MODULE_big = mini_ivm
DATA = mini_ivm--1.0.sql
REGRESS = mini_ivm_test

OBJS = mini_ivm_hook.o \
       mini_ivm_utils.o \
       mini_ivm_config.o \
       mini_ivm_maintain.o \
       mini_ivm_ddl.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)