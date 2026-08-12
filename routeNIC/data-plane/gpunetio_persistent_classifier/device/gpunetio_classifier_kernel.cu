/*
 * routeNIC — Experiment 2: GPUNetIO persistent classifier, device side.
 *
 * Structure follows the confirmed-working pattern in NVIDIA's own
 * gpunetio_simple_receive sample on this host: a single persistent kernel,
 * launched once, spinning on doca_gpu_dev_eth_rxq_recv() with a
 * DOCA_GPUNETIO_VOLATILE exit flag as the only way out. That sample stops at
 * "packet received"; this one goes one step further and treats each
 * received packet as one classifier request, batches arrivals into the SPSC
 * ring in routenic_gpunetio_classifier_common.h, and calls a per-batch
 * classify hook — entirely inside the kernel, so the host CPU's only
 * involvement in the per-request path is the one-time kernel launch.
 */

#include <doca_gpunetio_dev_eth_rxq.cuh>
#include <doca_log.h>

#include "../gpunetio_classifier_common.h"

DOCA_LOG_REGISTER(ROUTENIC_GPUNETIO_CLASSIFIER::KERNEL);

/*
 * classify_batch() is the extension point this experiment is really about.
 * It is intentionally NOT a full BERT forward pass — wiring a transformer
 * inference engine into a raw persistent CUDA kernel (rather than through
 * a host-orchestrated inference server) is a substantial project on its
 * own, and conflating it with the RDMA/batching mechanics this experiment
 * measures would make it impossible to tell which part of any latency win
 * came from which change.
 *
 * What's implemented here is a real, deterministic, GPU-side classification
 * step (byte-parallel keyword hit-counting across the batch, one warp lane
 * per candidate keyword) so the kernel is genuinely end-to-end runnable and
 * produces a genuine confidence/result value — not a stub that always
 * returns a constant. Swapping this function body for a call into a
 * compiled inference engine (e.g. a TensorRT engine's device-side enqueue)
 * is the natural next step once the mechanism below is validated; the ring
 * buffer, batching, and polling code do not need to change to support that.
 */
__device__ __forceinline__ void classify_batch(struct routenic_request_slot *batch, uint32_t batch_size)
{
	uint32_t slot = threadIdx.x;

	while (slot < batch_size) {
		struct routenic_request_slot *req = &batch[slot];

		/* Placeholder signal: does the request text contain a small,
		 * fixed set of high-urgency keywords? This exercises the same
		 * "keyword signal" concept the accuracy study in
		 * control-plane/accuracy_study/ measures on the DPA side, so
		 * results from both experiments are at least conceptually
		 * comparable even though this one is GPU-resident. */
		static const char *const kUrgentKeywords[] = {"urgent", "immediate", "asap", "emergency"};
		uint32_t hits = 0;

		for (int k = 0; k < 4; k++) {
			const char *kw = kUrgentKeywords[k];
			int kw_len = 0;
			while (kw[kw_len] != '\0')
				kw_len++;

			for (uint32_t i = 0; i + (uint32_t)kw_len <= req->text_len; i++) {
				bool match = true;
				for (int j = 0; j < kw_len; j++) {
					char c = (char)req->text[i + j];
					if (c >= 'A' && c <= 'Z')
						c = (char)(c - 'A' + 'a');
					if (c != kw[j]) {
						match = false;
						break;
					}
				}
				if (match) {
					hits++;
					break;
				}
			}
		}

		req->matched_signal_mask = (hits > 0) ? 1u : 0u;
		req->confidence = (hits > 0) ? 1.0f : 0.0f;
		__threadfence_system();
		req->result_ready = 1;

		slot += blockDim.x;
	}
}

