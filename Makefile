CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch

BUILD=build
SRCS=$(wildcard *.c)
OBJS=$(SRCS:%.c=$(BUILD)/%.o)

$(BUILD)/chibicc-g2: $(OBJS) | $(BUILD)/include
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD)/%.o: %.c chibicc.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# The compiler searches <exe dir>/include for the g2 headers.
$(BUILD)/include: | $(BUILD)
	ln -s ../include $@

# Compiles each test in test-g2/, assembles it with the g2 assembler (g2a)
# and runs it in a headless VM (tools/g2run.py).
test: $(BUILD)/chibicc-g2
	python3 tools/run_tests.py

# The original x86 test suite (test/) can't run against the g2 backend.
# It is kept as a reference for porting tests to the g2 suite.

clean:
	rm -rf $(BUILD) tmp*
	find * -type f -name '*~' -exec rm {} ';'

.PHONY: test clean
