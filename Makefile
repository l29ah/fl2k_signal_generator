CFLAGS += -std=c11 -D_DEFAULT_SOURCE
ifdef DEBUG
	CFLAGS+=-ggdb3 -DDEBUG -Og -Wall -pedantic
else
	CFLAGS+=-DNDEBUG
endif

CFLAGS += $(shell pkg-config libosmo-fl2k --cflags)
LDLIBS := $(shell pkg-config libosmo-fl2k --libs)

EXE = fl2k_signal_generator

all: main-build

main-build: astyle
	$(MAKE) --no-print-directory $(EXE)

.PHONY:	clean astyle

clean:
	rm -rf *.o *.d $(EXE)

astyle:
	astyle --style=linux --indent=tab --unpad-paren --pad-header --pad-oper *.c
