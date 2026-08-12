/*
 * routeNIC — Experiment 1: real end-to-end test driver for the DPA
 * fast-path kernel. Unlike everything else in this experiment up to this
 * point, this file is meant to actually run on the BlueField-3 (not just
 * compile) and report genuine pass/fail against real DPA hardware.
 *
 * Bootstrap pattern (doca_argp-free device open via allocate_dpa_resources)
 * mirrors dpa_kernel_launch_main.c exactly, minus the doca_argp CLI-parsing
 * layer that sample uses only to let the user override the device name —
 * this driver hardcodes it instead, since it has exactly one job.
 *
 * Test methodology: six fixed request strings, each chosen to hit exactly
 * one of the five keyword rules (or none), with expected out_matched
 * bitmaps computed by hand from the same rule table
 * routenic_build_rule_table() builds. This is the same rule table and the
 * same case-folded exact-substring semantics
 * control-plane/accuracy_study/dpa_adapted.py models in software — a real
 * hardware run of this driver is the actual validation that the accuracy
 * study's "folded case mode" numbers describe what this kernel truly does,
 * not just what it was designed to do.
 */

#include <stdio.h>
#include <string.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dpa.h>

#include "../dpa_kernel/fastpath_shared.h"
#include "dpa_common.h"

DOCA_LOG_REGISTER(ROUTENIC_FASTPATH::MAIN);

doca_error_t routenic_build_rule_table(struct fastpath_keyword_rule *out_rules, uint32_t *out_num_rules);
doca_error_t routenic_fastpath_launch(struct dpa_resources *resources,
				       uint64_t request_text_dpa_addr,
				       uint32_t request_len,
				       uint64_t rules_dpa_addr,
				       uint32_t num_rules,
				       uint64_t out_matched_dpa_addr);

struct fastpath_test_case {
	const char *request_text;
	uint8_t expected_matched[FASTPATH_MAX_RULES]; /* one byte per rule, 0/1 */
};

/* Expected results hand-derived from routenic_build_rule_table()'s five
 * rules (code/ml/urgent/sensitive/secrets, all OR, all case-folded exact
 * substring) — see that function in fastpath_launcher.c for the literal
 * keyword lists these were computed against. */
static const struct fastpath_test_case TEST_CASES[] = {
	{"please help debug this function, thanks", {1, 0, 0, 0, 0}},
	{"let's discuss machine learning classifier design", {0, 1, 0, 0, 0}},
	{"this is urgent, need help asap", {0, 0, 1, 0, 0}},
	{"my credit card was declined", {0, 0, 0, 1, 0}},
	{"aws_secret must never be logged", {0, 0, 0, 0, 1}},
	{"just a normal question about the weather", {0, 0, 0, 0, 0}},
};
#define NUM_TEST_CASES (sizeof(TEST_CASES) / sizeof(TEST_CASES[0]))

