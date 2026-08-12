/*
 * routeNIC — real hardware test for Experiment 3 (ASAP2 tenant steering).
 * Mirrors flow_drop_sample.c's structure exactly: init DOCA Flow, start a
 * real port, call the routeNIC logic being tested, process entries, report
 * genuine pass/fail, clean up.
 */

#include <stdio.h>
#include <string.h>

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(ROUTENIC_ASAP2_TEST);

/* From routeNIC/data-plane/asap2_flow_routing/asap2_tenant_steering.c */
struct routenic_tenant_route {
	uint16_t vlan_tci;
	uint16_t recipe_queue_id;
};
doca_error_t routenic_asap2_tenant_steering_setup(struct doca_flow_port *port,
						    const struct routenic_tenant_route *routes,
						    uint32_t num_routes,
						    void *entries_status_ctx,
						    struct doca_flow_pipe **out_pipe);

doca_error_t routenic_asap2_test(int nb_queues)
{
	const int nb_ports = 1;
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[1];
	uint32_t actions_mem_size[1];
	struct doca_flow_pipe *steering_pipe = NULL;
	struct entries_status status = {0};
	const struct routenic_tenant_route routes[] = {
		{.vlan_tci = 100, .recipe_queue_id = 0},
		{.vlan_tci = 200, .recipe_queue_id = 1},
		{.vlan_tci = 300, .recipe_queue_id = 2},
	};
	const uint32_t num_routes = sizeof(routes) / sizeof(routes[0]);
	doca_error_t result;

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;
	resource.nr_counters = num_routes;

	result = init_doca_flow(nb_queues, "vnf,hws", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	actions_mem_size[0] = ACTIONS_MEM_SIZE(num_routes);
	result = init_doca_flow_vnf_ports(nb_ports, ports, actions_mem_size, &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	printf("routeNIC Experiment 3: real ASAP2 tenant steering pipe test (%u tenant routes)\n", num_routes);

	result = routenic_asap2_tenant_steering_setup(ports[0], routes, num_routes, &status, &steering_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("routenic_asap2_tenant_steering_setup failed: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	printf("  pipe created: %p\n", (void *)steering_pipe);
	printf("  entries processed=%d failure=%s\n", status.nb_processed, status.failure ? "true" : "false");

	int pass = (steering_pipe != NULL) && (status.nb_processed == (int)num_routes + 1) && !status.failure;
	printf("routeNIC Experiment 3: %s (real hardware, %s)\n",
	       pass ? "PASS" : "FAIL",
	       "doca_flow_pipe_control_add_entry + doca_flow_entries_process against a live ConnectX-7 port");

	stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return pass ? DOCA_SUCCESS : DOCA_ERROR_BAD_STATE;
}
