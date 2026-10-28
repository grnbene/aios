# Grain Language Support for Visual Studio Code

Official syntax highlighting, language configuration, and snippets for **Grain (`.gr`)**, the unified substrate programming language for AIOS.

## Features

- **Syntax Highlighting**:
  - Substrate Declarations: `entity`, `on`, `fn`, `let`
  - Substrate Types & Objects: `Id128`, `Capability`, `StateChannel`, `StateFrame`, `MemoryObject`, `Entity`
  - Substrate Constants: `read_only`, `write_only`, `read_write`, `grant`, `never`, `shared_mem`, `ram`, `persistent`, `current_generation`
  - Substrate Methods: `publish`, `consume`, `stats`, `attenuate`, `can`, `has_expired`, `read`, `write`, `migrate`, `dispatch`
  - Scoped Namespaces: `hardware::uart0`
  - Number Literals: Decimal, Hexadecimal (`0x...`), Binary (`0b...`), Floating-point
  - Formatted String Interpolation: `f"..."` with nested `{}` expressions
  - Pattern Matching: `match`, `=>`, `??`, `?.`
  - Comments: `//` and `#`
- **Language Configuration**:
  - Comment toggling (`//`)
  - Auto-closing brackets and quotes (`{}`, `[]`, `()`, `""`, `''`)
  - Bracket matching and surrounding pairs
  - Indentation rules
- **Snippets**:
  - `entity`: Full Substrate entity template
  - `on`: Substrate event handler
  - `fn`: Function declaration
  - `let`: Variable declaration
  - `Capability`: Capability block
  - `StateFrame` / `StateChannel`: Substrate communications
  - `test`: Test suite declaration

## Installation

### Method 1: Local Installation
Copy or symlink this directory into your VS Code extensions folder:

- **macOS / Linux**:
  ```bash
  cp -r editors/vscode ~/.vscode/extensions/aios.grain-1.0.0
  ```
- **Windows**:
  ```cmd
  xcopy /E /I editors\vscode %USERPROFILE%\.vscode\extensions\aios.grain-1.0.0
  ```

Reload or restart Visual Studio Code.

### Method 2: Package with vsce
If `vsce` is installed:
```bash
cd editors/vscode
npx @vscode/vsce package
code --install-extension grain-1.0.0.vsix
```

## Language Server (LSP) Integration

Grain includes a built-in Language Server Protocol server:
```bash
grain lsp
```
This provides live diagnostics, document symbols, code completion, and formatting. It can be connected to VS Code using standard generic LSP clients (such as the generic LSP extension).
