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
# grain's web host (src/net/grain_serve.cpp) names the language's own json()
# response helper; that word only, in that file. The same for other names that
# belong to somebody else: OpenSSH's ssh-agent (the SSH client and the pipe it
# listens on), ECMAScript's JSON object (the JS engine), and JSON the data
# format where a file is parsed or written as one. Each is that word, in those
# files.
hits=$(grep -rniwE "agent|agents|tool|tools|tool_call|prompt|prompts|mcp|json|llm|assay|orchestrat(e|or|ion)|objective|harness" include src \
  | grep -vE '^[^:]+:[0-9]+:\s*//' | grep -vE '/\*|\*/' \
  | grep -vE '^src/userland/' \
  | grep -vE '^include/aios/userland/' \
  | grep -vE '^src/channel/' \
  | grep -vE '^include/aios/channel/' \
  | grep -vE '^include/aios/hal/process\.hpp' \
  | grep -vE '^src/hal/linux_audio\.cpp' \
  | grep -vE '^src/net/gzip\.cpp' \
  | grep -viE '^src/net/grain_serve\.cpp:[0-9]+:.*json' \
  | grep -viE '^(src|include/aios)/net/ssh[a-z_]*\.(cpp|hpp):[0-9]+:.*agent' \
  | grep -viE '^src/hal/(win|mac|linux)_net\.cpp:[0-9]+:.*agent' \
  | grep -viE '^(src|include/aios)/js/[a-z_]+\.(cpp|hpp):[0-9]+:.*json' \
  | grep -viE '^(src/core/json\.cpp|include/aios/core/json\.hpp|src/crypto/signed_chain\.cpp):[0-9]+:.*json' \
  | grep -vE '(include/aios|src)/fleet/orchestrator\.' || true)
if [ -n "$hits" ]; then echo "HARNESS SMELL — the kernel speaks like a harness:"; echo "$hits" | sed 's/^/  /'; exit 1; fi
echo "no harness vocabulary in the kernel"
