#!/usr/bin/env bash
set -Eeuo pipefail

# Prepare an AMD University Program cloud instance for building and profiling
# the Delta-Stepping PathFinder pipeline. This script intentionally stops after
# downloading assets, generating the device graph, and compiling all binaries.
# It never invokes sudo or a system package manager.

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
readonly ZLIB_VERSION="${ZLIB_VERSION:-1.3.1}"
readonly DEVICE_NAME="xcvu3p"
readonly DEVICE_FILE="$CONTEST_DIR/$DEVICE_NAME.device"
readonly DEVICE_GRAPH="$PROJECT_DIR/$DEVICE_NAME.full-poc-base-wire.devicegraph"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

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

validate_base_tools() {
  log "Validating the non-privileged AUP build environment"
  local command_name
  for command_name in curl git make g++ python3 tar; do
    require_command "$command_name"
  done
}

ensure_python_pip() {
  if python3 -m pip --version >/dev/null 2>&1; then
    return
  fi

  log "Installing pip in the current user's home directory"
  mkdir -p "$CACHE_DIR"
  local get_pip="$CACHE_DIR/get-pip.py"
  download "https://bootstrap.pypa.io/get-pip.py" "$get_pip"
  python3 "$get_pip" --user
  python3 -m pip --version >/dev/null 2>&1 ||
    die "pip could not be installed without administrator privileges."
}

install_zlib() {
  if [[ -f "$LOCAL_PREFIX/include/zlib.h" ]] &&
     find "$LOCAL_PREFIX/lib" -maxdepth 1 \
       \( -name 'libz.so' -o -name 'libz.a' \) -print -quit 2>/dev/null |
       grep -q .; then
    log "The user-local zlib installation is already available"
    return
  fi

  log "Building zlib $ZLIB_VERSION in the current user's home directory"
  mkdir -p "$CACHE_DIR" "$LOCAL_PREFIX/src"
  local archive="$CACHE_DIR/zlib-$ZLIB_VERSION.tar.gz"
  local source_dir="$LOCAL_PREFIX/src/zlib-$ZLIB_VERSION"
  download \
    "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz" \
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

locate_rocprofv3() {
  local candidate
  candidate="$(command -v rocprofv3 || true)"
  if [[ -z "$candidate" && -x "$ROCM_ROOT/bin/rocprofv3" ]]; then
    candidate="$ROCM_ROOT/bin/rocprofv3"
  fi
  if [[ -z "$candidate" ]]; then
    candidate="$(
      find "$ROCM_ROOT" -type f -name rocprofv3 -perm -u+x \
        -print -quit 2>/dev/null || true
    )"
  fi
  [[ -n "$candidate" ]] ||
    die "rocprofv3 was not found in the preinstalled ROCm tree at $ROCM_ROOT. This setup never uses sudo; select an AUP image that includes ROCprofiler-SDK/rocprofv3 or set ROCM_PATH to that installation."
  ROCPROFV3_PATH="$(cd -- "$(dirname -- "$candidate")" && pwd -P)/$(basename "$candidate")"
  export ROCPROFV3_PATH
  log "Using preinstalled rocprofv3 at $ROCPROFV3_PATH"
}

locate_roctx_installation() {
  local sdk_header
  local sdk_library
  sdk_header="$(
    find "$ROCM_ROOT/include" /usr/include \
      -path '*/rocprofiler-sdk-roctx/roctx.h' -print -quit 2>/dev/null ||
      true
  )"
  sdk_library="$(
    find "$ROCM_ROOT/lib" "$ROCM_ROOT/lib64" \
      /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu \
      -name 'librocprofiler-sdk-roctx.so*' -print -quit 2>/dev/null ||
      true
  )"
  if [[ -n "$sdk_header" && -n "$sdk_library" ]]; then
    ROCTX_STYLE="sdk"
    ROCTX_INCLUDE_DIR="$(dirname "$(dirname "$sdk_header")")"
    ROCTX_LIBRARY="$sdk_library"
  else
    local legacy_header
    local legacy_library
    legacy_header="$(
      find "$ROCM_ROOT/roctracer/include" "$ROCM_ROOT/include" /usr/include \
        -name roctx.h \
        ! -path '*/rocprofiler-sdk-roctx/*' \
        -print -quit 2>/dev/null || true
    )"
    legacy_library="$(
      find "$ROCM_ROOT/lib" "$ROCM_ROOT/lib64" \
        /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu \
        -name 'libroctx64.so*' -print -quit 2>/dev/null || true
    )"
    if [[ -n "$legacy_header" && -n "$legacy_library" ]]; then
      ROCTX_STYLE="legacy"
      ROCTX_INCLUDE_DIR="$(dirname "$legacy_header")"
      ROCTX_LIBRARY="$legacy_library"
    else
      ROCTX_STYLE="none"
      ROCTX_INCLUDE_DIR="$ROCM_ROOT/include"
      ROCTX_LIBRARY=""
    fi
  fi

  if [[ -n "$ROCTX_LIBRARY" ]]; then
    ROCM_LIBRARY_DIR="$(dirname "$ROCTX_LIBRARY")"
    log "Using $ROCTX_STYLE ROCTx from $ROCTX_LIBRARY"
  elif [[ -d "$ROCM_ROOT/lib" ]]; then
    ROCM_LIBRARY_DIR="$ROCM_ROOT/lib"
    log "ROCTx development files are unavailable; custom ranges will be disabled"
  else
    ROCM_LIBRARY_DIR="$ROCM_ROOT/lib64"
    log "ROCTx development files are unavailable; custom ranges will be disabled"
  fi
  export ROCTX_STYLE ROCTX_INCLUDE_DIR ROCTX_LIBRARY ROCM_LIBRARY_DIR
}

