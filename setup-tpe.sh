#!/usr/bin/env bash
set -Eeuo pipefail

# Prepare an AMD University Program cloud instance for building and profiling
# the Delta-Stepping PathFinder pipeline. This script intentionally stops after
# downloading assets, generating the device graph, and compiling all binaries.

readonly PROJECT_NAME="rips2026-amd-profiling"
readonly PROJECT_REPO_URL="${PROJECT_REPO_URL:-https://github.com/amber-bajaj-1/rips2026-amd-profiling.git}"
readonly CONTEST_REPO_URL="${CONTEST_REPO_URL:-https://github.com/Xilinx/fpga24_routing_contest.git}"
readonly WORKSPACE_ROOT="${AUP_WORKSPACE_ROOT:-$HOME}"
readonly CONTEST_DIR="${CONTEST_DIR:-$WORKSPACE_ROOT/fpga24_routing_contest}"
readonly PROJECT_DIR="$CONTEST_DIR/$PROJECT_NAME"
readonly LOCAL_PREFIX="${LOCAL_PREFIX:-$HOME/.local/rips2026-amd-profiling}"
readonly CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/rips2026-amd-profiling"
readonly JAVA_HOME_SETUP="${RIPS_JAVA_HOME:-$HOME/.local/opt/temurin-21}"
readonly CAPNP_VERSION="${CAPNP_VERSION:-1.4.0}"
readonly DEVICE_NAME="xcvu3p"
readonly DEVICE_FILE="$CONTEST_DIR/$DEVICE_NAME.device"
readonly DEVICE_GRAPH="$PROJECT_DIR/$DEVICE_NAME.full-poc-base-wire.devicegraph"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

APT_UPDATED=0
PRIVILEGE_COMMAND=()

log() {
  printf '\n[%s] %s\n' "$PROJECT_NAME" "$*"
}

die() {
  printf '\n[%s] ERROR: %s\n' "$PROJECT_NAME" "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 ||
    die "Required command '$1' is unavailable."
}

validate_managed_paths() {
  local path
  for path in "$LOCAL_PREFIX" "$CACHE_DIR" "$JAVA_HOME_SETUP"; do
    [[ "$path" == "$HOME/"* && "$path" != *"/../"* ]] ||
      die "Managed setup path must be a direct descendant of HOME: $path"
  done
}

download() {
  local url="$1"
  local output="$2"
  curl --fail --location --retry 3 --retry-delay 2 \
    --output "$output" "$url"
}

configure_privilege_command() {
  if [[ "$(id -u)" -eq 0 ]]; then
    PRIVILEGE_COMMAND=()
  elif command -v sudo >/dev/null 2>&1; then
    PRIVILEGE_COMMAND=(sudo)
  fi
}

apt_update_once() {
  if ((APT_UPDATED == 0)); then
    "${PRIVILEGE_COMMAND[@]}" apt-get update
    APT_UPDATED=1
  fi
}

