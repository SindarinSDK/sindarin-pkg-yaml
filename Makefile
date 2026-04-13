# Sindarin YAML Package

.PHONY: setup test

%.sn: %.sn.c
	@:

ifeq ($(OS),Windows_NT)
    EXE_EXT := .exe
else
    EXE_EXT :=
endif

BIN_DIR      := bin
SN           ?= sn
SRC_SOURCES  := $(wildcard src/*.sn)
RUN_TESTS_SN := .sn/sindarin-pkg-test/src/execute.sn
RUN_TESTS    := $(BIN_DIR)/run_tests$(EXE_EXT)

setup:
	@$(SN) --install

test: setup $(RUN_TESTS)
	@$(RUN_TESTS) --verbose

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(RUN_TESTS): $(RUN_TESTS_SN) $(SRC_SOURCES) | $(BIN_DIR)
	@$(SN) $(RUN_TESTS_SN) -o $@ -l 1