persist_environment() {
  local environment_dir="$HOME/.config/$PROJECT_NAME"
  local environment_file="$environment_dir/environment.sh"
  mkdir -p "$environment_dir"
  cat >"$environment_file" <<EOF
export JAVA_HOME="$JAVA_HOME_SETUP"
export LOCAL_PREFIX="$LOCAL_PREFIX"
export ROCM_PATH="$ROCM_ROOT"
export ROCPROFV3="$ROCPROFV3_PATH"
export PATH="$HOME/.local/bin:\$JAVA_HOME/bin:\$LOCAL_PREFIX/bin:\$ROCM_PATH/bin:$(dirname "$ROCPROFV3_PATH"):\$PATH"
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

prepare_java_schema() {
  local java_schema="$CONTEST_DIR/fpga-interchange-schema/interchange/capnp/java.capnp"
  mkdir -p "$(dirname "$java_schema")"
  if [[ ! -s "$java_schema" ]]; then
    log "Downloading the Cap'n Proto Java schema without system package tools"
    download \
      "https://raw.githubusercontent.com/capnproto/capnproto-java/master/compiler/src/main/schema/capnp/java.capnp" \
      "$java_schema"
  fi
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
  log "Generating FPGA Interchange C++ schemas"
  prepare_java_schema
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

  log "Writing detected AUP build paths to $config_file"
  cat >"$config_file" <<EOF
# Generated by setup-tpe.sh. This file is intentionally not committed.
CONTEST_DIR := $CONTEST_DIR
SCHEMA_DIR := $SCHEMA_DIR
ROCM_PATH := $ROCM_ROOT
ROCM_LIB_DIR := $ROCM_LIBRARY_DIR
ROCTX_STYLE := $ROCTX_STYLE
ROCTX_INCLUDE_DIR := $ROCTX_INCLUDE_DIR
ROCTX_LIBRARY := $ROCTX_LIBRARY
ROCPROFV3 := $ROCPROFV3_PATH
INTERCHANGE_CPPFLAGS := -I$LOCAL_PREFIX/include
INTERCHANGE_LIBS := -L$LOCAL_PREFIX/lib -Wl,-rpath,$LOCAL_PREFIX/lib -lcapnp -lkj -lz
EOF
}

compile_pipeline() {
  log "Compiling all preprocessing, routing, and post-processing binaries"

  make -C "$PROJECT_DIR" clean
  make -C "$PROJECT_DIR" -j"$JOBS" pipeline

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
  validate_base_tools
  ensure_python_pip

  install_java_21
  export JAVA_HOME="$JAVA_HOME_SETUP"
  export PATH="$HOME/.local/bin:$JAVA_HOME/bin:$LOCAL_PREFIX/bin:$PATH"
  export LD_LIBRARY_PATH="$LOCAL_PREFIX/lib:${LD_LIBRARY_PATH:-}"
  export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

  install_zlib
  install_capnproto
  configure_rocm_environment
  require_command hipcc
  locate_rocprofv3
  locate_roctx_installation
  persist_environment

  prepare_contest_repository
  prepare_profiling_repository
  prepare_java_schema
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
    "rocprofv3: $ROCPROFV3_PATH" \
    "All benchmarks and binaries are ready; no routing workload was started." \
    "Next: cd \"$PROJECT_DIR\" && make run BENCHMARK=logicnets_jscl" \
    "Profile: make profile BENCHMARK=logicnets_jscl"
}

main "$@"
