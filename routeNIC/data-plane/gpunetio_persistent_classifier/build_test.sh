#!/bin/bash
# routeNIC — Experiment 2: builds the real hardware bring-up test.
# Flags/link order confirmed by actually building and running this on the
# lab's DGX Spark (GB10 + ConnectX-7) — see docs/experiments/02-*.md for
# what that run found and fixed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DOCA_DIR="/opt/mellanox/doca"
DOCA_INC="-I${DOCA_DIR}/include -I/usr/include/libnl3 -DALLOW_EXPERIMENTAL_API -DDOCA_ALLOW_EXPERIMENTAL_API -I${DOCA_DIR}/samples"
DOCA_LIBDIR="${DOCA_DIR}/lib/aarch64-linux-gnu"
GPU_ARCH="arch=compute_121,code=sm_121" # GB10; override for other GPUs

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

nvcc -c "${SCRIPT_DIR}/device/gpunetio_classifier_kernel.cu" ${DOCA_INC} \
	-forward-unknown-to-host-compiler -rdc=true -gencode "${GPU_ARCH}" \
	-o "${BUILD_DIR}/kernel.o"

nvcc -dlink "${BUILD_DIR}/kernel.o" -gencode "${GPU_ARCH}" -o "${BUILD_DIR}/kernel_dlink.o"

gcc -c "${SCRIPT_DIR}/host/gpunetio_classifier_launcher.c" ${DOCA_INC} -I/usr/local/cuda/include -o "${BUILD_DIR}/launcher.o"
gcc -c "${SCRIPT_DIR}/host/gpunetio_classifier_main.c" ${DOCA_INC} -I/usr/local/cuda/include -o "${BUILD_DIR}/main.o"
gcc -c "${DOCA_DIR}/samples/common.c" ${DOCA_INC} -o "${BUILD_DIR}/common.o"

nvcc "${BUILD_DIR}/launcher.o" "${BUILD_DIR}/main.o" "${BUILD_DIR}/common.o" "${BUILD_DIR}/kernel.o" "${BUILD_DIR}/kernel_dlink.o" \
	-gencode "${GPU_ARCH}" \
	-L"${DOCA_LIBDIR}" -Xlinker -rpath -Xlinker "${DOCA_LIBDIR}" \
	-ldoca_common -ldoca_gpunetio -ldoca_eth \
	-ldoca_flow_info_comp -ldoca_flow_pipeline_loader -ldoca_flow -ldoca_flow_tune_server -ldoca_flow_definitions \
	-ldoca_argp -lpthread \
	-o "${BUILD_DIR}/routenic_gpunetio_test"

echo "Built ${BUILD_DIR}/routenic_gpunetio_test"
echo "Run: sudo ${BUILD_DIR}/routenic_gpunetio_test <gpu-pci-addr> <nic-pci-addr>"
echo "  e.g.: sudo ${BUILD_DIR}/routenic_gpunetio_test 0000000f:01:00.0 0000:01:00.0"
