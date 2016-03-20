# pg_keeper/Makefile

MODULE_big = pg_keeper
OBJS = pg_keeper.o

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK = $(libpq)

REGRESS = pg_keeper

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_keeper
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
