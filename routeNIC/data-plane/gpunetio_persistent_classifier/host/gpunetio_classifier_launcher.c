/*
 * routeNIC — Experiment 2: host-side bootstrap for the persistent
 * classifier kernel. API calls confirmed against NVIDIA's own installed
 * DOCA GPUNetIO sample on this host
 * (/opt/mellanox/doca/samples/doca_gpunetio/gpunetio_simple_receive):
 * doca_gpu_create, doca_eth_rxq_create + doca_eth_rxq_set_type(CYCLIC),
 * doca_mmap_create for the packet buffer, doca_ctx_start.
 *
 * Earlier versions of this file compiled clean (gcc -fsyntax-only) but had
 * two substantial gaps only found by actually trying to run it against real
 * hardware:
 *   1. `ddev` was declared but never opened (stayed NULL) — every call that
 *      needed it (doca_eth_rxq_create, mmap dev registration) would have
 *      failed or misbehaved at runtime despite type-checking fine.
 *   2. There was no DOCA Flow port/pipe setup at all. Without it, nothing
 *      ever steers arriving packets into this rxq in the first place — the
 *      kernel would poll forever and legitimately never see a packet, which
 *      would have looked identical to "the kernel doesn't work" without
 *      this being the actual cause.
 * Both are fixed here by mirroring gpunetio_simple_receive_sample.c's
 * confirmed-working `init_doca_device`/`init_doca_flow`/`start_doca_flow`/
 * `create_udp_pipe`/`create_root_pipe`/full `create_rxq` (packet-buffer mmap
 * included) sequence, adapted to this experiment's request-ring addition.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_gpunetio.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_flow.h>

#include "../gpunetio_classifier_common.h"
/* open_doca_device_with_pci(): /opt/mellanox/doca/samples/common.h, the same
 * device-open helper every DOCA sample on this host (including Experiment
 * 3's) already shares. */
#include "common.h"

DOCA_LOG_REGISTER(ROUTENIC_GPUNETIO_CLASSIFIER::LAUNCHER);

#define ROUTENIC_FLOW_NB_COUNTERS 524228 /* matches gpunetio_simple_receive_sample.c's FLOW_NB_COUNTERS */
#define ROUTENIC_MAX_PKT_NUM 16384
#define ROUTENIC_MAX_PKT_SIZE 2048

static struct doca_flow_port *g_routenic_flow_port; /* one port per process, matches the sample's own global df_port */

static size_t routenic_host_page_size(void)
{
	long ret = sysconf(_SC_PAGESIZE);
	return (ret == -1) ? 4096 : (size_t)ret;
}

static doca_error_t routenic_open_nic_device(const char *nic_pcie_addr, struct doca_dev **ddev)
{
	doca_error_t result = open_doca_device_with_pci(nic_pcie_addr, NULL, ddev);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to open NIC device %s: %s", nic_pcie_addr, doca_error_get_descr(result));
	return result;
}

static doca_error_t routenic_init_doca_flow(void)
{
	struct doca_flow_cfg *cfg = NULL;
	doca_error_t result;

	result = doca_flow_cfg_create(&cfg);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_flow_cfg_set_pipe_queues(cfg, 1);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_cfg_set_mode_args(cfg, "vnf");
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_cfg_set_nr_counters(cfg, ROUTENIC_FLOW_NB_COUNTERS);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_init(cfg);
out:
	doca_flow_cfg_destroy(cfg);
	return result;
}

static doca_error_t routenic_start_flow_port(struct doca_dev *dev)
{
	struct doca_flow_port_cfg *port_cfg = NULL;
	doca_error_t result;

	result = doca_flow_port_cfg_create(&port_cfg);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_flow_port_cfg_set_port_id(port_cfg, 0);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_port_start(port_cfg, &g_routenic_flow_port);
out:
	doca_flow_port_cfg_destroy(port_cfg);
	return result;
}

/*
 * Matches gpunetio_simple_receive's create_udp_pipe(): a basic (non-root)
 * pipe matching UDP traffic, RSS-forwarding to this rxq's queue 0, dropping
 * on miss. UDP is the match criterion (not something classifier-specific)
 * purely so this experiment can be triggered with an ordinary UDP packet
 * during bring-up rather than needing a custom protocol/port.
 */
