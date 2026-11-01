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
CTL_SRCS := src/controller.c src/learn.c src/forecast.c src/temporal.c
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

$(BUILD)/test_learn: tests/test_learn.c src/learn.c include/lerwatt/learn.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_learn.c src/learn.c $(LDLIBS)

$(BUILD)/test_forecast: tests/test_forecast.c sim/platform_rk3588.c sim/model.c src/learn.c src/forecast.c include/lerwatt/learn.h include/lerwatt/forecast.h sim/sim.h | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ tests/test_forecast.c sim/platform_rk3588.c sim/model.c src/learn.c src/forecast.c $(LDLIBS)

$(BUILD)/test_forecast_feedback: tests/test_forecast_feedback.c src/forecast.c include/lerwatt/forecast.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_forecast_feedback.c src/forecast.c $(LDLIBS)

$(BUILD)/test_controller: tests/test_controller.c $(CTL_SRCS) $(wildcard include/lerwatt/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_controller.c $(CTL_SRCS) $(LDLIBS)

$(BUILD)/test_control_forecast: tests/test_control_forecast.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) $(wildcard include/lerwatt/*.h sim/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ tests/test_control_forecast.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) $(LDLIBS)

$(BUILD)/test_temporal: tests/test_temporal.c src/temporal.c include/lerwatt/temporal.h include/lerwatt/forecast.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_temporal.c src/temporal.c $(LDLIBS)

test: $(BUILD)/test_model $(BUILD)/test_learn $(BUILD)/test_forecast $(BUILD)/test_forecast_feedback $(BUILD)/test_temporal $(BUILD)/test_control_forecast $(BUILD)/test_controller $(BUILD)/test_closed_loop
	$(BUILD)/test_model
	$(BUILD)/test_learn
	$(BUILD)/test_forecast
	$(BUILD)/test_forecast_feedback
	$(BUILD)/test_temporal
	python3 tests/test_runtime_forecast.py
	$(BUILD)/test_control_forecast --temporal --integrated --development
	$(BUILD)/test_controller
	$(BUILD)/test_closed_loop
	$(BUILD)/test_closed_loop --cross-cluster

e2e: all
	tests/e2e.sh $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all test e2e clean
