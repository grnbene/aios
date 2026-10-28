# Grain Language Support for Visual Studio

Syntax highlighting and language configuration for **Grain (`.gr`)**, the unified substrate programming language for AIOS, in Visual Studio (2017, 2019, 2022).

## Features

- **TextMate Syntax Highlighting**:
  - Declarations: `entity`, `on`, `fn`, `let`
  - Substrate Types & Objects: `Id128`, `Capability`, `StateChannel`, `StateFrame`, `MemoryObject`, `Entity`
  - Substrate Constants: `read_only`, `write_only`, `read_write`, `grant`, `never`, `shared_mem`, `ram`, `persistent`, `current_generation`
  - Substrate Methods: `publish`, `consume`, `stats`, `attenuate`, `can`, `has_expired`, `read`, `write`, `migrate`, `dispatch`
  - Scoped Namespaces: `hardware::uart0`
  - Number Literals: Decimals, Hexadecimal (`0x...`), Binary (`0b...`), Floats
  - Formatted String Interpolation: `f"..."` with nested expressions
  - Pattern Matching: `match`, `=>`, `??`, `?.`
  - Comments: `//` and `#`
- **Language Configuration**:
  - Comment line prefix (`//`)
  - Auto-closing pairs and bracket matching (`{}`, `[]`, `()`, `""`, `''`)
  - Auto-indentation on open braces

## Installation

### Method 1: User Profile Extensions Folder (Quick Install)

Visual Studio automatically loads TextMate grammars placed in the user's extension folder:

1. Copy the `Syntaxes` folder and `language-configuration.json` to:
   ```cmd
   %USERPROFILE%\.vs\Extensions\Grain\
       ├── Syntaxes\
       │   └── grain.tmLanguage (or grain.tmLanguage.json)
       └── language-configuration.json
   ```
2. Restart Visual Studio. Any `.gr` file will open with syntax highlighting and bracket matching.

### Method 2: VSIX Extension

To build and install as a standard `.vsix` extension:
1. Open Visual Studio with the Visual Studio SDK installed.
2. Build the project using `extension.vsixmanifest` and `Grain.pkgdef`.
3. Double-click the generated `.vsix` file to install it across all installed Visual Studio editions (Community, Professional, Enterprise).
