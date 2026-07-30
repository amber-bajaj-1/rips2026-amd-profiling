SHELL := /bin/bash -o pipefail

-include Makefile.local

ROCM_PATH ?= /opt/rocm
ROCM_LIB_DIR ?= $(if $(wildcard $(ROCM_PATH)/lib),$(ROCM_PATH)/lib,$(ROCM_PATH)/lib64)
ROCTX_INCLUDE_DIR ?= $(ROCM_PATH)/include

HIPCC ?= $(if $(wildcard $(ROCM_PATH)/bin/hipcc),$(ROCM_PATH)/bin/hipcc,hipcc)
ROCPROFV3 ?= $(if $(wildcard $(ROCM_PATH)/bin/rocprofv3),$(ROCM_PATH)/bin/rocprofv3,rocprofv3)
CXX ?= g++

HIP_FLAGS ?= -std=c++17 -O3 -x hip
CXX_FLAGS ?= -std=c++17 -O3
INTERCHANGE_LIBS ?= -lcapnp -lkj -lz
CONTEST_DIR ?= $(abspath ..)
SCHEMA_DIR ?= $(CONTEST_DIR)/fpga-interchange-schema/interchange

PATHFINDER_ENABLE_ROCTX ?= 1
PATHFINDER_ROCTX_FLAGS :=
PATHFINDER_ROCTX_LIBS :=
ifneq ($(PATHFINDER_ENABLE_ROCTX),0)
PATHFINDER_ROCTX_FLAGS += -DPATHFINDER_ENABLE_ROCTX -I$(ROCTX_INCLUDE_DIR)
PATHFINDER_ROCTX_LIBS += -L$(ROCM_LIB_DIR) \
	-Wl,-rpath,$(ROCM_LIB_DIR) -lrocprofiler-sdk-roctx
endif

BENCHMARK ?=
PATHFINDER_ARGS ?=
PROFILE_PATHFINDER_ARGS ?= $(PATHFINDER_ARGS) --delta-telemetry
DEVICE_GRAPH ?= $(CURDIR)/xcvu3p.full-poc-base-wire.devicegraph
INPUT_PHYS ?= $(if $(strip $(BENCHMARK)),$(CONTEST_DIR)/$(BENCHMARK)_unrouted.phys,)
OUTPUT_PHYS ?= $(if $(strip $(BENCHMARK)),$(CONTEST_DIR)/$(BENCHMARK)_PathFinderFile.phys,)
LOGICAL_NETLIST ?= $(if $(strip $(BENCHMARK)),$(CONTEST_DIR)/$(BENCHMARK).netlist,)

PROFILE_ROOT ?= $(CURDIR)/profiling
PROFILE_LABEL ?= $(if $(strip $(BENCHMARK)),$(BENCHMARK),custom)
PROFILE_RUN ?= $(shell date +%Y%m%d-%H%M%S)
PROFILE_OUTPUT_DIR ?= $(PROFILE_ROOT)/$(PROFILE_LABEL)/$(PROFILE_RUN)
PROFILE_OUTPUT_PHYS ?= $(PROFILE_OUTPUT_DIR)/$(PROFILE_LABEL)_PathFinderFile.phys
PROFILE_PREFIX ?= $(ROCPROFV3) --runtime-trace --output-format csv --output-directory $(PROFILE_OUTPUT_DIR) --

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

.PHONY: all router pipeline interchange-tools help run profile clean

all: router

router: PathFinderFile pathfinder

pipeline: router interchange-tools

interchange-tools: interchange_to_csr device_to_routing_graph routes_to_phys

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
	$(CXX) $(CXX_FLAGS) -I"$(SCHEMA_DIR)" \
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
	$(CXX) $(CXX_FLAGS) -I"$(SCHEMA_DIR)" \
		pre-process/device_to_routing_graph.cpp \
		pre-process/device_routing_graph.cpp \
		"$(SCHEMA_DIR)/DeviceResources.capnp.c++" \
		"$(SCHEMA_DIR)/LogicalNetlist.capnp.c++" \
		"$(SCHEMA_DIR)/References.capnp.c++" \
		$(INTERCHANGE_LIBS) -o $@

routes_to_phys: \
		post-process/routes_to_phys.cpp \
		pre-process/gzip_io.hpp \
		pre-process/import_policy.hpp
	$(require_schema_dir)
	$(CXX) $(CXX_FLAGS) -I"$(SCHEMA_DIR)" \
		post-process/routes_to_phys.cpp \
		"$(SCHEMA_DIR)/PhysicalNetlist.capnp.c++" \
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

define execute_pipeline
	env PATHFINDER_PROFILE_COMMAND='$(PROFILE_PREFIX)' \
		./PathFinderFile "$(INPUT_PHYS)" "$(PROFILE_OUTPUT_PHYS)" \
		--logical-netlist "$(LOGICAL_NETLIST)" \
		--device-graph "$(DEVICE_GRAPH)" \
		$(PROFILE_PATHFINDER_ARGS)
endef

run: pipeline
	$(require_run_inputs)
	$(require_regular_output)
	env PATHFINDER_PROFILE_COMMAND='' \
		./PathFinderFile "$(INPUT_PHYS)" "$(OUTPUT_PHYS)" \
		--logical-netlist "$(LOGICAL_NETLIST)" \
		--device-graph "$(DEVICE_GRAPH)" \
		$(PATHFINDER_ARGS)

profile: pipeline
	$(require_run_inputs)
	@command -v "$(ROCPROFV3)" >/dev/null 2>&1 || \
		{ echo "rocprofv3 is unavailable at $(ROCPROFV3); run ./setup-tpe.sh first."; exit 2; }
	@mkdir -p "$(PROFILE_OUTPUT_DIR)" "$(dir $(PROFILE_OUTPUT_PHYS))"
	@echo "Profiling output: $(PROFILE_OUTPUT_DIR)"
	@$(execute_pipeline) 2>&1 | tee "$(PROFILE_OUTPUT_DIR)/pathfinder-wrapper.log"

help:
	@echo "Build the Delta-Stepping router:"
	@echo "  make"
	@echo
	@echo "Build the full conversion/routing/reconstruction pipeline:"
	@echo "  make pipeline"
	@echo
	@echo "Run a contest benchmark without profiling:"
	@echo "  make run BENCHMARK=logicnets_jscl"
	@echo
	@echo "Run the same benchmark with rocprofv3 profiling:"
	@echo "  make profile BENCHMARK=logicnets_jscl"
	@echo
	@echo "The profiler traces and wrapper log are written below:"
	@echo "  $(PROFILE_ROOT)/<benchmark>/<timestamp>/"
	@echo "The profiled routed .phys file is written in the same run directory."
	@echo
	@echo "For a benchmark outside the contest naming convention, set:"
	@echo "  INPUT_PHYS=... LOGICAL_NETLIST=... OUTPUT_PHYS=..."

clean:
	rm -f PathFinderFile pathfinder interchange_to_csr \
		device_to_routing_graph routes_to_phys
