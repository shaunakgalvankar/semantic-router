/*
 * routeNIC — Experiment 1: real timed throughput benchmark for the DPA
 * fast-path kernel, against the SAME 4 real keyword rules and the SAME
 * 34-query corpus routeNIC/control-plane/software_baseline_bench benchmarks
 * on the real router's own Classify() path — so the two numbers are a fair
 * apples-to-apples comparison, not two different workloads.
 *
 * Deliberately uses only the 4 rules that exist in the real
 * config/config.yaml (code_keywords, machine_learning, urgent_keywords,
 * fuzzy_sensitive_keywords) — NOT the added case_sensitive_secrets fixture
 * fastpath_launcher.c's routenic_build_rule_table() also builds, since the
 * software benchmark doesn't have that rule either and comparing against a
 * rule table the other side doesn't have would not be fair.
 *
 * Timing methodology, matched to what the Go benchmark measures: rule table
 * setup (H2D copy) happens once, outside the timed region — exactly like
 * NewKeywordClassifier() compiling its regexes once, outside b.ResetTimer().
 * Each timed iteration does exactly what one Classify(text) call does on the
 * software side: hand over one request's text and get one result back. Here
 * that's an H2D copy of the request text + kernel launch + wait-for-completion
 * + D2H copy of the result — the real, full round-trip cost of invoking the
 * DPA fast path once from the host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dpa.h>

#include "../dpa_kernel/fastpath_shared.h"
#include "dpa_common.h"

DOCA_LOG_REGISTER(ROUTENIC_FASTPATH::BENCH_MAIN);

doca_error_t routenic_fastpath_create_reusable_completion_event(struct dpa_resources *resources,
								  struct doca_sync_event **out_comp_event);
doca_error_t routenic_fastpath_launch_reuse_event(struct dpa_resources *resources,
						    struct doca_sync_event *comp_event,
						    uint64_t target_value,
						    uint64_t request_text_dpa_addr,
						    uint32_t request_len,
						    uint64_t rules_dpa_addr,
						    uint32_t num_rules,
						    uint64_t out_matched_dpa_addr);
doca_error_t routenic_fastpath_launch_batch_reuse_event(struct dpa_resources *resources,
							  struct doca_sync_event *comp_event,
							  uint64_t target_value,
							  uint64_t text_addrs_arr,
							  uint64_t text_lens_arr,
							  uint64_t rules_dpa_addr,
							  uint32_t num_rules,
							  uint64_t out_matched_addrs_arr,
							  uint32_t batch_size);

#define NUM_REAL_RULES 4

/* Identical to realKeywordRules() in
 * control-plane/software_baseline_bench/keyword_bench_test.go — same 4
 * rules, same keyword lists, same order. The DPA kernel doesn't distinguish
 * bm25/ngram/regex methods (it only does case-folded exact-substring OR),
 * so only the keyword lists and operator matter here; see
 * control-plane/accuracy_study/ for how much that simplification costs in
 * agreement with the real per-method software behavior. */
static doca_error_t build_real_rule_table(struct fastpath_keyword_rule *out_rules)
{
	struct {
		const char *keywords[FASTPATH_MAX_KEYWORDS];
	} table[NUM_REAL_RULES] = {
		{{"code", "function", "debug", "algorithm", "refactor", NULL}},
		{{"machine learning", "model training", "gradient", "neural network", "classifier", NULL}},
		{{"urgent", "immediate", "asap", "emergency", NULL}},
		{{"social security", "credit card", "password", "api key", NULL}},
	};

	for (uint32_t r = 0; r < NUM_REAL_RULES; r++) {
		out_rules[r].operator= FASTPATH_OP_OR;
		out_rules[r].num_keywords = 0;
		for (uint32_t k = 0; k < FASTPATH_MAX_KEYWORDS && table[r].keywords[k] != NULL; k++) {
			size_t len = strlen(table[r].keywords[k]);
			memcpy(out_rules[r].keywords[k], table[r].keywords[k], len + 1);
			out_rules[r].keyword_lens[k] = (uint32_t)len;
			out_rules[r].num_keywords++;
		}
	}
	return DOCA_SUCCESS;
}

/* Identical order and text to realQueries in
 * control-plane/software_baseline_bench/keyword_bench_test.go. */
