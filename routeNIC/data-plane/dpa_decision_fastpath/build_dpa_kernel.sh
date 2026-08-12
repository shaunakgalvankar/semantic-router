#!/bin/bash
# routeNIC — Experiment 1: builds the DPA fast-path kernel via dpacc.
#
# Flags mirror /opt/mellanox/doca/samples/doca_dpa/build_dpacc_samples.sh
# exactly (this repo's actual sample build script) — verified to compile
# clean on this host for both the BF3 and CX7 mcpu targets before this
# script was written.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DOCA_DIR="/opt/mellanox/doca"
DOCA_INCLUDE="${DOCA_DIR}/include"
DOCA_LIB_DIR="${DOCA_DIR}/lib/aarch64-linux-gnu"
DOCA_DPACC="${DOCA_DIR}/tools/dpacc"
DOCA_APP_ATTRIBUTES2BLOB="${DOCA_DIR}/tools/dpa-app-attributes2blob"

# nv-dpa-bf3 is the real target; nv-dpa-cx7 is included so the same kernel
# source can be validated (compile-only — ConnectX-7 has no DPA to actually
# run it on) against the other NIC in this lab, per the project's "no fixed
# NIC" instruction for anything that isn't inherently BF3-only.
# --app-name must be exactly "dpa_sample_app": dpa_common.c (the shared DPA
# bootstrap every sample in this DOCA install links against, reused as-is by
# host/fastpath_launcher.c/fastpath_main.c rather than re-derived) hardcodes
# `extern struct doca_dpa_app *dpa_sample_app;` and passes that literal
# symbol to doca_dpa_set_app() — confirmed by reading dpa_common.c and every
# sample's build_dpacc_samples.sh, which all pass this same fixed name.
# Originally built here as "routenic_fastpath_app", which compiled fine via
# dpacc (dpacc doesn't care about downstream host-side linkage) but would
# have failed to link the first time fastpath_main.c actually called
# allocate_dpa_resources() — caught by reading the real source, not by
# compiling alone.
MCPU_TARGETS="nv-dpa-bf3,nv-dpa-cx7"
APP_NAME="dpa_sample_app"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

"${DOCA_APP_ATTRIBUTES2BLOB}" "${SCRIPT_DIR}/dpa_kernel/fastpath_attributes.yaml" "${BUILD_DIR}/fastpath_attributes.blob"

"${DOCA_DPACC}" "${SCRIPT_DIR}/dpa_kernel/fastpath_kernel_dev.c" \
	-o "${BUILD_DIR}/routenic_fastpath.a" \
	-mcpu="${MCPU_TARGETS}" \
	-hostcc=gcc \
	-hostcc-options="-Wno-deprecated-declarations -Wall -DFLEXIO_ALLOW_EXPERIMENTAL_API" \
	--devicecc-options="-Wno-deprecated-declarations -Wall -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API -O2" \
	--app-name="${APP_NAME}" \
	-device-libs="-L${DOCA_LIB_DIR} -ldoca_dpa_dev -ldoca_dpa_dev_comm -ldoca_dpa_dev_verbs" \
	-flto \
	-I"${DOCA_INCLUDE}" \
	--dpa-proc-attr="${BUILD_DIR}/fastpath_attributes.blob"

echo "Built ${BUILD_DIR}/routenic_fastpath.a"
