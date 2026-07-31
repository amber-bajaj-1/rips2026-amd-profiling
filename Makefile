SHELL := /bin/bash -o pipefail

-include Makefile.local

ROCM_PATH ?= /opt/rocm
ROCM_LIB_DIR ?= $(if $(wildcard $(ROCM_PATH)/lib),$(ROCM_PATH)/lib,$(ROCM_PATH)/lib64)
ROCTX_INCLUDE_DIR ?= $(ROCM_PATH)/include
ROCTX_STYLE ?= sdk
ROCTX_LIBRARY ?= $(ROCM_LIB_DIR)/librocprofiler-sdk-roctx.so

HIPCC ?= $(if $(wildcard $(ROCM_PATH)/bin/hipcc),$(ROCM_PATH)/bin/hipcc,hipcc)
ROCPROFV3 ?= $(if $(wildcard $(ROCM_PATH)/bin/rocprofv3),$(ROCM_PATH)/bin/rocprofv3,rocprofv3)
CXX ?= g++

HIP_FLAGS ?= -std=c++17 -O3 -x hip
CXX_FLAGS ?= -std=c++17 -O3
INTERCHANGE_CPPFLAGS ?=
INTERCHANGE_LIBS ?= -lcapnp -lkj -lz
RIPS_ROOT ?= $(abspath ..)
BENCHMARK_DIR ?= $(CURDIR)/benchmarks
SCHEMA_DIR ?= $(CURDIR)/dependencies/fpga-interchange-schema/interchange

PATHFINDER_ENABLE_ROCTX ?= $(if $(filter none,$(ROCTX_STYLE)),0,1)
PATHFINDER_ROCTX_FLAGS :=
PATHFINDER_ROCTX_LIBS :=
ifneq ($(PATHFINDER_ENABLE_ROCTX),0)
PATHFINDER_ROCTX_FLAGS += -DPATHFINDER_ENABLE_ROCTX -I$(ROCTX_INCLUDE_DIR)
ifeq ($(ROCTX_STYLE),legacy)
PATHFINDER_ROCTX_FLAGS += -DPATHFINDER_USE_LEGACY_ROCTX
endif
PATHFINDER_ROCTX_LIBS += $(ROCTX_LIBRARY) \
	-Wl,-rpath,$(dir $(ROCTX_LIBRARY))
endif

BENCHMARK ?=
# This matches PathfinderOptions::delta. A --delta flag is emitted only when
# the caller overrides the built-in value.
DELTA ?= 1
PATHFINDER_ARGS ?=
DELTA_ARG = $(if $(strip $(DELTA)),$(if $(filter 1,$(strip $(DELTA))),,--delta $(DELTA)))
RUN_PATHFINDER_ARGS = $(DELTA_ARG) $(PATHFINDER_ARGS)
PROFILE_PATHFINDER_ARGS = $(RUN_PATHFINDER_ARGS) --delta-telemetry
DEVICE_FILE ?= $(BENCHMARK_DIR)/xcvu3p.device
DEVICE_GRAPH ?= $(BENCHMARK_DIR)/xcvu3p.full-poc-base-wire.devicegraph
REBUILD_DEVICE_GRAPH ?= 0
INPUT_PHYS ?= $(if $(strip $(BENCHMARK)),$(BENCHMARK_DIR)/$(BENCHMARK)_unrouted.phys,)
OUTPUT_PHYS ?= $(if $(strip $(BENCHMARK)),$(BENCHMARK_DIR)/$(BENCHMARK)_PathFinderFile.phys,)
LOGICAL_NETLIST ?= $(if $(strip $(BENCHMARK)),$(BENCHMARK_DIR)/$(BENCHMARK).netlist,)