install_base_packages() {
  if ! command -v apt-get >/dev/null 2>&1; then
    log "apt-get is unavailable; validating preinstalled build tools instead."
    return
  fi
  if [[ "$(id -u)" -ne 0 && ${#PRIVILEGE_COMMAND[@]} -eq 0 ]]; then
    log "No root or sudo access; validating preinstalled build tools instead."
    return
  fi

  log "Installing system build dependencies"
  apt_update_once
  "${PRIVILEGE_COMMAND[@]}" apt-get install -y \
    build-essential \
    ca-certificates \
    curl \
    git \
    make \
    perl \
    pkg-config \
    python3 \
    python3-pip \
    tar \
    time \
    unzip \
    wget \
    zlib1g-dev
}

install_java_21() {
  if [[ -x "$JAVA_HOME_SETUP/bin/java" ]] &&
     "$JAVA_HOME_SETUP/bin/java" -version 2>&1 |
       grep -Eq 'version "21[.]|openjdk version "21[.]'; then
    log "Java 21 is already installed at $JAVA_HOME_SETUP"
    return
  fi

  local java_arch
  case "$(uname -m)" in
    x86_64 | amd64) java_arch="x64" ;;
    aarch64 | arm64) java_arch="aarch64" ;;
    *) die "Unsupported Java download architecture: $(uname -m)" ;;
  esac

  log "Installing a local Java 21 runtime"
  mkdir -p "$CACHE_DIR" "$(dirname "$JAVA_HOME_SETUP")"
  local archive="$CACHE_DIR/temurin-21-linux-$java_arch.tar.gz"
  local extract_dir
  extract_dir="$(mktemp -d "$CACHE_DIR/java-21.XXXXXX")"
  download \
    "https://api.adoptium.net/v3/binary/latest/21/ga/linux/$java_arch/jdk/hotspot/normal/eclipse" \
    "$archive"
  tar -xzf "$archive" -C "$extract_dir"

  local extracted_jdk
  extracted_jdk="$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -print -quit)"
  [[ -n "$extracted_jdk" ]] || die "The Java archive did not contain a JDK."
  rm -rf -- "$JAVA_HOME_SETUP"
  mv -- "$extracted_jdk" "$JAVA_HOME_SETUP"
  rm -rf -- "$extract_dir"
}

install_capnproto() {
  if [[ -x "$LOCAL_PREFIX/bin/capnp" ]] &&
     "$LOCAL_PREFIX/bin/capnp" --version 2>&1 |
       grep -Fq "Cap'n Proto version $CAPNP_VERSION"; then
    log "Cap'n Proto $CAPNP_VERSION is already installed"
    return
  fi

  log "Building Cap'n Proto $CAPNP_VERSION locally"
  mkdir -p "$CACHE_DIR" "$LOCAL_PREFIX/src"
  local archive="$CACHE_DIR/capnproto-c++-$CAPNP_VERSION.tar.gz"
  local source_dir="$LOCAL_PREFIX/src/capnproto-c++-$CAPNP_VERSION"
  download \
    "https://capnproto.org/capnproto-c++-$CAPNP_VERSION.tar.gz" \
    "$archive"
  rm -rf -- "$source_dir"
  tar -xzf "$archive" -C "$LOCAL_PREFIX/src"
  (
    cd "$source_dir"
    ./configure --prefix="$LOCAL_PREFIX"
    make -j"$JOBS"
    make install
  )
}

configure_rocm_environment() {
  local hipcc_path
  hipcc_path="$(command -v hipcc || true)"

  if [[ -n "${ROCM_PATH:-}" ]]; then
    ROCM_ROOT="$ROCM_PATH"
  elif [[ -n "$hipcc_path" ]]; then
    ROCM_ROOT="$(cd -- "$(dirname -- "$hipcc_path")/.." && pwd -P)"
  elif [[ -x /opt/rocm/bin/hipcc ]]; then
    ROCM_ROOT="/opt/rocm"
  else
    die "hipcc was not found. Use an AUP image with HIP/ROCm installed."
  fi
  export ROCM_ROOT
  export ROCM_PATH="$ROCM_ROOT"
  export PATH="$ROCM_ROOT/bin:$PATH"
  export LD_LIBRARY_PATH="$ROCM_ROOT/lib:$ROCM_ROOT/lib64:${LD_LIBRARY_PATH:-}"
}

