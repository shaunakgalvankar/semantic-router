/*
 * routeNIC — Experiment 2: host-side bootstrap for the persistent
 * classifier kernel. API calls confirmed against NVIDIA's own installed
 * DOCA GPUNetIO sample on this host
 * (/opt/mellanox/doca/samples/doca_gpunetio/gpunetio_simple_receive):
 * doca_gpu_create, doca_eth_rxq_create + doca_eth_rxq_set_type(CYCLIC),
 * doca_mmap_create for the packet buffer, doca_ctx_start. This file adds
 * only what's routeNIC-specific on top: allocating and registering the
 * SPSC request ring in GB10 unified memory, and launching the persistent
 * kernel exactly once (the project's one allowed host-CPU touch of the
 * per-request path).
 */

#include <string.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_gpunetio.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_flow.h>

#include "../gpunetio_classifier_common.h"

DOCA_LOG_REGISTER(ROUTENIC_GPUNETIO_CLASSIFIER::LAUNCHER);

/*
 * Allocates the SPSC request ring in GB10 unified memory as GPU memory
 * registered for GPUDirect RDMA (doca_mmap with DOCA_ACCESS_FLAG_PCI_*), so
 * a peer (BF3, or another host over RDMA) can RDMA WRITE directly into it
 * with the GPU as the only consumer — no host-CPU copy in between.
 */
static doca_error_t routenic_alloc_request_ring(struct doca_gpu *gpu_dev,
						  struct doca_dev *ddev,
						  struct routenic_request_ring **out_ring,
						  struct doca_mmap **out_mmap)
{
	struct doca_mmap *mmap = NULL;
	void *gpu_addr = NULL;
	doca_error_t result;
	size_t ring_size = sizeof(struct routenic_request_ring);

	result = doca_gpu_mem_alloc(gpu_dev,
				     ring_size,
				     4096, /* page-aligned, matches every sample's GPU alloc convention */
				     DOCA_GPU_MEM_TYPE_GPU,
				     &gpu_addr,
				     NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate request ring in GPU memory: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_mmap_create(&mmap);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_mmap_set_memrange(mmap, gpu_addr, ring_size);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	result = doca_mmap_add_dev(mmap, ddev);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	/* GPUDirect RDMA target: the BF3 (or any RDMA peer) can WRITE into
	 * this region directly; the persistent kernel is the sole reader. */
	result = doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;
	result = doca_mmap_start(mmap);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;

	memset(gpu_addr, 0, ring_size);
	*out_ring = (struct routenic_request_ring *)gpu_addr;
	*out_mmap = mmap;
	return DOCA_SUCCESS;

destroy_mmap:
	doca_mmap_destroy(mmap);
	return result;
}

/*
 * Creates the Ethernet receive queue used by Experiment 8's variant (raw
 * packet parsing, no BF3 involvement) and, in the RDMA-write deployment
 * (BF3 -> unified memory), simply left unused by the kernel's receive loop
 * -- both code paths share the same persistent_classify() kernel and
 * differ only in how requests reach the ring, per docs/architecture.md's
 * "two independent axes of offload."
 */
static doca_error_t routenic_create_rxq(struct doca_gpu *gpu_dev, struct doca_dev *ddev, struct routenic_rxq_queue *rxq)
{
	doca_error_t result;

	rxq->gpu_dev = gpu_dev;
	rxq->ddev = ddev;

	result = doca_eth_rxq_create(rxq->ddev, ROUTENIC_MAX_RX_NUM_PKTS, 2048, &rxq->eth_rxq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_create: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_rxq_set_type(rxq->eth_rxq_cpu, DOCA_ETH_RXQ_TYPE_CYCLIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_set_type: %s", doca_error_get_descr(result));
		return result;
	}

	rxq->eth_rxq_ctx = doca_eth_rxq_as_doca_ctx(rxq->eth_rxq_cpu);
	if (rxq->eth_rxq_ctx == NULL)
		return DOCA_ERROR_UNEXPECTED;

	result = doca_ctx_set_datapath_on_gpu(rxq->eth_rxq_ctx, rxq->gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_set_datapath_on_gpu: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_start(rxq->eth_rxq_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_ctx_start: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_rxq_get_gpu_handle(rxq->eth_rxq_cpu, &rxq->eth_rxq_gpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_get_gpu_handle: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Runs the whole experiment: bootstrap, launch the persistent kernel once,
 * block until `gpu_exit_condition` is externally signaled (e.g. by a
 * control-plane orchestrator writing to host-pinned memory the kernel also
 * polls — not shown here, see control-plane/orchestrator/), report the
 * total classified count.
 */
doca_error_t routenic_classifier_launch(struct routenic_classifier_cfg *cfg)
{
	struct doca_gpu *gpu_dev = NULL;
	struct doca_dev *ddev = NULL;
	struct routenic_rxq_queue rxq = {0};
	struct routenic_request_ring *ring = NULL;
	struct doca_mmap *ring_mmap = NULL;
	uint32_t *gpu_exit_condition = NULL;
	uint64_t *tot_classified = NULL;
	cudaStream_t stream;
	doca_error_t result;

	result = doca_gpu_create(cfg->gpu_pcie_addr, &gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(result));
		return result;
	}

	/* NOTE: opening `ddev` by PCI address (cfg->nic_pcie_addr) is what
	 * makes this experiment device-agnostic per the project's "no fixed
	 * NIC" instruction — pass the BF3's or the ConnectX-7's PCI BDF here
	 * and the rest of this file is unchanged either way; only the
	 * DPA-hosted experiments (1, 4) are inherently BF3-only. */

	result = routenic_create_rxq(gpu_dev, ddev, &rxq);
	if (result != DOCA_SUCCESS)
		return result;

	result = routenic_alloc_request_ring(gpu_dev, ddev, &ring, &ring_mmap);
	if (result != DOCA_SUCCESS)
		return result;

	if (cudaMalloc((void **)&gpu_exit_condition, sizeof(uint32_t)) != cudaSuccess)
		return DOCA_ERROR_NO_MEMORY;
	if (cudaMalloc((void **)&tot_classified, sizeof(uint64_t)) != cudaSuccess)
		return DOCA_ERROR_NO_MEMORY;
	cudaMemset(gpu_exit_condition, 0, sizeof(uint32_t));

	cudaStreamCreate(&stream);

	/* The one host-CPU touch of the per-request path: launching the
	 * persistent kernel. Everything after this call runs on the GPU
	 * until gpu_exit_condition is set. */
	result = routenic_kernel_persistent_classify(
		stream, &rxq, ring, cfg->exec_scope, cfg->max_batch_size, gpu_exit_condition, tot_classified);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to launch persistent classifier kernel: %s", doca_error_get_descr(result));
		return result;
	}

	cudaStreamSynchronize(stream);

	uint64_t final_count = 0;
	cudaMemcpy(&final_count, tot_classified, sizeof(uint64_t), cudaMemcpyDeviceToHost);
	DOCA_LOG_INFO("routeNIC persistent classifier: classified %lu requests", final_count);

	return DOCA_SUCCESS;
}
