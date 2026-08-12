/*
 * routeNIC — Experiment 2: GPUNetIO persistent classifier
 *
 * Shared host/device types. The receive/polling mechanics follow the
 * pattern confirmed against NVIDIA's own installed DOCA GPUNetIO samples
 * (/opt/mellanox/doca/samples/doca_gpunetio/gpunetio_simple_receive) on this
 * host — same doca_gpu_eth_rxq handle, same DOCA_GPUNETIO_VOLATILE exit-flag
 * loop — extended with a request ring buffer so the persistent kernel does
 * request *batching*, not just packet reception.
 */

#ifndef ROUTENIC_GPUNETIO_CLASSIFIER_COMMON_H_
#define ROUTENIC_GPUNETIO_CLASSIFIER_COMMON_H_

#include <stdint.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include <doca_error.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_gpunetio.h>
#include <doca_gpunetio_eth_def.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTENIC_MAX_PCI_ADDR_LEN 32U
#define ROUTENIC_CUDA_BLOCK_THREADS 32
#define ROUTENIC_MAX_RX_NUM_PKTS 2048
#define ROUTENIC_MAX_RX_TIMEOUT_NS 500000U /* 500us, matches the DOCA sample default */

/* One classifier request slot. `text` is fixed-size and truncated on the
 * host/DPU side before RDMA write — a real deployment would carry a length
 * prefix and a variable-length region; fixed-size keeps the first working
 * version's memory layout (and therefore its perf numbers) easy to reason
 * about, and is called out explicitly in the experiment doc as a
 * simplification to revisit once the mechanism is proven. */
#define ROUTENIC_MAX_REQUEST_TEXT_BYTES 512
#define ROUTENIC_RING_CAPACITY 4096

struct routenic_request_slot {
	uint64_t request_id;
	uint32_t text_len;
	uint8_t text[ROUTENIC_MAX_REQUEST_TEXT_BYTES];
	/* Written by the persistent kernel once classification completes;
	 * the host/monitoring side polls this field to know when a slot's
	 * result is ready without a second round-trip. */
	volatile uint32_t result_ready;
	uint32_t matched_signal_mask;
	float confidence;
};

/* A single-producer/single-consumer ring living in GB10 unified memory.
 * The producer (BF3, via RDMA WRITE) advances `write_idx`; the persistent
 * CUDA kernel is the sole consumer and advances `read_idx`. Both indices are
 * free-running (not modulo'd) so wraparound detection is a simple subtract,
 * matching the classic SPSC ring pattern. */
struct routenic_request_ring {
	struct routenic_request_slot slots[ROUTENIC_RING_CAPACITY];
	volatile uint64_t write_idx;
	volatile uint64_t read_idx;
};

struct routenic_classifier_cfg {
	char gpu_pcie_addr[ROUTENIC_MAX_PCI_ADDR_LEN];
	char nic_pcie_addr[ROUTENIC_MAX_PCI_ADDR_LEN];
	int cuda_id;
	enum doca_gpu_dev_eth_exec_scope exec_scope;
	uint32_t max_batch_size;
};

struct routenic_rxq_queue {
	struct doca_gpu *gpu_dev;
	struct doca_dev *ddev;

	struct doca_ctx *eth_rxq_ctx;
	struct doca_eth_rxq *eth_rxq_cpu;
	struct doca_gpu_eth_rxq *eth_rxq_gpu;
	struct doca_mmap *pkt_buff_mmap;
	void *gpu_pkt_addr;
	int dmabuf_fd;
	struct doca_flow_port *port;
	struct doca_flow_pipe *rxq_pipe;
	struct doca_flow_pipe *root_pipe;
	struct doca_flow_pipe_entry *root_udp_entry;
};

doca_error_t routenic_classifier_launch(struct routenic_classifier_cfg *cfg);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side launcher for the persistent kernel. Mirrors
 * kernel_receive_packets() in the DOCA sample: one CUDA kernel launch call,
 * which is the project's one allowed host-CPU touch of the per-request path
 * (the kernel itself runs until `gpu_exit_condition` is set). */
doca_error_t routenic_kernel_persistent_classify(cudaStream_t stream,
						  struct routenic_rxq_queue *rxq,
						  struct routenic_request_ring *ring,
						  enum doca_gpu_dev_eth_exec_scope exec_scope,
						  uint32_t max_batch_size,
						  uint32_t *gpu_exit_condition,
						  uint64_t *tot_requests_classified);

#ifdef __cplusplus
}
#endif

#endif /* ROUTENIC_GPUNETIO_CLASSIFIER_COMMON_H_ */
