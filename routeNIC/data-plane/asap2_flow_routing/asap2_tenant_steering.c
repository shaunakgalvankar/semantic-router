/*
 * routeNIC — Experiment 3: ASAP2 hardware tenant/recipe steering.
 *
 * API pattern (doca_flow_pipe_cfg_create -> doca_flow_pipe_cfg_set_match ->
 * doca_flow_pipe_create, struct doca_flow_match) confirmed against NVIDIA's
 * own installed DOCA Flow sample on this host
 * (/opt/mellanox/doca/samples/doca_flow/flow_hash_pipe).
 *
 * Honest scoping note: the router's real tenant identifier is an HTTP
 * header (x-authz-tenant-id, injected by the auth backend — see
 * src/semantic-router/pkg/headers/headers.go in the main repo). DOCA Flow's
 * hardware match engine operates on L2-L4 fields (and tunnel/VLAN tags),
 * not application-layer HTTP headers, without a custom header parser
 * definition — a materially bigger undertaking than what this experiment
 * is scoped to answer first. This experiment instead assumes the realistic
 * hardware-steerable proxy every real ASAP2 multi-tenant deployment
 * actually uses: an upstream gateway/ext_authz tags each tenant's traffic
 * with a distinct VLAN ID (or VXLAN VNI) before it reaches this NIC, and
 * hardware steers on THAT. Extending this to genuine HTTP-header matching
 * via a custom parser is a documented follow-up, not assumed away.
 */

#include <doca_error.h>
#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_net.h>

DOCA_LOG_REGISTER(ROUTENIC_ASAP2::TENANT_STEERING);

#define ROUTENIC_MAX_TENANTS 64

struct routenic_tenant_route {
	uint16_t vlan_tci; /* the tenant's assigned VLAN tag, matched in hardware */
	uint16_t recipe_queue_id; /* which downstream RSS queue / recipe pipeline this tenant's traffic steers to */
};

/*
 * Builds one hardware flow pipe that matches on VLAN TCI and steers
 * directly to a per-recipe RSS queue — the hardware equivalent of the
 * router's software entrypoint/recipe resolution
 * (RecipeForRequestModel in src/semantic-router/pkg/config/recipes.go),
 * done once per tenant in the flow table instead of once per request in
 * software. This is also a direct hardware analogue of the tenant-aware
 * entrypoint rules design being discussed for the router itself (issue
 * #2868) — same "which tenant, which recipe" question, answered in
 * silicon instead of Go.
 */
static doca_error_t routenic_create_tenant_steering_pipe(struct doca_flow_port *port,
							   struct doca_flow_fwd *default_fwd,
							   struct doca_flow_pipe **out_pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg = NULL;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, ROUTENIC_MAX_TENANTS);
	if (result != DOCA_SUCCESS)
		goto destroy_cfg;

	/* Control-type pipe (DOCA_FLOW_PIPE_CONTROL, doca_flow.h:324): the
	 * only pipe type whose entries can each carry their own `fwd`
	 * (doca_flow_pipe_control_add_entry, doca_flow.h:1736) — required
	 * here since every tenant steers to a *different* recipe queue, not
	 * one shared destination set at pipe-creation time. */
	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe type to CONTROL: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, default_fwd, NULL, out_pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create tenant steering pipe: %s", doca_error_get_descr(result));

destroy_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Adds one hardware entry per tenant: VLAN TCI -> RSS queue for that
 * tenant's recipe pipeline. This is the operation whose *scaling behavior*
 * this experiment is actually meant to measure — see the experiment doc:
 * hardware flow-table lookups should stay flat as tenant count grows where
 * a software header-match dictionary lookup degrades, and this is a direct
 * hardware analogue of exactly that question for the router's own
 * entrypoint-rule design.
 */
static doca_error_t routenic_add_tenant_entry(struct doca_flow_pipe *pipe,
						const struct routenic_tenant_route *route,
						struct doca_flow_pipe_entry **out_entry)
{
	struct doca_flow_match match = {0};
	struct doca_flow_fwd fwd = {0};
	uint16_t queue_array[1];
	doca_error_t result;

	match.outer.eth_vlan[0].tci = route->vlan_tci;

	/* A single-element queues_array makes this a deterministic
	 * "steer tenant X to queue Y" forward rather than a load-balancing
	 * hash across many queues — deliberately, so "which queue did this
	 * tenant's traffic land on" stays trivially verifiable during
	 * bring-up. API shape (fwd.rss_type / fwd.rss.queues_array /
	 * fwd.rss.outer_flags) confirmed against
	 * flow_ct_udp_query_sample.c on this host. */
	queue_array[0] = route->recipe_queue_id;
	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	fwd.rss.queues_array = queue_array;
	fwd.rss.nr_queues = 1;
	fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_TCP;

	result = doca_flow_pipe_control_add_entry(0,
						   pipe,
						   &match,
						   NULL, /* match_mask: exact match on the value above */
						   NULL, /* condition */
						   NULL, /* actions */
						   NULL, /* actions_mask */
						   NULL, /* action_descs */
						   NULL, /* monitor */
						   0, /* priority: entries are one-per-tenant, no overlap to break ties on */
						   &fwd,
						   NULL, /* usr_ctx */
						   out_entry);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add tenant entry (vlan=%u): %s", route->vlan_tci, doca_error_get_descr(result));

	return result;
}

doca_error_t routenic_asap2_tenant_steering_setup(struct doca_flow_port *port,
						    const struct routenic_tenant_route *routes,
						    uint32_t num_routes,
						    struct doca_flow_pipe **out_pipe)
{
	struct doca_flow_fwd default_fwd = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_pipe *pipe = NULL;
	doca_error_t result;

	if (num_routes > ROUTENIC_MAX_TENANTS)
		return DOCA_ERROR_INVALID_VALUE;

	/* Fail-closed default: traffic with no matching tenant VLAN is
	 * dropped in hardware rather than falling through to an
	 * unauthenticated software path — deliberately mirrors the
	 * "ClaimedNoMatch must never become passthrough" invariant from the
	 * tenant-rules design discussion (issue #2868), applied one layer
	 * lower in the stack. */
	result = routenic_create_tenant_steering_pipe(port, &default_fwd, &pipe);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < num_routes; i++) {
		struct doca_flow_pipe_entry *entry = NULL;
		result = routenic_add_tenant_entry(pipe, &routes[i], &entry);
		if (result != DOCA_SUCCESS)
			return result;
	}

	*out_pipe = pipe;
	return DOCA_SUCCESS;
}
