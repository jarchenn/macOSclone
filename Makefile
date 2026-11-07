# Aurora OS — thin Make front end. All logic lives in tools/*.py (stdlib only)
# so the same commands work on Linux, macOS and Windows (GNU make + Python 3).

ifeq ($(OS),Windows_NT)
PYTHON ?= python
else
PYTHON ?= python3
endif

B := $(PYTHON) tools/build.py
MEM ?= 256M
SMP ?= 1

.PHONY: all help deps run run-bios debug test smoke integration storage network smp stress posix desktop soak check clean dist

all:
	$(B) build --variant release

help:
	@$(B) help

deps:
	$(PYTHON) tools/fetch_deps.py

run:
	$(B) run --memory $(MEM) --smp $(SMP)

run-bios:
	$(B) run --memory $(MEM) --bios

debug:
	$(B) debug --memory $(MEM)

test:
	$(B) test

smoke:
	$(B) build --variant release test panic
	$(PYTHON) tools/smoke_test.py

integration:
	$(B) build --variant test
	$(PYTHON) tools/interaction_test.py

storage:
	$(B) build --variant test
	$(PYTHON) tools/storage_test.py

network:
	$(B) build --variant test
	$(PYTHON) tools/network_test.py

smp:
	$(B) build --variant test
	$(PYTHON) tools/smp_test.py

stress:
	$(B) build --variant test
	$(PYTHON) tools/smp_test.py --only stress --stress-seconds 1800

posix:
	$(B) build --variant test
	$(PYTHON) tools/posix_test.py

desktop:
	$(PYTHON) tools/build.py build --variant test
	$(PYTHON) tools/desktop_test.py

soak:
	$(B) build --variant test
	$(PYTHON) tools/interaction_test.py --soak 240

check:
	$(B) check --variant release

clean:
	$(B) clean

dist:
	$(B) dist
