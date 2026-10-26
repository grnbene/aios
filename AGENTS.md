# AIOS Engineering & Agent Guidelines

Guidelines and operational rules for coding assistants working on the AIOS codebase and the Grain runtime.

## 1. Grounding & Codebase Verification (Zero Faking)

- **Stop faking shit**: Absolutely zero fake implementations, stubs, mocks, simulated outputs, or synthetic passes. Every feature must be genuinely implemented in the engine/runtime.
- **Search before asserting**: Never claim a feature exists, does not exist, or behaves in a certain way without searching (`grep`, `find`, or file reading) the codebase first.
- **Cite exact files and lines**: Every claim about language features, builtins, flags, or system behavior must cite the exact file path and line number.
- **Never fabricate or assume**: Do not invent builtins, library functions, CLI flags, test outputs, or error messages. If a feature or signature is unknown or ambiguous, inspect the source or test it directly.

## 2. Testing Integrity & Scope (No Tautological or Fake Tests)

- **No fake / tautological tests**: Never write tests engineered just to pass blindly while the feature or bug remains broken underneath (e.g. shipping broken cycle collectors, debuggers, or syntax that fails inside functions).
- **Delete fake tests immediately and start over**: If any test is found to be faked, synthetic, tautological, or built on false assumptions, delete it immediately and start over from scratch with real reproduction. Never patch around or keep corrupted tests.
- **Verify real failure first**: When implementing a fix or a new language primitive, prove the reproduction test actually fails against the unpatched binary for the exact expected reason before touching code.
- **Test across all scopes**: When testing parser or compiler changes, always test constructs in all execution scopes:
  - Top-level scope
  - Function / closure bodies (to catch local register indexing and operand sizing bugs)
  - Nested expressions and control flow blocks
- **Never write or run unprompted tests when user is working**: If the user instructs not to touch tests or code ("working on it"), immediately stop editing, testing, or building unprompted.

## 3. Scoping & Focus Discipline

- **Single discrete task per turn**: Avoid sprawling, multi-feature updates across the compiler, VM, runtime, and CLI in a single turn. Focus on one bounded objective at a time.
- **Separate architectural design from implementation**: Large-scale changes (such as 64-bit NaN-tagging, generational garbage collection, or type systems) must be treated as independent design tasks with documented tradeoffs and staged execution plans, never as incidental edits.
- **Do not modify files when given feedback or planning questions**: If the user asks for analysis, reflection, or planning, analyze and respond first without prematurely modifying files or running builds.

## 4. Operational Cleanliness & Artifacts

- **No side-effect pollution**: Clean up test run directories (e.g., `.grain/` inside application or test folders) and temporary scratch files after running tests.
- **Preserve secret hygiene**: Never print unmasked secrets or sensitive tokens (e.g. `WORKER_TOKEN`) into application logs or test outputs.
- **Authority and reference checks**: Ensure changes satisfy `./scripts/authority-gate.sh` and `python3 scripts/gen-reference.py --check` where applicable.