static doca_error_t routenic_create_udp_rxq_pipe(struct routenic_rxq_queue *rxq)
{
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	struct doca_flow_fwd miss_fwd = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	struct doca_flow_pipe_entry *entry = NULL;
	uint16_t rss_queues[1] = {0};
	doca_error_t result;

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

	doca_eth_rxq_apply_queue_id(rxq->eth_rxq_cpu, 0);
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = rss_queues;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.rss.nr_queues = 1;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, rxq->port);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "ROUTENIC_RXQ_UDP_PIPE");
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_create(pipe_cfg, &fwd, &miss_fwd, &rxq->rxq_pipe);
	if (result != DOCA_SUCCESS)
		goto out;
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_basic_add_entry(
		0, rxq->rxq_pipe, &match, 0, NULL, NULL, NULL, DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, NULL, &entry);
	if (result != DOCA_SUCCESS)
		return result;
	return doca_flow_entries_process(rxq->port, 0, 0, 0);

out:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/* Matches gpunetio_simple_receive's create_root_pipe(): the is_root control
 * pipe every packet actually hits first, matching plain Ethernet+IPv4+UDP
 * and forwarding into the rxq pipe above. */
static doca_error_t routenic_create_root_pipe(struct routenic_rxq_queue *rxq)
{
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_match udp_match = {
		.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4),
		.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4,
		.outer.ip4.next_proto = IPPROTO_UDP,
	};
	struct doca_flow_fwd udp_fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = rxq->rxq_pipe};
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, rxq->port);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "ROUTENIC_ROOT_PIPE");
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS)
		goto out;
	result = doca_flow_pipe_create(pipe_cfg, NULL, NULL, &rxq->root_pipe);
	if (result != DOCA_SUCCESS)
		goto out;
	doca_flow_pipe_cfg_destroy(pipe_cfg);

	result = doca_flow_pipe_control_add_entry(
		0, rxq->root_pipe, &udp_match, NULL, NULL, NULL, NULL, NULL, NULL, 0, &udp_fwd, NULL, &rxq->root_udp_entry);
	if (result != DOCA_SUCCESS)
		return result;
	return doca_flow_entries_process(rxq->port, 0, 0, 0);

out:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

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
	int dmabuf_fd = -1;
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
	result = doca_mmap_add_dev(mmap, ddev);
	if (result != DOCA_SUCCESS)
		goto destroy_mmap;

	/* Try dmabuf-based GPU memory export first, exactly like the packet
	 * buffer mmap in routenic_create_rxq() — and fall back to plain
	 * doca_mmap_set_memrange() only if that fails. The original version
	 * of this function skipped straight to set_memrange(), which depends
	 * on the legacy nvidia-peermem kernel module; on this host that
	 * module isn't loaded (confirmed via `lsmod`), so that path failed
	 * at doca_mmap_start() with a real, specific error
	 * (DOCA_ERROR_DRIVER, "Failed to register user memory") the first
	 * time this was actually run — not something a syntax check or even
	 * a clean compile could have caught. */
	result = doca_gpu_dmabuf_fd(gpu_dev, gpu_addr, ring_size, &dmabuf_fd);
	if (result != DOCA_SUCCESS) {
		result = doca_mmap_set_memrange(mmap, gpu_addr, ring_size);
		if (result != DOCA_SUCCESS)
			goto destroy_mmap;
	} else {
		result = doca_mmap_set_dmabuf_memrange(mmap, dmabuf_fd, gpu_addr, 0, ring_size);
		if (result != DOCA_SUCCESS)
			goto destroy_mmap;
	}

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
 *
 * The packet-buffer mmap block below (doca_eth_rxq_estimate_packet_buf_size
 * through doca_eth_rxq_set_pkt_buf) was entirely missing from the first
 * version of this function — it compiled fine (nothing here was syntactically
 * wrong, there was just less of it), but without real packet-buffer memory
 * registered, doca_ctx_start() below would have had nothing valid to bind to.
 * Restored to match gpunetio_simple_receive_sample.c's create_rxq() exactly.
 */
