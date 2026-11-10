# A C agent repository, as `granite agents add` builds one: `make O=<folder> agent` leaves the
# runner at <folder>/agent. GraniteC is found through pkg-config alone.
O ?= build
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
GRANITEC_CFLAGS := $(shell $(PKG_CONFIG) --cflags granitec)
GRANITEC_LIBS := $(shell $(PKG_CONFIG) --libs granitec)

.PHONY: agent
agent: $(O)/agent

$(O)/agent: parrot.c
	@mkdir -p $(O)
	$(CC) $(CFLAGS) $(GRANITEC_CFLAGS) -o $@ parrot.c $(GRANITEC_LIBS)
