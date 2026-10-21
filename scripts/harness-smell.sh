#!/usr/bin/env sh
# The kernel (include/, src/) may not contain the vocabulary of an agent
# harness: it speaks of entities, capabilities, state, graphs and channels.
# Comments explaining the rule are exempt; code is not.
cd "$(dirname "$0")/.."
# One narrow exemption, and only in the layer that speaks to the outside world.
# A media type and a file extension are somebody else's spelling, not ours:
# bitcoind and every execution client reject a request that does not carry the
# type verbatim, and a file arrives named whatever the sender named it. Scoped
# to src/channel/ and to those two tokens; everywhere else the law is unchanged.
hits=$(grep -rniwE "agent|agents|tool|tools|tool_call|prompt|prompts|mcp|json|llm|assay|orchestrat(e|or|ion)|objective|harness" include src \
  | grep -vE '^[^:]+:[0-9]+:\s*//' | grep -vE '/\*|\*/' \
  | grep -vE '^src/userland/' \
  | grep -vE '^include/aios/userland/' \
  | grep -vE '^src/channel/' \
  | grep -vE '^include/aios/channel/' \
  | grep -vE '^include/aios/hal/process\.hpp' \
  | grep -vE '^src/hal/linux_audio\.cpp' \
  | grep -vE '^src/net/gzip\.cpp' \
  | grep -vE '(include/aios|src)/fleet/orchestrator\.' || true)
if [ -n "$hits" ]; then echo "HARNESS SMELL — the kernel speaks like a harness:"; echo "$hits" | sed 's/^/  /'; exit 1; fi
echo "no harness vocabulary in the kernel"
