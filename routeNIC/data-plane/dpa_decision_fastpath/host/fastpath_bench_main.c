/*
 * routeNIC — Experiment 1: real timed throughput benchmark for the DPA
 * fast-path kernel, against the SAME 4 real keyword rules the real router's
 * config/config.yaml defines and a real, large-scale, well-known query
 * corpus (see the --corpus argument) — so this and
 * routeNIC/control-plane/software_baseline_bench are a fair apples-to-apples
 * comparison, not two different workloads.
 *
 * Deliberately uses only the 4 rules that exist in the real
 * config/config.yaml (code_keywords, machine_learning, urgent_keywords,
 * fuzzy_sensitive_keywords) — matching realKeywordRules() in
 * control-plane/software_baseline_bench/keyword_bench_test.go exactly.
 *
 * Timing methodology, matched to what the Go benchmark measures: rule table
 * setup (H2D copy) happens once, outside the timed region — exactly like
 * NewKeywordClassifier() compiling its regexes once, outside b.ResetTimer().
 *
 * Usage: routenic_fastpath_bench <corpus-file> [batch_size] [max_queries]
 *   corpus-file: one query per line (e.g.
 *     control-plane/software_baseline_bench/data/chatbot_arena_queries.txt)
 *   batch_size: requests per batched kernel launch (default 128)
 *   max_queries: 0 = use the whole corpus file (default 0)
 */

#define _GNU_SOURCE
#include <errno.h>
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
 * rules, same keyword lists, same order. */
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

/* Loads one query per line from `path`. Returns a malloc'd array of
 * malloc'd strings (caller frees each string, then the array) and sets
 * *out_count. Trailing newline stripped; empty lines skipped. */
static char **load_corpus(const char *path, size_t *out_count, size_t max_queries)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "Failed to open corpus file %s: %s\n", path, strerror(errno));
		return NULL;
	}

	size_t cap = 1024, count = 0;
	char **queries = malloc(cap * sizeof(char *));
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t len;

	while ((len = getline(&line, &line_cap, f)) != -1) {
		if (len > 0 && line[len - 1] == '\n') {
			line[len - 1] = '\0';
			len--;
		}
		if (len == 0)
			continue;
		if (count == cap) {
			cap *= 2;
			queries = realloc(queries, cap * sizeof(char *));
		}
		queries[count] = strdup(line);
		count++;
		if (max_queries > 0 && count >= max_queries)
			break;
	}
	free(line);
	fclose(f);

	*out_count = count;
	return queries;
}

