/*
 * routeNIC — Experiment 1: DPA decision fast-path kernel.
 *
 * Entry point convention (__dpa_global__, doca_dpa_dev.h) confirmed against
 * NVIDIA's own installed DOCA DPA sample on this host
 * (/opt/mellanox/doca/samples/doca_dpa/dpa_kernel_launch).
 *
 * This is the hardware counterpart to control-plane/accuracy_study/'s
 * dpa_adapted.py: same match semantics (exact substring, case-folded,
 * OR/AND/NOT combination over keyword rules), so the accuracy numbers that
 * study measured apply to what this kernel actually computes.
 *
 * Algorithmic note: dpa_adapted.py models the DPA side with Aho-Corasick
 * (the right choice once the pattern count is large enough that a single
 * shared-state pass matters). This first kernel uses a simpler, equally
 * correct nested-loop multi-pattern scan instead — for a rule set this
 * small it's a fine starting point, it needs no automaton construction or
 * transition table in DPA memory, and it produces IDENTICAL match/no-match
 * decisions to the Aho-Corasick model since both implement plain exact
 * substring containment. Swapping this for a compiled Aho-Corasick
 * transition table is the natural next step once basic RDMA-triggered
 * execution is validated on real hardware — the accuracy study's numbers
 * don't change either way, only the DPA program's own compute cost does.
 */

#include <doca_dpa_dev.h>

#include "fastpath_shared.h"

/* Lowercases a single ASCII byte; matches dpa_adapted.py's "folded" case
 * mode (single global lowercase pass) — see accuracy_study/README.md for
 * why "strict" per-rule case handling costs more DPA program complexity
 * than a first kernel should take on, and what accuracy that trades away. */
static inline uint8_t fastpath_tolower(uint8_t c)
{
	if (c >= 'A' && c <= 'Z')
		return (uint8_t)(c - 'A' + 'a');
	return c;
}

static int fastpath_contains(const uint8_t *haystack, uint32_t haystack_len, const char *needle, uint32_t needle_len)
{
	if (needle_len == 0 || needle_len > haystack_len)
		return 0;

	for (uint32_t i = 0; i + needle_len <= haystack_len; i++) {
		int matched = 1;
		for (uint32_t j = 0; j < needle_len; j++) {
			if (fastpath_tolower(haystack[i + j]) != fastpath_tolower((uint8_t)needle[j])) {
				matched = 0;
				break;
			}
		}
		if (matched)
			return 1;
	}
	return 0;
}

/* Evaluates one keyword rule's OR/AND/NOR combination over its keyword
 * list, mirroring evaluate_rule() in software_baseline.py and
 * DpaMatcher.evaluate_rule() in dpa_adapted.py exactly. */
static int fastpath_eval_rule(const struct fastpath_keyword_rule *rule, const uint8_t *text, uint32_t text_len)
{
	int any_hit = 0;
	int all_hit = 1;

	for (uint32_t k = 0; k < rule->num_keywords; k++) {
		int hit = fastpath_contains(text, text_len, rule->keywords[k], rule->keyword_lens[k]);
		any_hit = any_hit || hit;
		all_hit = all_hit && hit;
	}

	switch (rule->operator) {
	case FASTPATH_OP_OR:
		return any_hit;
	case FASTPATH_OP_AND:
		return rule->num_keywords > 0 ? all_hit : 0;
	case FASTPATH_OP_NOR:
		return !any_hit;
	default:
		return 0;
	}
}

/*
 * Kernel entry point. `request` is a single classifier request already
 * staged in DPA-visible memory (RDMA-written by an upstream DOCA Eth/RDMA
 * receive step — see the experiment README for how this plugs into the
 * receive path); `rules` is the fixed, compile-time-sized rule table shared
 * across invocations; `out_matched` is written back for the host/GPU side
 * to consume.
 *
 * This kernel answers exactly one question: "can this request's routing
 * decision be fully resolved from keyword-only signals, without ever
 * reaching the GPU?" A `1` result means yes and `out_matched` says which
 * way; a `0` result means the request still needs the full GPU classifier
 * for at least one non-keyword signal, matching the accuracy study's
 * coverage finding that only a minority of real decisions are eligible for
 * this at all.
 */
__dpa_global__ void routenic_fastpath_evaluate(uint64_t text_addr,
						uint32_t text_len,
						uint64_t rules_addr,
						uint32_t num_rules,
						uint64_t out_matched_addr)
{
	const uint8_t *text = (const uint8_t *)text_addr;
	const struct fastpath_keyword_rule *rules = (const struct fastpath_keyword_rule *)rules_addr;
	uint8_t *out_matched = (uint8_t *)out_matched_addr;

	for (uint32_t r = 0; r < num_rules; r++)
		out_matched[r] = (uint8_t)fastpath_eval_rule(&rules[r], text, text_len);

	DOCA_DPA_DEV_LOG_INFO("routeNIC fast path: evaluated %u rules against a %u-byte request\n",
			      num_rules,
			      text_len);
}

/*
 * Batched variant: one launch evaluates up to FASTPATH_MAX_BATCH requests at
 * once, one DPA thread per request (doca_dpa_dev_thread_rank() — real,
 * hardware-assigned per-thread rank within this launch, not a host-side
 * loop). Added after benchmarking routenic_fastpath_evaluate() above on
 * real BF3 hardware: even after fixing the dominant per-launch sync-event
 * creation cost (see fastpath_launcher.c's
 * routenic_fastpath_launch_reuse_event()), a single-request-per-launch
 * shape still pays DPA kernel dispatch overhead once per request. Batching
 * amortizes that fixed cost across every thread in the launch instead.
 *
 * text_addrs/text_lens/out_matched_addrs are each arrays of `batch_size`
 * entries in DPA-accessible memory, indexed by thread rank — the host
 * builds these exactly like it builds one request's args for the
 * single-request kernel, just for many requests at once.
 */
__dpa_global__ void routenic_fastpath_evaluate_batch(uint64_t text_addrs_arr,
						       uint64_t text_lens_arr,
						       uint64_t rules_addr,
						       uint32_t num_rules,
						       uint64_t out_matched_addrs_arr,
						       uint32_t batch_size)
{
	unsigned int rank = doca_dpa_dev_thread_rank();

	if (rank >= batch_size)
		return;

	const uint64_t *text_addrs = (const uint64_t *)text_addrs_arr;
	const uint32_t *text_lens = (const uint32_t *)text_lens_arr;
	const uint64_t *out_matched_addrs = (const uint64_t *)out_matched_addrs_arr;
	const struct fastpath_keyword_rule *rules = (const struct fastpath_keyword_rule *)rules_addr;

	const uint8_t *text = (const uint8_t *)text_addrs[rank];
	uint32_t text_len = text_lens[rank];
	uint8_t *out_matched = (uint8_t *)out_matched_addrs[rank];

	for (uint32_t r = 0; r < num_rules; r++)
		out_matched[r] = (uint8_t)fastpath_eval_rule(&rules[r], text, text_len);
}