PROFILE_ROOT ?= $(CURDIR)/profiling
PROFILE_LABEL ?= $(if $(strip $(BENCHMARK)),$(BENCHMARK),custom)
ifndef PROFILE_RUN
PROFILE_RUN := $(shell date +%Y%m%d-%H%M%S)
endif
PROFILE_OUTPUT_DIR ?= $(PROFILE_ROOT)/$(PROFILE_LABEL)/$(PROFILE_RUN)
PROFILE_RUNTIME_DIR ?= $(PROFILE_OUTPUT_DIR)/runtime
PROFILE_RUNTIME_DATA_DIR ?= $(PROFILE_RUNTIME_DIR)/rocprofv3
PROFILE_COUNTER_DIR ?= $(PROFILE_OUTPUT_DIR)/counters
PROFILE_OUTPUT_PHYS ?= $(PROFILE_RUNTIME_DIR)/$(PROFILE_LABEL)_PathFinderFile.phys
PROFILE_COUNTER_OUTPUT_PHYS ?= $(PROFILE_COUNTER_DIR)/$(PROFILE_LABEL)_PathFinderFile.phys
PROFILE_PREFIX ?= $(ROCPROFV3) --runtime-trace --stats --output-format csv --output-directory $(PROFILE_RUNTIME_DATA_DIR) --
COUNTER_INPUT ?= $(CURDIR)/profiling-config/gfx115x-pmcs.yaml
COUNTER_VALIDATOR ?= $(CURDIR)/profiling-config/validate_counter_output.py
PROFILE_COUNTER_DATA_DIR ?= $(PROFILE_COUNTER_DIR)/rocprofv3-pmc
COUNTER_PROFILE_PREFIX ?= $(ROCPROFV3) --input $(COUNTER_INPUT) --output-format csv --output-directory $(PROFILE_COUNTER_DATA_DIR) --

DELTA_SOURCES := \
	delta_stepping/delta_stepping.cpp
