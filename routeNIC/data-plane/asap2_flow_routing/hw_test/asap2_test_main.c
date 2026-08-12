#include <stdlib.h>

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>
#include <dpdk_utils.h>

DOCA_LOG_REGISTER(ROUTENIC_ASAP2_TEST_MAIN);

doca_error_t routenic_asap2_test(int nb_queues);

int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct flow_dev_ctx flow_dev_ctx = {0};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 1,
		.port_config.nb_queues = 1,
	};

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

	result = doca_argp_init(NULL, &flow_dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_flow_device_params(NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = register_flow_stats_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register stats parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&flow_dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	result = routenic_asap2_test(dpdk_config.port_config.nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("routenic_asap2_test() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini_with_devs(dpdk_config.port_config.nb_ports);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("routeNIC Experiment 3 test finished successfully");
	else
		DOCA_LOG_INFO("routeNIC Experiment 3 test finished with errors");
	return exit_status;
}
