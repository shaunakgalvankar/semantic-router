// routeNIC — reports, against the real large corpus, how many real queries
// actually match any of the 4 real keyword rules at all. Not a benchmark:
// a one-shot report used to characterize the real corpus for
// docs/BENCHMARK_COMPARISON.md.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"

	"github.com/vllm-project/semantic-router/src/semantic-router/pkg/classification"
	"github.com/vllm-project/semantic-router/src/semantic-router/pkg/config"
)

func realKeywordRules() []config.KeywordRule {
	return []config.KeywordRule{
		{Name: "code_keywords", Operator: "OR", Keywords: []string{"code", "function", "debug", "algorithm", "refactor"}, CaseSensitive: false, Method: "bm25", BM25Threshold: 0.1},
		{Name: "machine_learning", Operator: "OR", Keywords: []string{"machine learning", "model training", "gradient", "neural network", "classifier"}, CaseSensitive: false, Method: "bm25", BM25Threshold: 0.1},
		{Name: "urgent_keywords", Operator: "OR", Keywords: []string{"urgent", "immediate", "asap", "emergency"}, CaseSensitive: false, Method: "ngram", NgramThreshold: 0.4, NgramArity: 3},
		{Name: "fuzzy_sensitive_keywords", Operator: "OR", Keywords: []string{"social security", "credit card", "password", "api key"}, CaseSensitive: false, Method: "regex", FuzzyMatch: true, FuzzyThreshold: 2},
	}
}

func main() {
	f, err := os.Open("../data/chatbot_arena_queries.txt")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer f.Close()

	var queries []string
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		if l := sc.Text(); l != "" {
			queries = append(queries, l)
		}
	}

	kc, err := classification.NewKeywordClassifier(realKeywordRules())
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer kc.Free()

	type groundTruthRow struct {
		Query    string `json:"query"`
		Category string `json:"category"` // "" = no rule matched
	}

	counts := map[string]int{}
	noMatch := 0
	rows := make([]groundTruthRow, 0, len(queries))
	for _, q := range queries {
		category, _, err := kc.Classify(q)
		if err != nil {
			continue
		}
		if category == "" {
			noMatch++
		} else {
			counts[category]++
		}
		rows = append(rows, groundTruthRow{Query: q, Category: category})
	}

	fmt.Printf("total queries: %d\n", len(queries))
	fmt.Printf("no rule matched: %d (%.2f%%)\n", noMatch, 100*float64(noMatch)/float64(len(queries)))
	for name, c := range counts {
		fmt.Printf("matched %-28s %d (%.2f%%)\n", name, c, 100*float64(c)/float64(len(queries)))
	}

	outPath := "../data/software_ground_truth.json"
	f2, err := os.Create(outPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer f2.Close()
	enc := json.NewEncoder(f2)
	if err := enc.Encode(rows); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("wrote %s (%d rows)\n", outPath, len(rows))
}
