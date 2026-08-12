/*
 * routeNIC — Experiment 1: host-side launcher for the DPA fast-path kernel.
 *
 * Generic DPA bootstrap (opening the doca_dev, creating the doca_dpa
 * context, creating the sync events) is identical across every DOCA DPA
 * sample on this host and is intentionally NOT re-derived here — see
 * `create_doca_dpa_context()`/`create_doca_dpa_wait_sync_event()`/
 * `create_doca_dpa_completion_sync_event()` in
 * /opt/mellanox/doca/samples/doca_dpa/dpa_common.c, which every sample in
 * that tree (including dpa_kernel_launch, whose launch call this file
 * mirrors) links against. Re-implementing that boilerplate per-experiment
 * would just be a worse copy of code already proven correct on this host;
 * this file focuses on what's actually routeNIC-specific: marshaling the
 * keyword ruleset into `struct fastpath_keyword_rule[]` and the launch call
 * itself.
 */

#include <string.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dpa.h>

#include "../dpa_kernel/fastpath_shared.h"
/* struct dpa_resources and the create_doca_dpa_*_sync_event() helpers below
 * come from /opt/mellanox/doca/samples/doca_dpa/dpa_common.h — build this
 * file with that directory on the include path (see build_dpa_kernel.sh),
 * exactly as every other DOCA DPA sample on this host already does. */
#include "dpa_common.h"

DOCA_LOG_REGISTER(ROUTENIC_FASTPATH::LAUNCHER);

/* Declares the DPA kernel entry point as a host-callable function pointer,
 * per doca_dpa_kernel_launch_update_set()'s documented convention
 * (doca_dpa.h:452-457): the device symbol name must match exactly. */
extern doca_dpa_func_t routenic_fastpath_evaluate;

/*
 * Builds the fixed rule table the kernel evaluates, from the same 5 rules
 * control-plane/accuracy_study/workload.py uses — kept in sync by hand
 * rather than a shared schema for now (see the experiment README's "known
 * gaps" section: a real deployment would generate this table from the
 * router's actual canonical config instead of duplicating it here).
 */
static doca_error_t routenic_build_rule_table(struct fastpath_keyword_rule *out_rules, uint32_t *out_num_rules)
{
	struct {
		enum fastpath_operator op;
		const char *keywords[FASTPATH_MAX_KEYWORDS];
	} table[] = {
		{FASTPATH_OP_OR, {"code", "function", "debug", "algorithm", "refactor", NULL}},
		{FASTPATH_OP_OR,
		 {"machine learning", "model training", "gradient", "neural network", "classifier", NULL}},
		{FASTPATH_OP_OR, {"urgent", "immediate", "asap", "emergency", NULL}},
		{FASTPATH_OP_OR, {"social security", "credit card", "password", "api key", NULL}},
		{FASTPATH_OP_OR, {"aws_secret", "api_key", "private_key", NULL}},
	};
	uint32_t n = sizeof(table) / sizeof(table[0]);

	if (n > FASTPATH_MAX_RULES)
		return DOCA_ERROR_INVALID_VALUE;

	for (uint32_t r = 0; r < n; r++) {
		out_rules[r].operator= table[r].op;
		out_rules[r].num_keywords = 0;
		for (uint32_t k = 0; k < FASTPATH_MAX_KEYWORDS && table[r].keywords[k] != NULL; k++) {
			size_t len = strlen(table[r].keywords[k]);
			if (len >= FASTPATH_MAX_KEYWORD_LEN)
				return DOCA_ERROR_INVALID_VALUE;
			memcpy(out_rules[r].keywords[k], table[r].keywords[k], len + 1);
			out_rules[r].keyword_lens[k] = (uint32_t)len;
			out_rules[r].num_keywords++;
		}
	}
	*out_num_rules = n;
	return DOCA_SUCCESS;
}

/*
 * Launches the fast-path kernel once against a single request buffer
 * already resident in DPA-accessible memory, and blocks for completion.
 * `resources` is the shared DPA context/device bootstrap (see file header);
 * `request_text`/`request_len` identify the request to evaluate;
 * `out_matched` receives one byte per rule (see fastpath_kernel_dev.c).
 *
 * This single-request-per-launch shape is a starting point for correctness
 * validation, not the throughput-oriented design — see the experiment
 * README for how this composes with an RDMA-triggered, batched invocation
 * once the mechanism above is validated end-to-end on hardware.
 */
doca_error_t routenic_fastpath_launch(struct dpa_resources *resources,
				       uint64_t request_text_dpa_addr,
				       uint32_t request_len,
				       uint64_t rules_dpa_addr,
				       uint64_t out_matched_dpa_addr)
{
	struct fastpath_keyword_rule rules[FASTPATH_MAX_RULES];
	uint32_t num_rules = 0;
	struct doca_sync_event *wait_event = NULL;
	struct doca_sync_event *comp_event = NULL;
	doca_error_t result, tmp_result;

	result = routenic_build_rule_table(rules, &num_rules);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to build fast-path rule table: %s", doca_error_get_descr(result));
		return result;
	}

	/* Caller is responsible for having already RDMA-written / DMA-copied
	 * `rules` to rules_dpa_addr in DPA-accessible memory before this
	 * call — that memory-placement step is deliberately factored out of
	 * this launcher so it can be swapped between "host writes it once at
	 * startup" (this experiment) and "BF3 rewrites it on config reload"
	 * (a natural extension) without touching the launch call below. */

	result = create_doca_dpa_wait_sync_event(resources->pf_dpa_ctx, resources->pf_doca_device, &wait_event);
	if (result != DOCA_SUCCESS)
		return result;

	result = create_doca_dpa_completion_sync_event(resources->pf_dpa_ctx, resources->pf_doca_device, &comp_event, NULL);
	if (result != DOCA_SUCCESS)
		goto destroy_wait_event;

	result = doca_dpa_kernel_launch_update_set(resources->pf_dpa_ctx,
						    NULL, /* no wait condition: run immediately */
						    0,
						    comp_event,
						    1,
						    1, /* one DPA thread is enough for one request */
						    &routenic_fastpath_evaluate,
						    request_text_dpa_addr,
						    request_len,
						    rules_dpa_addr,
						    num_rules,
						    out_matched_dpa_addr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to launch routenic_fastpath_evaluate: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	result = doca_sync_event_wait_gt(comp_event, 0, SYNC_EVENT_MASK_FFS);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed waiting for fast-path kernel completion: %s", doca_error_get_descr(result));

destroy_comp_event:
	tmp_result = doca_sync_event_destroy(comp_event);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(result, tmp_result);
destroy_wait_event:
	tmp_result = doca_sync_event_destroy(wait_event);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	return result;
}