install_profiler_if_needed() {
  local existing_header
  local existing_library
  existing_header="$(
    find "$ROCM_ROOT/include" /usr/include \
      -path '*/rocprofiler-sdk-roctx/roctx.h' -print -quit 2>/dev/null ||
      true
  )"
  existing_library="$(
    find "$ROCM_ROOT/lib" "$ROCM_ROOT/lib64" /usr/lib /usr/lib64 \
      -name 'librocprofiler-sdk-roctx.so' -print -quit 2>/dev/null ||
      true
  )"
  if command -v rocprofv3 >/dev/null 2>&1 &&
     [[ -n "$existing_header" && -n "$existing_library" ]]; then
    log "rocprofv3 and the ROCTx development files are already installed"
    return
  fi

  if ! command -v apt-get >/dev/null 2>&1 ||
     [[ "$(id -u)" -ne 0 && ${#PRIVILEGE_COMMAND[@]} -eq 0 ]]; then
    die "rocprofv3 is missing and cannot be installed without apt and root/sudo access."
  fi

  log "Installing ROCprofiler-SDK and rocprofv3"
  apt_update_once
  if apt-cache show rocprofiler-sdk >/dev/null 2>&1; then
    "${PRIVILEGE_COMMAND[@]}" apt-get install -y rocprofiler-sdk
  elif apt-cache show amdrocm-profiler-base >/dev/null 2>&1; then
    "${PRIVILEGE_COMMAND[@]}" apt-get install -y amdrocm-profiler-base
  else
    die "No ROCprofiler-SDK package is available from the configured ROCm repositories."
  fi

  export PATH="$ROCM_ROOT/bin:$PATH"
  command -v rocprofv3 >/dev/null 2>&1 ||
    die "The profiler package installed, but rocprofv3 is still unavailable."
}

locate_roctx_installation() {
  local header
  local library
  header="$(
    find "$ROCM_ROOT/include" /usr/include \
      -path '*/rocprofiler-sdk-roctx/roctx.h' -print -quit 2>/dev/null ||
      true
  )"
  library="$(
    find "$ROCM_ROOT/lib" "$ROCM_ROOT/lib64" /usr/lib /usr/lib64 \
      -name 'librocprofiler-sdk-roctx.so' -print -quit 2>/dev/null ||
      true
  )"
  [[ -n "$header" ]] ||
    die "ROCTx header rocprofiler-sdk-roctx/roctx.h was not found."
  [[ -n "$library" ]] ||
    die "ROCTx development library librocprofiler-sdk-roctx.so was not found."
  ROCM_INCLUDE_DIR="$(dirname "$(dirname "$header")")"
  ROCM_LIBRARY_DIR="$(dirname "$library")"
  export ROCM_INCLUDE_DIR ROCM_LIBRARY_DIR
}

persist_environment() {
  local environment_dir="$HOME/.config/$PROJECT_NAME"
  local environment_file="$environment_dir/environment.sh"
  mkdir -p "$environment_dir"
  cat >"$environment_file" <<EOF
export JAVA_HOME="$JAVA_HOME_SETUP"
export LOCAL_PREFIX="$LOCAL_PREFIX"
export ROCM_PATH="$ROCM_ROOT"
export PATH="$HOME/.local/bin:\$JAVA_HOME/bin:\$LOCAL_PREFIX/bin:\$ROCM_PATH/bin:\$PATH"
export LD_LIBRARY_PATH="$LOCAL_PREFIX/lib:$ROCM_LIBRARY_DIR:\${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
export RIPS_CONTEST_DIR="$CONTEST_DIR"
export RIPS_PROFILING_DIR="$PROJECT_DIR"
export RIPS_SCHEMA_DIR="$CONTEST_DIR/fpga-interchange-schema/interchange"
EOF

  touch "$HOME/.bashrc"
  local source_line="source \"$environment_file\""
  grep -qxF "$source_line" "$HOME/.bashrc" ||
    printf '%s\n' "$source_line" >>"$HOME/.bashrc"
}

prepare_contest_repository() {
  if [[ ! -d "$CONTEST_DIR/.git" ]]; then
    log "Cloning the FPGA'24 routing contest repository"
    git clone --recurse-submodules "$CONTEST_REPO_URL" "$CONTEST_DIR"
  else
    log "Using the existing contest repository at $CONTEST_DIR"
    git -C "$CONTEST_DIR" submodule update --init --recursive
  fi
}

