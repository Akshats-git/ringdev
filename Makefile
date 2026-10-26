# Out-of-tree Kbuild Makefile. `make` builds the module, `make tests` the user programs.
obj-m := ringdev.o
ccflags-y := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

KDIR ?= /lib/modules/$(shell uname -r)/build
CFLAGS ?= -O2 -g
UFLAGS := $(CFLAGS) -Wall -Wextra -pthread -I.

TESTS := $(patsubst tests/%.c,bin/%,$(wildcard tests/*.c))
USER  := $(patsubst user/%.c,bin/%,$(wildcard user/*.c))

all: module tests

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

tests: $(TESTS) $(USER)

bin/%: tests/%.c ringdev_uapi.h user/libringdev.h | bin
	$(CC) $(UFLAGS) -o $@ $<

bin/%: user/%.c ringdev_uapi.h user/libringdev.h | bin
	$(CC) $(UFLAGS) -o $@ $<

bin:
	mkdir -p bin

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf bin

.PHONY: all module tests clean