static doca_error_t run_one_test(struct dpa_resources *resources,
				  doca_dpa_dev_uintptr_t rules_dev_ptr,
				  uint32_t num_rules,
				  const struct fastpath_test_case *tc,
				  int *out_pass)
{
	doca_dpa_dev_uintptr_t text_dev_ptr = 0;
	doca_dpa_dev_uintptr_t out_dev_ptr = 0;
	uint8_t out_matched[FASTPATH_MAX_RULES] = {0};
	uint32_t text_len = (uint32_t)strlen(tc->request_text);
	doca_error_t result, tmp_result;

	result = doca_dpa_mem_alloc(resources->pf_dpa_ctx, text_len, &text_dev_ptr);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_dpa_mem_alloc(resources->pf_dpa_ctx, FASTPATH_MAX_RULES, &out_dev_ptr);
	if (result != DOCA_SUCCESS)
		goto free_text;

	result = doca_dpa_h2d_memcpy(resources->pf_dpa_ctx, text_dev_ptr, (void *)tc->request_text, text_len);
	if (result != DOCA_SUCCESS)
		goto free_out;
	result = doca_dpa_memset(resources->pf_dpa_ctx, out_dev_ptr, 0, FASTPATH_MAX_RULES);
	if (result != DOCA_SUCCESS)
		goto free_out;

	result = routenic_fastpath_launch(
		resources, text_dev_ptr, text_len, rules_dev_ptr, num_rules, out_dev_ptr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Kernel launch failed for %s: %s", tc->request_text, doca_error_get_descr(result));
		goto free_out;
	}

	result = doca_dpa_d2h_memcpy(resources->pf_dpa_ctx, out_matched, out_dev_ptr, FASTPATH_MAX_RULES);
	if (result != DOCA_SUCCESS)
		goto free_out;

	*out_pass = (memcmp(out_matched, tc->expected_matched, num_rules) == 0);
	printf("  %-55s expected=[%d,%d,%d,%d,%d] got=[%d,%d,%d,%d,%d]  %s\n",
	       tc->request_text,
	       tc->expected_matched[0], tc->expected_matched[1], tc->expected_matched[2],
	       tc->expected_matched[3], tc->expected_matched[4],
	       out_matched[0], out_matched[1], out_matched[2], out_matched[3], out_matched[4],
	       *out_pass ? "PASS" : "FAIL");

free_out:
	tmp_result = doca_dpa_mem_free(resources->pf_dpa_ctx, out_dev_ptr);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(result, tmp_result);
free_text:
	tmp_result = doca_dpa_mem_free(resources->pf_dpa_ctx, text_dev_ptr);
	if (tmp_result != DOCA_SUCCESS)
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	return result;
}

int main(void)
{
	struct dpa_config cfg = {0};
	struct dpa_resources resources = {0};
	struct fastpath_keyword_rule rules[FASTPATH_MAX_RULES];
	uint32_t num_rules = 0;
	doca_dpa_dev_uintptr_t rules_dev_ptr = 0;
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	int total_pass = 0;

	if (doca_log_backend_create_standard() != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

	/* Real device name on this BF3, confirmed via `ibv_devinfo -l`. Leave
	 * rdma_device_name as DEVICE_DEFAULT_NAME ("NOT_SET"): this kernel
	 * doesn't use RDMA at all, and open_dpa_device() (dpa_common.c) hard
	 * errors if pf_device_name and rdma_device_name are both explicitly
	 * set to the same real name — discovered by actually running this on
	 * the BF3, not by reading the source alone. */
	strcpy(cfg.pf_device_name, "mlx5_0");
	strcpy(cfg.rdma_device_name, DEVICE_DEFAULT_NAME);

	result = allocate_dpa_resources(&cfg, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DPA resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = routenic_build_rule_table(rules, &num_rules);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to build rule table: %s", doca_error_get_descr(result));
		goto cleanup_resources;
	}

	result = doca_dpa_mem_alloc(resources.pf_dpa_ctx, sizeof(rules[0]) * num_rules, &rules_dev_ptr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DPA memory for rule table: %s", doca_error_get_descr(result));
		goto cleanup_resources;
	}
	result = doca_dpa_h2d_memcpy(resources.pf_dpa_ctx, rules_dev_ptr, rules, sizeof(rules[0]) * num_rules);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to H2D copy rule table: %s", doca_error_get_descr(result));
		goto free_rules;
	}

	printf("routeNIC Experiment 1: real DPA fast-path kernel test (%u rules, %zu requests)\n",
	       num_rules,
	       NUM_TEST_CASES);
	for (size_t i = 0; i < NUM_TEST_CASES; i++) {
		int pass = 0;
		result = run_one_test(&resources, rules_dev_ptr, num_rules, &TEST_CASES[i], &pass);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Test case %zu errored: %s", i, doca_error_get_descr(result));
			continue;
		}
		total_pass += pass;
	}
	printf("routeNIC Experiment 1: %d/%zu real DPA hardware test cases passed\n", total_pass, NUM_TEST_CASES);

	exit_status = (total_pass == (int)NUM_TEST_CASES) ? EXIT_SUCCESS : EXIT_FAILURE;

free_rules:
	doca_dpa_mem_free(resources.pf_dpa_ctx, rules_dev_ptr);
cleanup_resources:
	destroy_dpa_resources(&resources);
	return exit_status;
}