int main(int argc, char **argv)
{
	struct dpa_config cfg = {0};
	struct dpa_resources resources = {0};
	struct fastpath_keyword_rule rules[NUM_REAL_RULES];
	doca_dpa_dev_uintptr_t rules_dev_ptr = 0;
	doca_error_t result;
	struct doca_log_backend *sdk_log;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <corpus-file> [batch_size] [max_queries]\n", argv[0]);
		return EXIT_FAILURE;
	}
	const char *corpus_path = argv[1];
	uint32_t batch_size = (argc > 2) ? (uint32_t)atoi(argv[2]) : 128;
	size_t max_queries = (argc > 3) ? (size_t)atol(argv[3]) : 0;

	size_t num_queries = 0;
	char **queries = load_corpus(corpus_path, &num_queries, max_queries);
	if (queries == NULL || num_queries == 0) {
		fprintf(stderr, "No queries loaded from %s\n", corpus_path);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "Loaded %zu queries from %s\n", num_queries, corpus_path);

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

	/* --- one-time setup, NOT timed --- */
	build_real_rule_table(rules);
	result = doca_dpa_mem_alloc(resources.pf_dpa_ctx, sizeof(rules), &rules_dev_ptr);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;
	result = doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, rules_dev_ptr, rules, sizeof(rules));
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	doca_dpa_dev_uintptr_t *text_dev_ptrs = malloc(num_queries * sizeof(doca_dpa_dev_uintptr_t));
	doca_dpa_dev_uintptr_t *out_dev_ptrs = malloc(num_queries * sizeof(doca_dpa_dev_uintptr_t));
	uint32_t *text_lens = malloc(num_queries * sizeof(uint32_t));

	for (size_t q = 0; q < num_queries; q++) {
		text_lens[q] = (uint32_t)strlen(queries[q]);
		if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, text_lens[q], &text_dev_ptrs[q]) != DOCA_SUCCESS)
			return EXIT_FAILURE;
		if (doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, text_dev_ptrs[q], queries[q], text_lens[q]) != DOCA_SUCCESS)
			return EXIT_FAILURE;
		if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, FASTPATH_MAX_RULES, &out_dev_ptrs[q]) != DOCA_SUCCESS)
			return EXIT_FAILURE;
	}

	/* --- single-request-per-launch pass over the whole corpus once --- */
	double *latencies_ns = malloc(sizeof(double) * num_queries);
	struct doca_sync_event *comp_event = NULL;
	result = routenic_fastpath_create_reusable_completion_event(&resources, &comp_event);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	fprintf(stderr, "Running single-request-per-launch pass (%zu launches)...\n", num_queries);
	uint64_t target_value = 0;
	double t_start = now_ns();
	for (size_t q = 0; q < num_queries; q++) {
		uint8_t out_matched[FASTPATH_MAX_RULES] = {0};
		double t0 = now_ns();

		target_value++;
		result = routenic_fastpath_launch_reuse_event(
			&resources, comp_event, target_value, text_dev_ptrs[q], text_lens[q], rules_dev_ptr, NUM_REAL_RULES, out_dev_ptrs[q]);
		if (result == DOCA_SUCCESS)
			doca_dpa_d2h_memcpy(resources.pf_dpa_ctx, out_matched, out_dev_ptrs[q], FASTPATH_MAX_RULES);

		latencies_ns[q] = now_ns() - t0;
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Launch failed on query %zu: %s", q, doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	}
	double t_end = now_ns();
	doca_sync_event_destroy(comp_event);

	qsort(latencies_ns, num_queries, sizeof(double), cmp_double);
	double sum = 0;
	for (size_t i = 0; i < num_queries; i++)
		sum += latencies_ns[i];
	double mean = sum / (double)num_queries;
	double total_s = (t_end - t_start) / 1e9;

	printf("routeNIC Experiment 1 bench, single-request-per-launch (real DPA hardware, %zu samples, corpus=%s):\n",
	       num_queries,
	       corpus_path);
	printf("  mean=%.0f ns  p50=%.0f ns  p95=%.0f ns  p99=%.0f ns\n",
	       mean,
	       latencies_ns[num_queries / 2],
	       latencies_ns[(size_t)(num_queries * 0.95)],
	       latencies_ns[(size_t)(num_queries * 0.99)]);
	printf("  wall clock=%.3fs  throughput=%.1f launches/sec\n", total_s, (double)num_queries / total_s);

	/* --- batched mode: chunk the whole corpus into batch_size-sized
	 * launches, one DPA thread per request in each batch. --- */
	size_t num_batches = (num_queries + batch_size - 1) / batch_size;
	uint64_t *text_addrs_host = malloc(batch_size * sizeof(uint64_t));
	uint32_t *text_lens_batch_host = malloc(batch_size * sizeof(uint32_t));
	uint64_t *out_addrs_host = malloc(batch_size * sizeof(uint64_t));
	doca_dpa_dev_uintptr_t text_addrs_arr_dev = 0, text_lens_arr_dev = 0, out_addrs_arr_dev = 0;
	if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, batch_size * sizeof(uint64_t), &text_addrs_arr_dev) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, batch_size * sizeof(uint32_t), &text_lens_arr_dev) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_dpa_mem_alloc(resources.pf_dpa_ctx, batch_size * sizeof(uint64_t), &out_addrs_arr_dev) != DOCA_SUCCESS)
		return EXIT_FAILURE;

	struct doca_sync_event *batch_comp_event = NULL;
	result = routenic_fastpath_create_reusable_completion_event(&resources, &batch_comp_event);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	double *batch_latencies_ns = malloc(sizeof(double) * num_batches);
	uint64_t batch_target = 0;

	fprintf(stderr, "Running batched pass (%zu batches of %u)...\n", num_batches, batch_size);
	double bt_start = now_ns();
	size_t total_requests_sent = 0;
	for (size_t b = 0; b < num_batches; b++) {
		size_t base = b * batch_size;
		uint32_t this_batch = (uint32_t)((base + batch_size <= num_queries) ? batch_size : (num_queries - base));

		for (uint32_t i = 0; i < this_batch; i++) {
			text_addrs_host[i] = text_dev_ptrs[base + i];
			text_lens_batch_host[i] = text_lens[base + i];
			out_addrs_host[i] = out_dev_ptrs[base + i];
		}
		doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, text_addrs_arr_dev, text_addrs_host, this_batch * sizeof(uint64_t));
		doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, text_lens_arr_dev, text_lens_batch_host, this_batch * sizeof(uint32_t));
		doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, out_addrs_arr_dev, out_addrs_host, this_batch * sizeof(uint64_t));

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
								      this_batch);
		batch_latencies_ns[b] = now_ns() - t0;
		total_requests_sent += this_batch;
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Batched launch failed on batch %zu (size %u): %s", b, this_batch, doca_error_get_descr(result));
			return EXIT_FAILURE;
		}
	}
	double bt_end = now_ns();
	doca_sync_event_destroy(batch_comp_event);

	qsort(batch_latencies_ns, num_batches, sizeof(double), cmp_double);
	double bsum = 0;
	for (size_t i = 0; i < num_batches; i++)
		bsum += batch_latencies_ns[i];
	double b_mean_per_launch = bsum / (double)num_batches;
	double b_total_s = (bt_end - bt_start) / 1e9;

	printf("routeNIC Experiment 1 bench, batched (batch_size=%u, real DPA hardware, %zu batches, %zu requests, corpus=%s):\n",
	       batch_size,
	       num_batches,
	       total_requests_sent,
	       corpus_path);
	printf("  mean=%.0f ns/launch  (%.0f ns/request amortized)  p50=%.0f ns/launch  p99=%.0f ns/launch\n",
	       b_mean_per_launch,
	       b_mean_per_launch / (double)batch_size,
	       batch_latencies_ns[num_batches / 2],
	       batch_latencies_ns[(size_t)(num_batches * 0.99)]);
	printf("  wall clock=%.3fs  throughput=%.1f requests/sec\n", b_total_s, (double)total_requests_sent / b_total_s);

	return EXIT_SUCCESS;
}