prepare_profiling_repository() {
  if [[ "$SCRIPT_DIR" == "$PROJECT_DIR" ]]; then
    log "Profiling repository is already inside the contest repository"
  elif [[ -e "$PROJECT_DIR" ]]; then
    log "Using the existing profiling repository at $PROJECT_DIR"
  elif [[ -f "$SCRIPT_DIR/Makefile" &&
          -d "$SCRIPT_DIR/delta_stepping" &&
          -d "$SCRIPT_DIR/routing" ]]; then
    log "Copying the current profiling working tree into the contest repository"
    cp -a -- "$SCRIPT_DIR" "$PROJECT_DIR"
  else
    log "Cloning the profiling repository into the contest repository"
    git clone "$PROJECT_REPO_URL" "$PROJECT_DIR"
  fi

  mkdir -p "$PROJECT_DIR/profiling"
}

download_benchmarks() {
  log "Preparing contest dependencies and downloading all benchmarks"
  make -C "$CONTEST_DIR" setup

  local benchmarks=(
    logicnets_jscl
    boom_med_pb
    vtr_mcml
    rosetta_fd
    corundum_25g
    finn_radioml
    vtr_lu64peeng
    corescore_500
    corescore_500_pb
    mlcad_d181_lefttwo3rds
    koios_dla_like_large
    boom_soc
    ispd16_example2
  )
  local benchmark
  for benchmark in "${benchmarks[@]}"; do
    [[ -s "$CONTEST_DIR/${benchmark}_unrouted.phys" ]] ||
      die "Missing benchmark physical netlist: ${benchmark}_unrouted.phys"
    [[ -s "$CONTEST_DIR/${benchmark}.netlist" ]] ||
      die "Missing benchmark logical netlist: ${benchmark}.netlist"
  done
}

prepare_device() {
  if [[ ! -s "$DEVICE_FILE" ]]; then
    local supplied_device="${AUP_DEVICE_FILE:-$WORKSPACE_ROOT/$DEVICE_NAME.device}"
    if [[ -s "$supplied_device" ]]; then
      log "Copying the supplied $DEVICE_NAME device"
      cp -- "$supplied_device" "$DEVICE_FILE"
    else
      log "Generating $DEVICE_NAME.device through RapidWright"
      make -C "$CONTEST_DIR" "$DEVICE_NAME.device"
    fi
  else
    log "Using the existing device at $DEVICE_FILE"
  fi
  [[ -s "$DEVICE_FILE" ]] || die "Failed to prepare $DEVICE_FILE"
}

generate_cpp_schemas() {
  local schema_dir="$CONTEST_DIR/fpga-interchange-schema/interchange"
  local java_schema="$schema_dir/capnp/java.capnp"
  log "Generating FPGA Interchange C++ schemas"
  mkdir -p "$(dirname "$java_schema")"
  if [[ ! -s "$java_schema" ]]; then
    download \
      "https://raw.githubusercontent.com/capnproto/capnproto-java/master/compiler/src/main/schema/capnp/java.capnp" \
      "$java_schema"
  fi
  (
    cd "$schema_dir"
    "$LOCAL_PREFIX/bin/capnp" compile -oc++ -I . \
      References.capnp \
      DeviceResources.capnp \
      LogicalNetlist.capnp \
      PhysicalNetlist.capnp
  )
  SCHEMA_DIR="$schema_dir"
  export SCHEMA_DIR
}

write_local_make_configuration() {
  local config_file="$PROJECT_DIR/Makefile.local"
  local rocprofv3_path
  rocprofv3_path="$(command -v rocprofv3)"

  log "Writing detected AUP build paths to $config_file"
  cat >"$config_file" <<EOF
# Generated by setup-tpe.sh. This file is intentionally not committed.
CONTEST_DIR := $CONTEST_DIR
SCHEMA_DIR := $SCHEMA_DIR
ROCM_PATH := $ROCM_ROOT
ROCM_LIB_DIR := $ROCM_LIBRARY_DIR
ROCTX_INCLUDE_DIR := $ROCM_INCLUDE_DIR
ROCPROFV3 := $rocprofv3_path
CXX_FLAGS := -std=c++17 -O3 -I$LOCAL_PREFIX/include
INTERCHANGE_LIBS := -L$LOCAL_PREFIX/lib -Wl,-rpath,$LOCAL_PREFIX/lib -lcapnp -lkj -lz
EOF
}

