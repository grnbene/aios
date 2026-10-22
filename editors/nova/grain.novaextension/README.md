# Grain Language Extension for Nova

Full syntax highlighting, symbol navigation, and code completion for **Grain (`.gr`)**, the unified substrate programming language for AIOS.

## Features

- **Syntax Highlighting**:
  - First-class Substrate keywords: `entity`, `on`, `fn`, `let`
  - Substrate Types & Objects: `Id128`, `Capability`, `StateChannel`, `StateFrame`, `MemoryObject`, `Entity`
  - Substrate Constants: `read_only`, `write_only`, `read_write`, `grant`, `never`, `shared_mem`, `ram`, `persistent`, `current_generation`
  - Substrate Methods: `publish`, `consume`, `stats`, `attenuate`, `can`, `has_expired`, `read`, `write`, `migrate`, `dispatch`
  - Scoped Namespaces: `hardware::uart0`
  - Number Literals: Decimals, Floats, Hex (`0x...`), Binary (`0b...`)
  - Formatted String Interpolation: `f"..."`
  - Comments: `//` and `#`
- **Symbol Navigation**: Jump to entities, event handlers, and functions directly in Nova's Symbols sidebar.
- **Auto-Completions**: Snippets and function argument templates for `entity`, `on`, `fn`, and substrate builtins.

## Installation

Double-click `grain.novaextension` or copy it to:
`~/Library/Application Support/Nova/Extensions/aios.grain.novaextension`
