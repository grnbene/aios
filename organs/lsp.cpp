// organ: lsp
// Given {path, text, line, col} returns {suggestion, diagnostics} for the
// code at that cursor: a lightweight, dependency-free lexer finds unmatched
// brackets / unterminated strings / long lines / mixed indentation, and a
// prefix-based keyword table (picked by file extension) drives completion.
#include <string>
#include <vector>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <sstream>

namespace {

// ---------- minimal hand-rolled JSON reading (no third-party lib) ----------

void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

bool parseJsonStringAt(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    // best-effort: skip 4 hex digits, emit '?' placeholder
                    if (i + 5 < s.size()) { out += '?'; i += 4; }
                    break;
                }
                default: out += n; break;
            }
            i += 2;
        } else {
            out += c;
            ++i;
        }
    }
    if (i >= s.size()) return false; // unterminated
    ++i; // closing quote
    return true;
}

// Finds "key" at the top level of the object and positions i right after the
// following ':'. Returns false if not found.
bool findKeyColon(const std::string& s, const std::string& key, size_t& i) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while (true) {
        pos = s.find(needle, pos);
        if (pos == std::string::npos) return false;
        size_t j = pos + needle.size();
        skipWs(s, j);
        if (j < s.size() && s[j] == ':') {
            i = j + 1;
            skipWs(s, i);
            return true;
        }
        pos = pos + 1;
    }
}

bool getStringField(const std::string& s, const std::string& key, std::string& out) {
    size_t i;
    if (!findKeyColon(s, key, i)) return false;
    return parseJsonStringAt(s, i, out);
}

bool getIntField(const std::string& s, const std::string& key, long long& out) {
    size_t i;
    if (!findKeyColon(s, key, i)) return false;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
    long long v = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) { v = v * 10 + (s[i] - '0'); ++i; }
    out = neg ? -v : v;
    return true;
}

std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if ((unsigned char)c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += c;
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// ---------- language detection ----------

enum class Lang { CppLike, Python, Shell, Generic };

Lang detectLang(const std::string& path) {
    auto endsWith = [&](const char* ext) {
        size_t n = std::strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (endsWith(".cpp") || endsWith(".cc") || endsWith(".cxx") || endsWith(".h") ||
        endsWith(".hpp") || endsWith(".hxx") || endsWith(".c") ||
        endsWith(".js") || endsWith(".ts") || endsWith(".java") || endsWith(".cjs") ||
        endsWith(".mjs") || endsWith(".jsx") || endsWith(".tsx"))
        return Lang::CppLike;
    if (endsWith(".py")) return Lang::Python;
    if (endsWith(".sh") || endsWith(".bash") || endsWith(".yaml") || endsWith(".yml"))
        return Lang::Shell;
    return Lang::Generic;
}

const std::vector<std::string>& keywordTable(Lang lang) {
    static const std::vector<std::string> cpp = {
        "auto", "const", "constexpr", "class", "struct", "namespace", "template",
        "typename", "public", "private", "protected", "virtual", "override",
        "static", "return", "if", "else", "for", "while", "switch", "case",
        "break", "continue", "default", "nullptr", "true", "false", "void",
        "int", "double", "float", "bool", "char", "string", "vector", "map",
        "unique_ptr", "shared_ptr", "std::", "include", "extern", "inline"
    };
    static const std::vector<std::string> py = {
        "def", "class", "import", "from", "return", "if", "elif", "else",
        "for", "while", "break", "continue", "try", "except", "finally",
        "with", "as", "lambda", "yield", "self", "None", "True", "False",
        "print", "len", "range", "list", "dict", "set", "str", "int"
    };
    static const std::vector<std::string> sh = {
        "if", "then", "else", "fi", "for", "do", "done", "while", "case",
        "esac", "function", "echo", "export", "local", "return"
    };
    static const std::vector<std::string> generic = {
        "TODO", "FIXME", "NOTE"
    };
    switch (lang) {
        case Lang::CppLike: return cpp;
        case Lang::Python: return py;
        case Lang::Shell: return sh;
        default: return generic;
    }
}

// ---------- cursor / line helpers ----------

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    lines.push_back(cur);
    return lines;
}

// word (identifier) prefix ending exactly at byte-column col (0-based) of `line`
std::string prefixAt(const std::string& line, long long col) {
    long long end = col;
    if (end < 0) end = 0;
    if (end > (long long)line.size()) end = (long long)line.size();
    long long start = end;
    while (start > 0) {
        char c = line[start - 1];
        if (std::isalnum((unsigned char)c) || c == '_') --start;
        else break;
    }
    return line.substr(start, end - start);
}

// ---------- bracket / string / comment scan across the whole file ----------

struct ScanResult {
    std::vector<std::string> diagnostics;
};

void scanText(const std::string& text, Lang lang, std::vector<std::string>& diags) {
    struct Open { char ch; int line; };
    std::vector<Open> stack;
    int line = 1, col = 0;
    bool inLineComment = false;
    bool inBlockComment = false;
    char inString = 0; // 0, '"' or '\''
    bool hashComments = (lang == Lang::Python || lang == Lang::Shell);

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        char next = (i + 1 < text.size()) ? text[i + 1] : '\0';

        if (c == '\n') {
            if (inLineComment) inLineComment = false;
            ++line; col = 0;
            continue;
        }
        ++col;

        if (inLineComment) continue;

        if (inBlockComment) {
            if (c == '*' && next == '/') { inBlockComment = false; ++i; ++col; }
            continue;
        }

        if (inString) {
            if (c == '\\' && !hashComments) { ++i; ++col; continue; }
            if (c == inString) inString = 0;
            continue;
        }

        if (hashComments) {
            if (c == '#') { inLineComment = true; continue; }
        } else {
            if (c == '/' && next == '/') { inLineComment = true; ++i; ++col; continue; }
            if (c == '/' && next == '*') { inBlockComment = true; ++i; ++col; continue; }
        }

        if (c == '"' || c == '\'') { inString = c; continue; }

        if (c == '(' || c == '{' || c == '[') { stack.push_back({c, line}); continue; }
        if (c == ')' || c == '}' || c == ']') {
            char want = (c == ')') ? '(' : (c == '}') ? '{' : '[';
            if (stack.empty() || stack.back().ch != want) {
                diags.push_back("unmatched '" + std::string(1, c) + "' at line " + std::to_string(line));
            } else {
                stack.pop_back();
            }
            continue;
        }
    }

    if (inString) diags.push_back("unterminated string starting before end of file (line " + std::to_string(line) + ")");
    if (inBlockComment) diags.push_back("unterminated block comment (unclosed /* ... */)");
    for (auto& o : stack) diags.push_back(std::string("unclosed '") + o.ch + "' opened at line " + std::to_string(o.line));
}