DELTA_HEADERS := \
	$(wildcard delta_stepping/*.hpp)
ROUTING_SOURCES := \
	routing/pathfinder.cpp
ROUTING_HEADERS := \
	routing/pathfinder.hpp \
	pre-process/import_policy.hpp
PREPROCESS_HEADERS := \
	pre-process/device_routing_graph.hpp \
	pre-process/gzip_io.hpp \
	pre-process/import_policy.hpp

.PHONY: all router pipeline interchange-tools device-graph help run \
	profile profile-counters profile-diagnostics profile-all clean

.NOTPARALLEL: profile-all

all: router

router: PathFinderFile pathfinder

pipeline: router interchange-tools

interchange-tools: interchange_to_csr device_to_routing_graph routes_to_phys

device-graph: device_to_routing_graph
	@test -s "$(DEVICE_FILE)" || \
		{ echo "Device file not found: $(DEVICE_FILE)"; exit 2; }
	@if [[ ! -s "$(DEVICE_GRAPH)" || "$(REBUILD_DEVICE_GRAPH)" == "1" ]]; then \
		echo "Generating device graph: $(DEVICE_GRAPH)"; \
		mkdir -p "$(dir $(DEVICE_GRAPH))"; \
		./device_to_routing_graph "$(DEVICE_FILE)" "$(DEVICE_GRAPH)" --full-device; \
	else \
		echo "Using device graph: $(DEVICE_GRAPH)"; \
	fi

PathFinderFile: routing/pathfinder_router.cpp
	$(CXX) $(CXX_FLAGS) $< -o $@

pathfinder: $(ROUTING_SOURCES) $(ROUTING_HEADERS) $(DELTA_SOURCES) $(DELTA_HEADERS)
	$(HIPCC) $(HIP_FLAGS) $(PATHFINDER_ROCTX_FLAGS) \
		$(ROUTING_SOURCES) $(DELTA_SOURCES) \
		-pthread $(PATHFINDER_ROCTX_LIBS) -o $@

define require_schema_dir
	@test -f "$(SCHEMA_DIR)/PhysicalNetlist.capnp.h" || \
		{ echo "Generated FPGA Interchange schemas were not found in SCHEMA_DIR=$(SCHEMA_DIR)"; exit 2; }
endef

interchange_to_csr: \
		pre-process/interchange_to_csr.cpp \
		pre-process/device_routing_graph.cpp \
		$(PREPROCESS_HEADERS)
	$(require_schema_dir)
	$(CXX) $(CXX_FLAGS) $(INTERCHANGE_CPPFLAGS) -I"$(SCHEMA_DIR)" \
		pre-process/interchange_to_csr.cpp \
		pre-process/device_routing_graph.cpp \
		"$(SCHEMA_DIR)/PhysicalNetlist.capnp.c++" \
		"$(SCHEMA_DIR)/LogicalNetlist.capnp.c++" \
		"$(SCHEMA_DIR)/References.capnp.c++" \
		$(INTERCHANGE_LIBS) -o $@

device_to_routing_graph: \
		pre-process/device_to_routing_graph.cpp \
		pre-process/device_routing_graph.cpp \
		$(PREPROCESS_HEADERS)
	$(require_schema_dir)
	$(CXX) $(CXX_FLAGS) $(INTERCHANGE_CPPFLAGS) -I"$(SCHEMA_DIR)" \
		pre-process/device_to_routing_graph.cpp \
		pre-process/device_routing_graph.cpp \
		"$(SCHEMA_DIR)/DeviceResources.capnp.c++" \
		"$(SCHEMA_DIR)/LogicalNetlist.capnp.c++" \
		"$(SCHEMA_DIR)/References.capnp.c++" \
		$(INTERCHANGE_LIBS) -o $@

routes_to_phys: \
		post-process/routes_to_phys.cpp \
		pre-process/gzip_io.hpp \
		pre-process/import_policy.hpp \
		$(SCHEMA_DIR)/PhysicalNetlist.capnp.c++ \
		$(SCHEMA_DIR)/References.capnp.c++
	$(require_schema_dir)
	$(CXX) $(CXX_FLAGS) $(INTERCHANGE_CPPFLAGS) -I"$(SCHEMA_DIR)" \
		post-process/routes_to_phys.cpp \
		"$(SCHEMA_DIR)/PhysicalNetlist.capnp.c++" \
		"$(SCHEMA_DIR)/References.capnp.c++" \
		$(INTERCHANGE_LIBS) -o $@

define require_run_inputs
	@test -n "$(strip $(INPUT_PHYS))" || \
		{ echo "Set BENCHMARK=<name>, or set INPUT_PHYS and LOGICAL_NETLIST explicitly."; exit 2; }
	@test -n "$(strip $(LOGICAL_NETLIST))" || \
		{ echo "LOGICAL_NETLIST is required"; exit 2; }
	@test -f "$(INPUT_PHYS)" || \
		{ echo "Input physical netlist not found: $(INPUT_PHYS)"; exit 2; }
	@test -f "$(LOGICAL_NETLIST)" || \
		{ echo "Logical netlist not found: $(LOGICAL_NETLIST)"; exit 2; }
	@test -f "$(DEVICE_GRAPH)" || \
		{ echo "Device routing graph not found: $(DEVICE_GRAPH)"; exit 2; }
endef

define require_regular_output
	@test -n "$(strip $(OUTPUT_PHYS))" || \
		{ echo "OUTPUT_PHYS is required"; exit 2; }
	@mkdir -p "$(dir $(OUTPUT_PHYS))"
endef

define execute_profile_pipeline
	env PATHFINDER_PROFILE_COMMAND='$(1)' \
		./PathFinderFile "$(INPUT_PHYS)" "$(2)" \
		--logical-netlist "$(LOGICAL_NETLIST)" \
		--device-graph "$(DEVICE_GRAPH)" $(PROFILE_PATHFINDER_ARGS)
endef

run: pipeline device-graph
	$(require_run_inputs)
	$(require_regular_output)
	env PATHFINDER_PROFILE_COMMAND='' \
		./PathFinderFile "$(INPUT_PHYS)" "$(OUTPUT_PHYS)" \
		--logical-netlist "$(LOGICAL_NETLIST)" \
		--device-graph "$(DEVICE_GRAPH)" $(RUN_PATHFINDER_ARGS)

profile: pipeline device-graph
	$(require_run_inputs)
	@command -v "$(ROCPROFV3)" >/dev/null 2>&1 || \
		{ echo "rocprofv3 is unavailable at $(ROCPROFV3); run ./setup-tpe.sh first."; exit 2; }
	@mkdir -p "$(PROFILE_RUNTIME_DATA_DIR)" "$(dir $(PROFILE_OUTPUT_PHYS))"
	@echo "Runtime profiling output: $(PROFILE_RUNTIME_DIR)"
	@$(call execute_profile_pipeline,$(PROFILE_PREFIX),$(PROFILE_OUTPUT_PHYS)) \
		2>&1 | tee "$(PROFILE_RUNTIME_DIR)/pathfinder-wrapper.log"

profile-counters: pipeline device-graph
	$(require_run_inputs)
	@command -v "$(ROCPROFV3)" >/dev/null 2>&1 || \
		{ echo "rocprofv3 is unavailable at $(ROCPROFV3); run ./setup-tpe.sh first."; exit 2; }
	@test -f "$(COUNTER_INPUT)" || \
		{ echo "Hardware-counter input file not found: $(COUNTER_INPUT)"; exit 2; }
	@test -f "$(COUNTER_VALIDATOR)" || \
		{ echo "Counter validator not found: $(COUNTER_VALIDATOR)"; exit 2; }
	@mkdir -p "$(PROFILE_COUNTER_DIR)" "$(dir $(PROFILE_COUNTER_OUTPUT_PHYS))"
	@echo "Hardware-counter profiling output: $(PROFILE_COUNTER_DIR)"
	@echo "Counter configuration: $(COUNTER_INPUT)"
	@$(call execute_profile_pipeline,$(COUNTER_PROFILE_PREFIX),$(PROFILE_COUNTER_OUTPUT_PHYS)) \
		2>&1 | tee "$(PROFILE_COUNTER_DIR)/pathfinder-wrapper.log"
	@test -d "$(PROFILE_COUNTER_DATA_DIR)" || \
		{ echo "rocprofv3 did not create $(PROFILE_COUNTER_DATA_DIR)"; exit 2; }
	@python3 "$(COUNTER_VALIDATOR)" "$(PROFILE_COUNTER_DATA_DIR)"
	@echo "Raw and derived PMC data: $(PROFILE_COUNTER_DATA_DIR)"

profile-diagnostics: profile-counters
	@echo "Focused hot-kernel diagnostics are complete."

profile-all: profile profile-counters
	@echo "Combined profiling output: $(PROFILE_OUTPUT_DIR)"

help:
	@echo "Build the Delta-Stepping router:"
	@echo "  make"
	@echo
	@echo "Build the full conversion/routing/reconstruction pipeline:"
	@echo "  make pipeline"
	@echo
	@echo "Run a bundled benchmark without profiling:"
	@echo "  make run BENCHMARK=logicnets_jscl"
	@echo "  Delta defaults to 1; use DELTA=auto or DELTA=<positive-number> to override it."
	@echo
	@echo "Collect the runtime trace and timing statistics:"
	@echo "  make profile BENCHMARK=logicnets_jscl"
	@echo
	@echo "Collect focused gfx115x hot-kernel counters with rocprofv3:"
	@echo "  make profile-counters BENCHMARK=logicnets_jscl"
	@echo
	@echo "Alias for the focused hot-kernel counter run:"
	@echo "  make profile-diagnostics BENCHMARK=logicnets_jscl"
	@echo
	@echo "Collect the runtime trace and diagnostics sequentially:"
	@echo "  make profile-all BENCHMARK=logicnets_jscl"
	@echo
	@echo "All profiling output is written below:"
	@echo "  $(PROFILE_ROOT)/<benchmark>/<timestamp>/"
	@echo
	@echo "For a benchmark outside the bundled naming convention:"
	@echo "  make run INPUT_PHYS=... LOGICAL_NETLIST=... OUTPUT_PHYS=..."
	@echo "  make profile-all INPUT_PHYS=... LOGICAL_NETLIST=... PROFILE_LABEL=..."

clean:
	rm -f PathFinderFile pathfinder interchange_to_csr \
		device_to_routing_graph routes_to_phys