static const char *const QUERIES[] = {
	"please review this code for bugs",
	"can you debug this function for me",
	"explain gradient descent in machine learning",
	"this is urgent, please respond immediately",
	"what is my credit card balance",
	"AWS_SECRET is exposed in the log file",
	"what's the weather like today",
	"tell me a joke about cats",
	"how do I bake sourdough bread",
	"recommend a good science fiction novel",
	"I am debugging the algorithm right now",
	"we are refactoring the codebase this week",
	"the model was trained on a large dataset",
	"our classifiers need retraining",
	"gradient updates during training are unstable",
	"this is urgnet, please help",
	"we have an emergancy on the production system",
	"asp, need this done now",
	"immediat action required",
	"I forgot my passwrod again",
	"please don't share your cerdit card number",
	"my social securty number was stolen",
	"the api ky was rotated yesterday",
	"AWS_SECRET must never be logged",
	"aws_secret must never be logged",
	"API_KEY rotation policy",
	"api_key rotation policy",
	"PRIVATE_KEY leaked in the repo",
	"private_key leaked in the repo",
	"urgent: server is down, respond immediately",
	"urgent: please update my credit card on file",
	"Hi team, this is urgent \xe2\x80\x94 the neural network training job crashed and "
	"I need someone to debug the gradient computation asap before the demo.",
	"Not urgent, just wondering if anyone has refactored the classifier code "
	"recently, no rush at all.",
	"Emergency: the password reset endpoint is leaking session tokens, "
	"please look at this immediately.",
};
#define NUM_QUERIES (sizeof(QUERIES) / sizeof(QUERIES[0]))

static double now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