void styleChecks(const std::vector<std::string>& lines, std::vector<std::string>& diags) {
    bool sawTab = false, sawSpaceIndent = false;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& l = lines[li];
        if (!l.empty() && (l.back() == ' ' || l.back() == '\t'))
            diags.push_back("trailing whitespace at line " + std::to_string(li + 1));
        if (l.size() > 120)
            diags.push_back("line " + std::to_string(li + 1) + " exceeds 120 columns");
        if (!l.empty()) {
            if (l[0] == '\t') sawTab = true;
            else if (l[0] == ' ') sawSpaceIndent = true;
        }
        if (l.find("TODO") != std::string::npos || l.find("FIXME") != std::string::npos)
            diags.push_back("marker (TODO/FIXME) at line " + std::to_string(li + 1));
    }
    if (sawTab && sawSpaceIndent)
        diags.push_back("mixed tabs and spaces used for indentation");
}

std::string buildSuggestion(const std::vector<std::string>& lines, long long lineNo, long long col, Lang lang) {
    if (lineNo < 1 || (size_t)lineNo > lines.size()) return "cursor line out of range";
    const std::string& line = lines[lineNo - 1];
    std::string prefix = prefixAt(line, col - 1 >= 0 ? col - 1 : 0);

    // an empty prefix -> nothing to complete; look at trailing punctuation for a hint instead
    if (prefix.empty()) {
        std::string beforeCursor = line.substr(0, std::min<size_t>(line.size(), (size_t)std::max<long long>(col - 1, 0)));
        std::string t = trim(beforeCursor);
        if (!t.empty() && (t.back() == '{' )) return "new block: indent and add matching '}'";
        if (!t.empty() && t.back() == '(') return "argument list: matching ')' expected";
        return "";
    }

    const auto& table = keywordTable(lang);
    for (const auto& kw : table) {
        if (kw.size() > prefix.size() && kw.compare(0, prefix.size(), prefix) == 0)
            return kw;
    }
    return "";
}

} // namespace

static std::string out;

extern "C" const char* aios_organ(const char* request) {
    try {
        if (!request) { out = "error: null request"; return out.c_str(); }
        std::string req(request);
        std::string reqTrim = trim(req);

        if (reqTrim == "describe") {
            out = "lsp: given {path, text, line, col} (line/col 1-based) returns "
                  "{\"suggestion\":..., \"diagnostics\":[...]} for the code at that cursor "
                  "-- unmatched/unclosed brackets, unterminated strings or comments, "
                  "trailing whitespace, long lines, mixed indentation, and a keyword-prefix completion.";
            return out.c_str();
        }

        std::string path, text;
        long long line = 0, col = 0;
        bool haveText = getStringField(reqTrim, "text", text);
        if (!haveText) { out = "error: missing or malformed 'text' field"; return out.c_str(); }
        getStringField(reqTrim, "path", path); // optional
        if (!getIntField(reqTrim, "line", line)) line = 1;
        if (!getIntField(reqTrim, "col", col)) col = 1;

        Lang lang = detectLang(path);
        std::vector<std::string> lines = splitLines(text);

        std::vector<std::string> diags;
        scanText(text, lang, diags);
        styleChecks(lines, diags);
        if (line < 1 || (size_t)line > lines.size())
            diags.push_back("cursor line " + std::to_string(line) + " is out of range (file has " + std::to_string(lines.size()) + " lines)");

        std::string suggestion = buildSuggestion(lines, line, col, lang);

        std::ostringstream os;
        os << "{\"suggestion\":\"" << escapeJson(suggestion) << "\",\"diagnostics\":[";
        for (size_t i = 0; i < diags.size(); ++i) {
            if (i) os << ",";
            os << "\"" << escapeJson(diags[i]) << "\"";
        }
        os << "]}";
        out = os.str();
        return out.c_str();
    } catch (...) {
        out = "error: unhandled exception in lsp organ";
        return out.c_str();
    }
}
