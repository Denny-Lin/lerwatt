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
ifeq ($(shell uname -s),Linux)
# Older glibc exposes clock_gettime/clock_nanosleep through librt.
LDLIBS  += -lrt
endif
BUILD   := build
PREFIX  ?= /usr/local
DESTDIR ?=
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo source)
CPPFLAGS += -DLERWATT_VERSION=\"$(VERSION)\"

HAL_SRCS := src/hal_fs.c src/hal_discover.c src/hal_sample.c src/hal_actuate.c
CTL_SRCS := src/controller.c src/learn.c src/forecast.c src/temporal.c src/predict.c
D_SRCS   := $(HAL_SRCS) $(CTL_SRCS) src/contract.c src/notify.c src/checkpoint.c src/lerwattd.c
SIM_SRCS := sim/platform_rk3588.c sim/model.c sim/abi.c sim/load.c src/contract.c sim/lerwatt_sim.c

all: $(BUILD)/lerwattd $(BUILD)/lerwatt-sim

$(BUILD):
	mkdir -p $@

$(BUILD)/lerwattd: $(D_SRCS) $(wildcard include/lerwatt/*.h src/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(D_SRCS) $(LDLIBS)

# Native hardware trials need no sched_ext or modern linux/bpf.h. Keep this a
# separate target so a cached BPF-enabled binary cannot be installed by mistake.
$(BUILD)/lerwattd-standalone: $(D_SRCS) $(wildcard include/lerwatt/*.h src/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLW_NO_BPF -o $@ $(D_SRCS) $(LDLIBS)

hardware: $(BUILD)/lerwattd $(BUILD)/lerwattd-standalone

# Optional native Lertick coordination; raw BPF syscalls, no libbpf dependency.
hardware-lertick: $(BUILD)/lerwattd

$(BUILD)/test_runtime_fs: tests/test_runtime_fs.c src/hal_fs.c src/hal_actuate.c $(wildcard include/lerwatt/*.h src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_runtime_fs.c src/hal_fs.c src/hal_actuate.c $(LDLIBS)

$(BUILD)/test_discovery: tests/test_discovery.c src/hal_discover.c src/hal_sample.c src/hal_fs.c $(wildcard include/lerwatt/*.h src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_discovery.c src/hal_discover.c src/hal_sample.c src/hal_fs.c $(LDLIBS)

$(BUILD)/test_notify: tests/test_notify.c src/notify.c src/notify.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_notify.c src/notify.c $(LDLIBS)

$(BUILD)/test_checkpoint: tests/test_checkpoint.c src/checkpoint.c src/checkpoint.h $(wildcard include/lerwatt/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_checkpoint.c src/checkpoint.c $(LDLIBS)

$(BUILD)/test_contract: tests/test_contract.c src/contract.c src/contract.h include/lerwatt/contract.h | $(BUILD)
	$(CC) $(CFLAGS) -DLW_NO_BPF -o $@ tests/test_contract.c src/contract.c $(LDLIBS)

test-deployment: $(BUILD)/lerwattd-standalone $(BUILD)/test_runtime_fs $(BUILD)/test_discovery $(BUILD)/test_notify $(BUILD)/test_checkpoint $(BUILD)/test_contract
	python3 tests/check_integration_abi.py
	$(BUILD)/test_runtime_fs
	$(BUILD)/test_discovery
	$(BUILD)/test_notify
	$(BUILD)/test_checkpoint
	$(BUILD)/test_contract
	python3 tests/test_daemon_deployment.py $(BUILD)/lerwattd-standalone
	sh tests/test_deploy.sh

test-integration-abi:
	python3 tests/check_integration_abi.py

install:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./deploy.sh install

uninstall:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./deploy.sh uninstall

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

$(BUILD)/test_predict: tests/test_predict.c $(CTL_SRCS) $(wildcard include/lerwatt/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_predict.c $(CTL_SRCS) $(LDLIBS)

$(BUILD)/test_cooperative_loop: tests/test_cooperative_loop.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) $(wildcard include/lerwatt/*.h sim/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ tests/test_cooperative_loop.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) $(LDLIBS)

$(BUILD)/test_control_forecast: tests/test_control_forecast.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) $(wildcard include/lerwatt/*.h sim/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -Isim -o $@ tests/test_control_forecast.c sim/platform_rk3588.c sim/model.c $(CTL_SRCS) $(LDLIBS)

$(BUILD)/test_temporal: tests/test_temporal.c src/temporal.c include/lerwatt/temporal.h include/lerwatt/forecast.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_temporal.c src/temporal.c $(LDLIBS)

test: $(BUILD)/test_model $(BUILD)/test_learn $(BUILD)/test_forecast $(BUILD)/test_forecast_feedback $(BUILD)/test_temporal $(BUILD)/test_control_forecast $(BUILD)/test_controller $(BUILD)/test_predict $(BUILD)/test_cooperative_loop $(BUILD)/test_closed_loop
	$(BUILD)/test_model
	$(BUILD)/test_learn
	$(BUILD)/test_forecast
	$(BUILD)/test_forecast_feedback
	$(BUILD)/test_temporal
	python3 tests/test_runtime_forecast.py
	python3 tests/test_runtime_summary.py
	python3 tests/test_cooperative_summary.py
	$(BUILD)/test_control_forecast --temporal --integrated --development
	$(BUILD)/test_controller
	$(BUILD)/test_predict
	$(BUILD)/test_closed_loop
	$(BUILD)/test_closed_loop --cross-cluster
	$(BUILD)/test_closed_loop --cooperative
	$(BUILD)/test_closed_loop --cooperative --cross-cluster
	$(BUILD)/test_cooperative_loop --full

e2e: all
	tests/e2e.sh $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all hardware hardware-lertick install uninstall test test-deployment test-integration-abi e2e clean