compile_pipeline() {
  log "Compiling all preprocessing, routing, and post-processing binaries"
  local cxx_flags="-std=c++17 -O3 -I$LOCAL_PREFIX/include"
  local interchange_libs="-L$LOCAL_PREFIX/lib -Wl,-rpath,$LOCAL_PREFIX/lib -lcapnp -lkj -lz"
  local roctx_flags="-DPATHFINDER_ENABLE_ROCTX -I$ROCM_INCLUDE_DIR"
  local roctx_libs="-L$ROCM_LIBRARY_DIR -Wl,-rpath,$ROCM_LIBRARY_DIR -lrocprofiler-sdk-roctx"

  make -C "$PROJECT_DIR" clean
  make -C "$PROJECT_DIR" -j"$JOBS" pipeline \
    SCHEMA_DIR="$SCHEMA_DIR" \
    ROCM_PATH="$ROCM_ROOT" \
    ROCM_LIB_DIR="$ROCM_LIBRARY_DIR" \
    CXX_FLAGS="$cxx_flags" \
    INTERCHANGE_LIBS="$interchange_libs" \
    PATHFINDER_ENABLE_ROCTX=1 \
    PATHFINDER_ROCTX_FLAGS="$roctx_flags" \
    PATHFINDER_ROCTX_LIBS="$roctx_libs"

  local binary
  for binary in \
    PathFinderFile \
    pathfinder \
    interchange_to_csr \
    device_to_routing_graph \
    routes_to_phys; do
    [[ -x "$PROJECT_DIR/$binary" ]] ||
      die "Expected binary was not built: $PROJECT_DIR/$binary"
  done
}

generate_device_graph() {
  if [[ -s "$DEVICE_GRAPH" && "${REBUILD_DEVICE_GRAPH:-0}" != "1" ]]; then
    log "Using the existing preprocessed device graph at $DEVICE_GRAPH"
    return
  fi

  log "Generating the preprocessed routing device graph"
  "$PROJECT_DIR/device_to_routing_graph" \
    "$DEVICE_FILE" \
    "$DEVICE_GRAPH" \
    --full-device
  [[ -s "$DEVICE_GRAPH" ]] || die "Device graph generation failed."
}

main() {
  validate_managed_paths
  configure_privilege_command
  install_base_packages

  require_command curl
  require_command git
  require_command make
  require_command g++
  require_command python3
  require_command tar

  install_java_21
  export JAVA_HOME="$JAVA_HOME_SETUP"
  export PATH="$HOME/.local/bin:$JAVA_HOME/bin:$LOCAL_PREFIX/bin:$PATH"
  export LD_LIBRARY_PATH="$LOCAL_PREFIX/lib:${LD_LIBRARY_PATH:-}"
  export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

  install_capnproto
  configure_rocm_environment
  require_command hipcc
  install_profiler_if_needed
  require_command rocprofv3
  locate_roctx_installation
  persist_environment

  prepare_contest_repository
  prepare_profiling_repository
  download_benchmarks
  prepare_device
  generate_cpp_schemas
  write_local_make_configuration
  compile_pipeline
  generate_device_graph

  log "Setup complete"
  printf '%s\n' \
    "Contest repository: $CONTEST_DIR" \
    "Profiling repository: $PROJECT_DIR" \
    "Device graph: $DEVICE_GRAPH" \
    "rocprofv3: $(command -v rocprofv3)" \
    "All benchmarks and binaries are ready; no routing workload was started." \
    "Next: cd \"$PROJECT_DIR\" && make run BENCHMARK=logicnets_jscl" \
    "Profile: make profile BENCHMARK=logicnets_jscl"
}

main "$@"
