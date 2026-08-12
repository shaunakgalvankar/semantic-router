/*
 * routeNIC — Experiment 2: real hardware bring-up test for the persistent
 * GPUNetIO classifier kernel.
 *
 * This is a bounded run (cfg.bounded_run_seconds), not the unbounded
 * "launch once, run forever" shape the rest of the experiment describes —
 * bring-up on real hardware needs a guaranteed-terminating test before
 * anything long-running is trusted. See gpunetio_classifier_launcher.c's
 * routenic_watchdog_thread() for how the bound is enforced without touching
 * the per-request path itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <doca_error.h>
#include <doca_log.h>

#include "../gpunetio_classifier_common.h"

DOCA_LOG_REGISTER(ROUTENIC_GPUNETIO_CLASSIFIER::TEST_MAIN);

int main(int argc, char **argv)
{
	struct doca_log_backend *sdk_log;
	struct routenic_classifier_cfg cfg = {0};
	doca_error_t result;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <gpu-pci-addr> <nic-pci-addr>\n", argv[0]);
		fprintf(stderr, "  e.g.: %s 000f:01:00.0 0000:01:00.0\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (doca_log_backend_create_standard() != DOCA_SUCCESS)
		return EXIT_FAILURE;
	if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) != DOCA_SUCCESS)
		return EXIT_FAILURE;
	doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

	strncpy(cfg.gpu_pcie_addr, argv[1], sizeof(cfg.gpu_pcie_addr) - 1);
	strncpy(cfg.nic_pcie_addr, argv[2], sizeof(cfg.nic_pcie_addr) - 1);
	cfg.exec_scope = DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK;
	cfg.max_batch_size = 32;
	cfg.bounded_run_seconds = 12;

	printf("routeNIC Experiment 2: real hardware bring-up test\n");
	printf("  GPU: %s   NIC: %s   bound: %us\n", cfg.gpu_pcie_addr, cfg.nic_pcie_addr, cfg.bounded_run_seconds);
	printf("  Send a real UDP packet to this NIC's IP during the run to exercise classify_batch() end to end.\n");

	result = routenic_classifier_launch(&cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("routenic_classifier_launch failed: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	printf("routeNIC Experiment 2: test finished successfully (kernel launched, ran bounded, exited cleanly)\n");
	return EXIT_SUCCESS;
}
