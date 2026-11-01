# SPDX-License-Identifier: MIT
#
# lerwattd  - the controller; talks only to the kernel ABI (real or simulated)
# lerwatt-sim - the RK3588 simulator; a separate program that provides that ABI
#
# The two share only include/lerwatt/contract.h (the Lertick contract) and the
# pure control law used by the closed-loop test. No simulator object is ever
# linked into lerwattd.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Werror -Iinclude -Isrc
LDLIBS  += -lm
BUILD   := build

HAL_SRCS := src/hal_fs.c src/hal_discover.c src/hal_sample.c src/hal_actuate.c
CTL_SRCS := src/controller.c
D_SRCS   := $(HAL_SRCS) $(CTL_SRCS) src/contract.c src/lerwattd.c
SIM_SRCS := sim/platform_rk3588.c sim/model.c sim/abi.c sim/load.c src/contract.c sim/lerwatt_sim.c

all: $(BUILD)/lerwattd $(BUILD)/lerwatt-sim

$(BUILD):
	mkdir -p $@

$(BUILD)/lerwattd: $(D_SRCS) $(wildcard include/lerwatt/*.h src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(D_SRCS) $(LDLIBS)

$(BUILD)/lerwatt-sim: $(SIM_SRCS) $(wildcard include/lerwatt/*.h src/*.h sim/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ $(SIM_SRCS) $(LDLIBS)

$(BUILD)/test_model: tests/test_model.c sim/platform_rk3588.c sim/model.c | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ $^ $(LDLIBS)

$(BUILD)/test_closed_loop: tests/test_closed_loop.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ $^ $(LDLIBS)

test: $(BUILD)/test_model $(BUILD)/test_closed_loop
	$(BUILD)/test_model
	$(BUILD)/test_closed_loop

e2e: all
	tests/e2e.sh $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all test e2e clean
