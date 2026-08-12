/*
 * routeNIC — Experiment 1: types shared between the DPA kernel and its host
 * launcher. Kept deliberately tiny and fixed-size (no dynamic allocation on
 * the DPA side).
 */

#ifndef ROUTENIC_FASTPATH_SHARED_H_
#define ROUTENIC_FASTPATH_SHARED_H_

#include <stdint.h>

#define FASTPATH_MAX_KEYWORDS 8
#define FASTPATH_MAX_KEYWORD_LEN 32
#define FASTPATH_MAX_RULES 8

enum fastpath_operator {
	FASTPATH_OP_OR = 0,
	FASTPATH_OP_AND = 1,
	FASTPATH_OP_NOR = 2,
};

/* Mirrors decision_model.KeywordRule's DPA-relevant fields exactly (name is
 * tracked host-side only, for reporting — the kernel never needs it). */
struct fastpath_keyword_rule {
	enum fastpath_operator operator;
	uint32_t num_keywords;
	char keywords[FASTPATH_MAX_KEYWORDS][FASTPATH_MAX_KEYWORD_LEN];
	uint32_t keyword_lens[FASTPATH_MAX_KEYWORDS];
};

#endif /* ROUTENIC_FASTPATH_SHARED_H_ */
