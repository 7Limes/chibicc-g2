CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

chibicc-g2: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): chibicc.h

# The original x86 test suite (test/) can't run against the g2 backend.
# It is kept as a reference for porting tests to the g2 suite.

clean:
	rm -rf chibicc-g2 tmp* *.g2asm
	find * -type f '(' -name '*~' -o -name '*.o' ')' -exec rm {} ';'

.PHONY: clean
