# Simple native build: CMake + Ninja into build/

.DEFAULT_GOAL := help

BUILD_DIR := build
BUILD_TYPE ?= Release
JOBS := $(shell nproc)

.PHONY: all build test clean version help

all: build ## Default build

build: ## Build with CMake/Ninja (configures automatically)
	cmake -S . -B $(BUILD_DIR) -GNinja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DBUILD_TESTS=ON
	cmake --build $(BUILD_DIR) --parallel $(JOBS)

test: build ## Build and run this repo's unit tests
	ctest --test-dir $(BUILD_DIR) --output-on-failure -R "serializer_tests|runner_tests|subsystem_tests|spool_tests"

clean: ## Remove the build directory
	rm -rf $(BUILD_DIR)

version: ## Print the project version
	@echo "0.1.0"

help: ## Show this help
	@printf '\n  \033[1m%s\033[0m\n' 'Acadia-Client'
	@printf '  Usage: make <target> [VAR=value ...]\n'
	@printf '\n'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z0-9_.\/-]+:.*?## / { printf "    \033[36m%-24s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@printf '\n'