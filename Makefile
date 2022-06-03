PGFILEDESC = "clientcomptage"
PGAPPICON = win32

PROGRAMS = clientcomptage

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)
SCRIPTS_built = clientcomptage
EXTRA_CLEAN = rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS))

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

all: $(PROGRAMS)

%: %.o $(WIN32RES)
	   $(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) -lpgfeutils -lpgcommon -lm -o $@$(X)

clientcomptage: clientcomptage.o

