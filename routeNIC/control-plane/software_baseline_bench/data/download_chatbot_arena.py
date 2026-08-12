#!/usr/bin/env python3
"""Downloads and extracts a real, large-scale query corpus from the LMSYS
Chatbot Arena Conversations dataset, for the software-vs-DPA benchmark.

This dataset is gated on HuggingFace and its user-prompt content is
CC-BY-4.0 (model outputs are CC-BY-NC-4.0, not used here — only the first
user turn of each conversation is extracted). Because it's gated
specifically to let LMSYS control access, this repo does NOT commit a copy
of the extracted data — regenerate it yourself with your own accepted
access, via this script. See routeNIC/control-plane/software_baseline_bench/README.md
for the full methodology.

Citation: Zheng et al., "Judging LLM-as-a-judge with MT-Bench and Chatbot
Arena Conversations", NeurIPS 2023. Dataset:
https://huggingface.co/datasets/lmsys/chatbot_arena_conversations

Usage:
    1. Accept the dataset's terms at the URL above (logged into your HF
       account).
    2. Generate a read token: https://huggingface.co/settings/tokens
    3. HF_TOKEN=hf_... python3 download_chatbot_arena.py

Requires: huggingface_hub, pyarrow (pip install huggingface_hub pyarrow)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from huggingface_hub import hf_hub_download
import pyarrow.parquet as pq

REPO_ID = "lmsys/chatbot_arena_conversations"
PARQUET_FILE = "data/train-00000-of-00001-cced8514c7ed782a.parquet"
DATA_DIR = Path(__file__).parent
MAX_PROMPT_LEN = 2000
MIN_PROMPT_LEN = 3


def main() -> int:
    token = os.environ.get("HF_TOKEN")
    if not token:
        print("Set HF_TOKEN to a HuggingFace read token with accepted access to", REPO_ID, file=sys.stderr)
        return 1

    print(f"Downloading {PARQUET_FILE} from {REPO_ID}...")
    parquet_path = hf_hub_download(
        repo_id=REPO_ID,
        repo_type="dataset",
        filename=PARQUET_FILE,
        token=token,
        local_dir=str(DATA_DIR),
    )

    table = pq.read_table(parquet_path)
    rows = table.to_pylist()
    print(f"{len(rows)} raw conversation rows")

    seen: set[str] = set()
    prompts: list[str] = []
    for r in rows:
        conv = r["conversation_a"]
        if not conv or conv[0]["role"] != "user":
            continue
        text = conv[0]["content"]
        if not text or not text.strip():
            continue
        if r["language"] != "English":
            continue
        if r["openai_moderation"]["flagged"]:
            continue
        text = text.strip().replace("\r\n", " ").replace("\n", " ").replace("\r", " ")
        if not (MIN_PROMPT_LEN <= len(text) <= MAX_PROMPT_LEN):
            continue
        if text in seen:
            continue
        seen.add(text)
        prompts.append(text)

    print(f"{len(prompts)} unique, clean, English, non-flagged first-turn prompts extracted")

    out_path = DATA_DIR / "chatbot_arena_queries.txt"
    with open(out_path, "w", encoding="utf-8") as f:
        for p in prompts:
            f.write(p + "\n")
    print(f"Wrote {out_path}")

    # Leave the raw parquet in place too (also gitignored) — useful if you
    # want to change the filtering above without re-downloading.
    return 0


if __name__ == "__main__":
    sys.exit(main())