static doca_error_t routenic_create_rxq(struct doca_gpu *gpu_dev, int cuda_id, struct doca_dev *ddev, struct routenic_rxq_queue *rxq)
{
	doca_error_t result;
	uint32_t cyclic_buffer_size = 0;
	struct cudaDeviceProp prop;

	rxq->gpu_dev = gpu_dev;
	rxq->ddev = ddev;
	rxq->port = g_routenic_flow_port;

	result = doca_eth_rxq_create(rxq->ddev, ROUTENIC_MAX_PKT_NUM, ROUTENIC_MAX_PKT_SIZE, &rxq->eth_rxq_cpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_create: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_rxq_set_type(rxq->eth_rxq_cpu, DOCA_ETH_RXQ_TYPE_CYCLIC);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed doca_eth_rxq_set_type: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_eth_rxq_estimate_packet_buf_size(
		DOCA_ETH_RXQ_TYPE_CYCLIC, 0, 0, ROUTENIC_MAX_PKT_SIZE, ROUTENIC_MAX_PKT_NUM, 0, 0, 0, &cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to estimate eth_rxq packet buffer size: %s", doca_error_get_descr(result));
		return result;
	}
	cyclic_buffer_size =
		(uint32_t)(((cyclic_buffer_size + routenic_host_page_size() - 1) / routenic_host_page_size()) *
			   routenic_host_page_size());

	result = doca_mmap_create(&rxq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_mmap_add_dev(rxq->pkt_buff_mmap, rxq->ddev);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_gpu_mem_alloc(rxq->gpu_dev,
				     cyclic_buffer_size,
				     routenic_host_page_size(),
				     DOCA_GPU_MEM_TYPE_GPU,
				     (void **)&rxq->gpu_pkt_addr,
				     NULL);
	if (result != DOCA_SUCCESS || rxq->gpu_pkt_addr == NULL) {
		DOCA_LOG_ERR("Failed to allocate GPU packet buffer memory: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_gpu_dmabuf_fd(rxq->gpu_dev, rxq->gpu_pkt_addr, cyclic_buffer_size, &rxq->dmabuf_fd);
	if (result != DOCA_SUCCESS) {
		/* Falls back to the legacy nvidia-peermem path, exactly as the
		 * confirmed-working sample does — not every kernel/driver
		 * combination supports dmabuf-based GPU memory export. */
		result = doca_mmap_set_memrange(rxq->pkt_buff_mmap, rxq->gpu_pkt_addr, cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set memrange for packet buffer mmap: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_mmap_set_dmabuf_memrange(
			rxq->pkt_buff_mmap, rxq->dmabuf_fd, rxq->gpu_pkt_addr, 0, cyclic_buffer_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set dmabuf memrange for packet buffer mmap: %s", doca_error_get_descr(result));
			return result;
		}
	}

	result = doca_mmap_set_permissions(rxq->pkt_buff_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_mmap_start(rxq->pkt_buff_mmap);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_eth_rxq_set_pkt_buf(rxq->eth_rxq_cpu, rxq->pkt_buff_mmap, 0, cyclic_buffer_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set eth_rxq packet buffer: %s", doca_error_get_descr(result));
		return result;
	}

	cudaGetDeviceProperties(&prop, cuda_id);
	if (prop.major < 9) {
		result = doca_eth_rxq_gpu_enable_mcst_qp(rxq->eth_rxq_cpu);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to enable GPU mcst qp: %s", doca_error_get_descr(result));
			return result;
		}
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

	result = routenic_create_udp_rxq_pipe(rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create UDP rxq pipe: %s", doca_error_get_descr(result));
		return result;
	}
	result = routenic_create_root_pipe(rxq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create root pipe: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

struct routenic_watchdog_ctx {
	uint32_t *gpu_exit_condition; /* device pointer */
	uint32_t seconds;
	cudaStream_t signal_stream; /* separate from the kernel's stream, so this memcpy can run concurrently */
};

/* Forces the persistent kernel to exit after a fixed wall-clock delay,
 * regardless of packet arrivals — the safety mechanism that makes a bring-up
 * test run bounded instead of an indefinitely spinning GPU kernel. Runs on
 * its own host thread, concurrently with the blocking
 * cudaStreamSynchronize() in routenic_classifier_launch() below; the
 * H2D memcpy is issued on a *different* CUDA stream than the kernel so it
 * isn't queued behind (and blocked on) the still-running persistent kernel. */
static void *routenic_watchdog_thread(void *arg)
{
	struct routenic_watchdog_ctx *ctx = (struct routenic_watchdog_ctx *)arg;
	uint32_t one = 1;

	sleep(ctx->seconds);
	DOCA_LOG_INFO("routeNIC watchdog: %u second bound reached, signaling kernel exit", ctx->seconds);
	cudaMemcpyAsync(ctx->gpu_exit_condition, &one, sizeof(one), cudaMemcpyHostToDevice, ctx->signal_stream);
	cudaStreamSynchronize(ctx->signal_stream);
	return NULL;
}

/*
 * Runs the whole experiment: bootstrap, launch the persistent kernel once,
 * block until `gpu_exit_condition` is signaled — either externally, or by
 * the bounded-run watchdog above when `cfg->bounded_run_seconds != 0` — and
 * report the total classified count.
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
	cudaStream_t stream, watchdog_stream;
	pthread_t watchdog_tid = 0;
	struct routenic_watchdog_ctx watchdog_ctx = {0};
	doca_error_t result;
	cudaError_t cuda_result;

	/* CUDA context must exist, and must be pointed at the right device in
	 * a multi-GPU-visible system, before any GPUNetIO call — matches
	 * gpunetio_simple_receive_main.c's cudaFree(0) + cudaDeviceGetByPCIBusId
	 * + cudaSetDevice sequence exactly. */
	cuda_result = cudaFree(0);
	if (cuda_result != cudaSuccess) {
		DOCA_LOG_ERR("CUDA initialization failed: %s", cudaGetErrorString(cuda_result));
		return DOCA_ERROR_BAD_STATE;
	}
	cuda_result = cudaDeviceGetByPCIBusId(&cfg->cuda_id, cfg->gpu_pcie_addr);
	if (cuda_result != cudaSuccess) {
		DOCA_LOG_ERR("Invalid GPU bus id %s: %s", cfg->gpu_pcie_addr, cudaGetErrorString(cuda_result));
		return DOCA_ERROR_BAD_STATE;
	}
	cudaSetDevice(cfg->cuda_id);

	result = doca_gpu_create(cfg->gpu_pcie_addr, &gpu_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_gpu_create returned %s", doca_error_get_descr(result));
		return result;
	}

	/* Opening `ddev` by PCI address (cfg->nic_pcie_addr) is what makes
	 * this experiment device-agnostic per the project's "no fixed NIC"
	 * instruction — pass the BF3's or the ConnectX-7's PCI BDF here and
	 * the rest of this file is unchanged either way; only the DPA-hosted
	 * experiments (1, 4) are inherently BF3-only. */
	result = routenic_open_nic_device(cfg->nic_pcie_addr, &ddev);
	if (result != DOCA_SUCCESS)
		return result;

	result = routenic_init_doca_flow();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}
	result = routenic_start_flow_port(ddev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA Flow port: %s", doca_error_get_descr(result));
		return result;
	}

	result = routenic_create_rxq(gpu_dev, cfg->cuda_id, ddev, &rxq);
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
	cudaMemset(tot_classified, 0, sizeof(uint64_t));

	cudaStreamCreate(&stream);

	if (cfg->bounded_run_seconds > 0) {
		cudaStreamCreate(&watchdog_stream);
		watchdog_ctx.gpu_exit_condition = gpu_exit_condition;
		watchdog_ctx.seconds = cfg->bounded_run_seconds;
		watchdog_ctx.signal_stream = watchdog_stream;
		if (pthread_create(&watchdog_tid, NULL, routenic_watchdog_thread, &watchdog_ctx) != 0) {
			DOCA_LOG_ERR("Failed to start bounded-run watchdog thread");
			return DOCA_ERROR_OPERATING_SYSTEM;
		}
	}

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

	if (watchdog_tid != 0)
		pthread_join(watchdog_tid, NULL);

	uint64_t final_count = 0;
	cudaMemcpy(&final_count, tot_classified, sizeof(uint64_t), cudaMemcpyDeviceToHost);
	DOCA_LOG_INFO("routeNIC persistent classifier: classified %lu requests", final_count);
	printf("routeNIC Experiment 2: persistent kernel exited cleanly, classified %lu requests\n", final_count);

	return DOCA_SUCCESS;
}
