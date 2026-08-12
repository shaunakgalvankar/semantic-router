// routeNIC — real software baseline for the DPA-vs-software comparison.
//
// This benchmarks the ACTUAL production keyword classifier
// (src/semantic-router/pkg/classification.KeywordClassifier), imported as a
// real dependency via go.mod replace directives — not a reimplementation.
// It uses the exact 4 keyword rules from this repo's own config/config.yaml
// and the same 34-query corpus routeNIC/control-plane/accuracy_study/workload.py
// uses, so the DPA hardware comparison and this benchmark are measuring the
// same inputs.
//
// One correction to config.yaml, not silently fixed: `fuzzy_sensitive_keywords`
// is declared there with `method: fuzzy`, which is not a recognized method in
// NewKeywordClassifier's dispatch (only "bm25", "ngram", "regex" are;
// confirmed by reading keyword_classifier.go directly) — that rule would fail
// to load verbatim. This file constructs it the way the fuzzy-match code path
// (matchesWithCount / hasFuzzyMatch in keyword_classifier_match.go) actually
// works: method "regex" (the default engine) with FuzzyMatch:true and an
// integer edit-distance FuzzyThreshold, matching what
// control-plane/accuracy_study/workload.py already documented as the same
// finding.
package softwarebaselinebench

import (
	"testing"

	"github.com/vllm-project/semantic-router/src/semantic-router/pkg/classification"
	"github.com/vllm-project/semantic-router/src/semantic-router/pkg/config"
)

func realKeywordRules() []config.KeywordRule {
	return []config.KeywordRule{
		{
			Name:          "code_keywords",
			Operator:      "OR",
			Keywords:      []string{"code", "function", "debug", "algorithm", "refactor"},
			CaseSensitive: false,
			Method:        "bm25",
			BM25Threshold: 0.1,
		},
		{
			Name:          "machine_learning",
			Operator:      "OR",
			Keywords:      []string{"machine learning", "model training", "gradient", "neural network", "classifier"},
			CaseSensitive: false,
			Method:        "bm25",
			BM25Threshold: 0.1,
		},
		{
			Name:           "urgent_keywords",
			Operator:       "OR",
			Keywords:       []string{"urgent", "immediate", "asap", "emergency"},
			CaseSensitive:  false,
			Method:         "ngram",
			NgramThreshold: 0.4,
			NgramArity:     3,
		},
		{
			// method corrected from config.yaml's invalid "fuzzy" — see file header.
			Name:           "fuzzy_sensitive_keywords",
			Operator:       "OR",
			Keywords:       []string{"social security", "credit card", "password", "api key"},
			CaseSensitive:  false,
			Method:         "regex",
			FuzzyMatch:     true,
			FuzzyThreshold: 2,
		},
	}
}

// Same 34 queries as control-plane/accuracy_study/workload.py's QUERIES,
// kept in the same order and grouped with the same comments so a diff
// between the two files stays meaningful if either is updated.
var realQueries = []string{
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
	"Hi team, this is urgent — the neural network training job crashed and I need someone to debug the gradient computation asap before the demo.",
	"Not urgent, just wondering if anyone has refactored the classifier code recently, no rush at all.",
	"Emergency: the password reset endpoint is leaking session tokens, please look at this immediately.",
}

// BenchmarkRealKeywordClassify measures the real production Classify() path
// (regex/bm25/ngram dispatch, including the Rust FFI call for bm25/ngram
// rules) over the same query mix the DPA hardware test used. This is the
// software side of the apples-to-apples comparison.
func BenchmarkRealKeywordClassify(b *testing.B) {
	kc, err := classification.NewKeywordClassifier(realKeywordRules())
	if err != nil {
		b.Fatalf("NewKeywordClassifier failed (real router config as-shipped would fail here too, for the same reason): %v", err)
	}
	defer kc.Free()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_, _, _ = kc.Classify(realQueries[i%len(realQueries)])
	}
}

// BenchmarkRealKeywordClassifyPerQuery reports one sub-benchmark per query so
// per-query latency variance (e.g. bm25/ngram FFI calls vs. plain regex) is
// visible, not just the aggregate mixed-workload average above.
func BenchmarkRealKeywordClassifyPerQuery(b *testing.B) {
	kc, err := classification.NewKeywordClassifier(realKeywordRules())
	if err != nil {
		b.Fatalf("NewKeywordClassifier failed: %v", err)
	}
	defer kc.Free()

	for _, q := range realQueries {
		name := q
		if len(name) > 40 {
			name = name[:40]
		}
		b.Run(name, func(b *testing.B) {
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				_, _, _ = kc.Classify(q)
			}
		})
	}
}