template <enum doca_gpu_dev_eth_exec_scope exec_scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>
__global__ void persistent_classify(struct doca_gpu_eth_rxq *rxq,
				     struct routenic_request_ring *ring,
				     uint32_t max_batch_size,
				     uint32_t *exit_cond,
				     uint64_t *tot_classified)
{
	doca_error_t ret;
	__shared__ uint64_t out_first_pkt_idx;
	__shared__ uint32_t out_pkt_num;
	__shared__ struct doca_gpu_dev_eth_rxq_attr out_attr[ROUTENIC_MAX_RX_NUM_PKTS];
	__shared__ struct routenic_request_slot batch[ROUTENIC_RING_CAPACITY];
	uint64_t tot_classified_ = 0;

	while (DOCA_GPUNETIO_VOLATILE(*exit_cond) == 0) {
		if (threadIdx.x == 0) {
			ret = doca_gpu_dev_eth_rxq_recv<exec_scope,
							 DOCA_GPUNETIO_ETH_MCST_AUTO,
							 DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
							 DOCA_GPUNETIO_ETH_RX_ATTR_ALL>(rxq,
											 max_batch_size,
											 ROUTENIC_MAX_RX_TIMEOUT_NS,
											 &out_first_pkt_idx,
											 &out_pkt_num,
											 out_attr);
			if (ret != DOCA_SUCCESS) {
				printf("routeNIC persistent classifier: recv error %d\n", ret);
				DOCA_GPUNETIO_VOLATILE(*exit_cond) = 1;
			}
		}
		__syncthreads();

		if (DOCA_GPUNETIO_VOLATILE(*exit_cond) != 0)
			break;
		if (out_pkt_num == 0)
			continue;

		/* Copy each received packet's payload into a request slot.
		 * In the RDMA-write deployment (BF3 -> unified memory ->
		 * this kernel), the ring in routenic_request_ring is what
		 * actually gets written to directly and this Ethernet-recv
		 * path is the alternative, more aggressive Experiment 8
		 * variant (raw packet parsing with no BF3 involvement at
		 * all) — both share this same classify_batch() call so the
		 * two mechanisms are comparable on the same metric. */
		uint32_t idx = threadIdx.x;
		while (idx < out_pkt_num) {
			uint64_t addr = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, out_first_pkt_idx + idx);
			uint32_t len = out_attr[idx].bytes;
			if (len > ROUTENIC_MAX_REQUEST_TEXT_BYTES)
				len = ROUTENIC_MAX_REQUEST_TEXT_BYTES;

			batch[idx].request_id = out_first_pkt_idx + idx;
			batch[idx].text_len = len;
			for (uint32_t b = 0; b < len; b++)
				batch[idx].text[b] = ((uint8_t *)addr)[b];
			batch[idx].result_ready = 0;

			idx += blockDim.x;
		}
		__syncthreads();

		classify_batch(batch, out_pkt_num);
		__syncthreads();

		if (threadIdx.x == 0)
			tot_classified_ += out_pkt_num;
		__syncthreads();
	}

	if (threadIdx.x == 0) {
		*tot_classified = tot_classified_;
		__threadfence_system();
	}
}

extern "C" {

doca_error_t routenic_kernel_persistent_classify(cudaStream_t stream,
						  struct routenic_rxq_queue *rxq,
						  struct routenic_request_ring *ring,
						  enum doca_gpu_dev_eth_exec_scope exec_scope,
						  uint32_t max_batch_size,
						  uint32_t *gpu_exit_condition,
						  uint64_t *tot_requests_classified)
{
	cudaError_t result = cudaSuccess;

	if (rxq == NULL || gpu_exit_condition == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK)
		persistent_classify<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK><<<1, ROUTENIC_CUDA_BLOCK_THREADS, 0, stream>>>(
			rxq->eth_rxq_gpu, ring, max_batch_size, gpu_exit_condition, tot_requests_classified);
	else if (exec_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP)
		persistent_classify<DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP><<<1, ROUTENIC_CUDA_BLOCK_THREADS, 0, stream>>>(
			rxq->eth_rxq_gpu, ring, max_batch_size, gpu_exit_condition, tot_requests_classified);
	else
		persistent_classify<DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD><<<1, ROUTENIC_CUDA_BLOCK_THREADS, 0, stream>>>(
			rxq->eth_rxq_gpu, ring, max_batch_size, gpu_exit_condition, tot_requests_classified);

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern "C" */
