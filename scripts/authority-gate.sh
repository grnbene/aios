#!/usr/bin/env bash
# The console is an entity. This gate keeps the command surface honest: it may
# READ the kernel's registries, but anything that ACTS must go through the
# console entity's Surface, where the kernel decides from its capabilities.
# Kernel authority (spawn/grant/revoke/terminate/checkpoint/restore/serve/
# connect/bind_mind/cycle/as/resident) and manager mutators are off limits here.
# Only the substrate's binding site (cli/console.cpp, the mains) touches them.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
files="$here/cli/commands.cpp $here/cli/shell.cpp $here/cli/embed.cpp $here/cli/out.cpp $here/cli/ask.cpp"
bad=0
auth='(as|spawn|terminate|grant|revoke|checkpoint|restore|restore_image|capture|serve|connect|bind_mind|swap_mind|unbind_mind|cycle|resident)'
mut='(install|create|destroy|destroy_owned|remove|add_capability|remove_capability|relate|unrelate|set_status|set_execution|allocate|allocate_with_id|write|migrate|release|publish|consume|subscribe|put|delegate|revoke_all|set_payload|attach_memory|add|register_provider|register_transport|observe|record_[a-z_]+|note_[a-z_]+)'
# kernel authority reached directly on a Kernel reference
hits=$(grep -nE "(c\.k|kernel|s\.kernel|\bk)(\.|->)${auth}\(" $files || true)
if [ -n "$hits" ]; then echo "KERNEL AUTHORITY used by the command surface:"; echo "$hits"; bad=1; fi
# manager mutators reached through the kernel's accessors
hits=$(grep -nE "(c\.k|kernel|s\.kernel|\bk)(\.|->)(entities|capabilities|memory|states|channels|graphs|devices|scheduler|store)\(\)\.${mut}\(" $files || true)
if [ -n "$hits" ]; then echo "KERNEL-INTERNAL MUTATION used by the command surface:"; echo "$hits"; bad=1; fi
# constructing a Surface for an arbitrary entity bypasses act_as (the ctor is private; this catches friend abuse)
hits=$(grep -nE "\bSurface\s*\(" $files | grep -v "act_as" || true)
if [ -n "$hits" ]; then echo "SURFACE CONSTRUCTED by the command surface:"; echo "$hits"; bad=1; fi
# host filesystem reached outside the shell's confined root. The rule: a site
# is allowed only when it says why on the same line, `// host-io: <reason>` -
# the machine's own state files (its run dir, its root, its settings) or a file
# the operator named on the command line. Each declared site is counted, and
# any undeclared one fails the gate, so a new one has to be argued for.
all=$(grep -nE "host\(\)\.(write_file|append_file|read_file|remove|make_dir|list_dir)\(" "$here/cli/commands.cpp" "$here/cli/embed.cpp" || true)
hits=$(printf '%s\n' "$all" | grep -v '// host-io: [^ ]' | grep . || true)
declared=$(printf '%s\n' "$all" | grep -c '// host-io: [^ ]' || true)
if [ -n "$hits" ]; then echo "HOST FILESYSTEM used by the command surface outside the shell (no // host-io: <reason>):"; echo "$hits"; bad=1; fi
echo "declared host I/O sites (// host-io: <reason>): $declared"
if [ $bad = 0 ]; then echo "authority gate: the command surface acts only as the console entity (0 violations)"; fi
exit $bad