int main(int argc, char **argv)
{
	struct dpa_config cfg = {0};
	struct dpa_resources resources = {0};
	struct fastpath_keyword_rule rules[NUM_REAL_RULES];
	doca_dpa_dev_uintptr_t rules_dev_ptr = 0;
	doca_dpa_dev_uintptr_t text_dev_ptrs[NUM_QUERIES];
	doca_dpa_dev_uintptr_t out_dev_ptrs[NUM_QUERIES];
	uint32_t iterations = (argc > 1) ? (uint32_t)atoi(argv[1]) : 200;
	double *latencies_ns;
	doca_error_t result;
	struct doca_log_backend *sdk_log;

	if (doca_log_backend_create_standard() != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

	strcpy(cfg.pf_device_name, "mlx5_0");
	strcpy(cfg.rdma_device_name, DEVICE_DEFAULT_NAME);

	result = allocate_dpa_resources(&cfg, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DPA resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	/* --- one-time setup, NOT timed (matches NewKeywordClassifier() being
	 * outside the Go benchmark's timed region) --- */
	build_real_rule_table(rules);
	result = doca_dpa_mem_alloc(resources.pf_dpa_ctx, sizeof(rules), &rules_dev_ptr);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, rules_dev_ptr, rules, sizeof(rules));
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	for (size_t q = 0; q < NUM_QUERIES; q++) {
		uint32_t len = (uint32_t)strlen(QUERIES[q]);
		if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, len, &text_dev_ptrs[q]) != DOCA_SUCCESS)
			return EXIT_FAILURE;
		if (doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, text_dev_ptrs[q], (void *)QUERIES[q], len) != DOCA_SUCCESS)
			return EXIT_FAILURE;
		if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, FASTPATH_MAX_RULES, &out_dev_ptrs[q]) != DOCA_SUCCESS)
			return EXIT_FAILURE;
	}

	latencies_ns = malloc(sizeof(double) * NUM_QUERIES * iterations);
	if (latencies_ns == NULL)
		return EXIT_FAILURE;

	/* Completion event created ONCE, outside the timed region — see
	 * routenic_fastpath_launch_reuse_event()'s comment in
	 * fastpath_launcher.c for why this specific change is the single
	 * biggest lever on this benchmark's outcome. */
	struct doca_sync_event *comp_event = NULL;
	result = routenic_fastpath_create_reusable_completion_event(&resources, &comp_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create reusable completion event: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	printf("routeNIC Experiment 1 bench: %u iterations x %zu queries = %zu real DPA launches\n",
	       iterations,
	       NUM_QUERIES,
	       (size_t)iterations * NUM_QUERIES);

	/* --- timed region: one launch+wait+D2H per call (event reused, not
	 * recreated), exactly what Classify(text) costs on the software side --- */
	size_t sample_idx = 0;
	uint64_t target_value = 0;
	double t_start = now_ns();
	for (uint32_t iter = 0; iter < iterations; iter++) {
		for (size_t q = 0; q < NUM_QUERIES; q++) {
			uint8_t out_matched[FASTPATH_MAX_RULES] = {0};
			double t0 = now_ns();

			target_value++;
			result = routenic_fastpath_launch_reuse_event(&resources,
									comp_event,
									target_value,
									text_dev_ptrs[q],
									(uint32_t)strlen(QUERIES[q]),
									rules_dev_ptr,
									NUM_REAL_RULES,
									out_dev_ptrs[q]);
			if (result == DOCA_SUCCESS)
				doca_dpa_d2h_memcpy(resources.pf_dpa_ctx, out_matched, out_dev_ptrs[q], FASTPATH_MAX_RULES);

			double t1 = now_ns();
			latencies_ns[sample_idx++] = t1 - t0;

			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Launch failed on query %zu: %s", q, doca_error_get_descr(result));
				return EXIT_FAILURE;
			}
		}
	}
	double t_end = now_ns();
	doca_sync_event_destroy(comp_event);

	qsort(latencies_ns, sample_idx, sizeof(double), cmp_double);
	double sum = 0;
	for (size_t i = 0; i < sample_idx; i++)
		sum += latencies_ns[i];
	double mean = sum / (double)sample_idx;
	double p50 = latencies_ns[sample_idx / 2];
	double p95 = latencies_ns[(size_t)(sample_idx * 0.95)];
	double p99 = latencies_ns[(size_t)(sample_idx * 0.99)];
	double total_s = (t_end - t_start) / 1e9;
	double throughput = (double)sample_idx / total_s;

	printf("routeNIC Experiment 1 bench, single-request-per-launch (real DPA hardware, %zu samples):\n", sample_idx);
	printf("  mean=%.0f ns  p50=%.0f ns  p95=%.0f ns  p99=%.0f ns\n", mean, p50, p95, p99);
	printf("  wall clock=%.3fs  throughput=%.1f launches/sec\n", total_s, throughput);

	/* --- batched mode: all NUM_QUERIES requests in ONE kernel launch,
	 * one DPA thread per request (routenic_fastpath_evaluate_batch). ---
	 * Second real optimization on top of the reused completion event:
	 * amortizes per-launch dispatch overhead across the whole batch
	 * instead of paying it once per request. */
	uint64_t text_addrs_host[NUM_QUERIES];
	uint32_t text_lens_host[NUM_QUERIES];
	uint64_t out_addrs_host[NUM_QUERIES];
	for (size_t q = 0; q < NUM_QUERIES; q++) {
		text_addrs_host[q] = text_dev_ptrs[q];
		text_lens_host[q] = (uint32_t)strlen(QUERIES[q]);
		out_addrs_host[q] = out_dev_ptrs[q];
	}

	doca_dpa_dev_uintptr_t text_addrs_arr_dev = 0, text_lens_arr_dev = 0, out_addrs_arr_dev = 0;
	if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, sizeof(text_addrs_host), &text_addrs_arr_dev) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, sizeof(text_lens_host), &text_lens_arr_dev) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, sizeof(out_addrs_host), &out_addrs_arr_dev) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, text_addrs_arr_dev, text_addrs_host, sizeof(text_addrs_host));
	doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, text_lens_arr_dev, text_lens_host, sizeof(text_lens_host));
	doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, out_addrs_arr_dev, out_addrs_host, sizeof(out_addrs_host));

	struct doca_sync_event *batch_comp_event = NULL;
	result = routenic_fastpath_create_reusable_completion_event(&resources, &batch_comp_event);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	double *batch_latencies_ns = malloc(sizeof(double) * iterations);
	if (batch_latencies_ns == NULL)
		return EXIT_FAILURE;

	uint64_t batch_target = 0;
	double bt_start = now_ns();
	for (uint32_t iter = 0; iter < iterations; iter++) {
		double t0 = now_ns();
		batch_target++;
		result = routenic_fastpath_launch_batch_reuse_event(&resources,
								      batch_comp_event,
								      batch_target,
								      text_addrs_arr_dev,
								      text_lens_arr_dev,
								      rules_dev_ptr,
								      NUM_REAL_RULES,
								      out_addrs_arr_dev,
								      NUM_QUERIES);
		double t1 = now_ns();
		batch_latencies_ns[iter] = t1 - t0;
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Batched launch failed on iteration %u: %s", iter, doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	}
	double bt_end = now_ns();
	doca_sync_event_destroy(batch_comp_event);

	qsort(batch_latencies_ns, iterations, sizeof(double), cmp_double);
	double bsum = 0;
	for (uint32_t i = 0; i < iterations; i++)
		bsum += batch_latencies_ns[i];
	double b_mean_per_launch = bsum / (double)iterations;
	double b_mean_per_request = b_mean_per_launch / (double)NUM_QUERIES;
	double b_total_s = (bt_end - bt_start) / 1e9;
	double b_throughput_requests = ((double)iterations * NUM_QUERIES) / b_total_s;

	printf("routeNIC Experiment 1 bench, batched (%zu requests/launch, real DPA hardware, %u launches):\n",
	       (size_t)NUM_QUERIES,
	       iterations);
	printf("  mean=%.0f ns/launch  (%.0f ns/request amortized)  p50=%.0f ns/launch\n",
	       b_mean_per_launch,
	       b_mean_per_request,
	       batch_latencies_ns[iterations / 2]);
	printf("  wall clock=%.3fs  throughput=%.1f requests/sec\n", b_total_s, b_throughput_requests);

	return EXIT_SUCCESS;
}
