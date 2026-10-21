#include "aios/userland/source_lang.hpp"
#include <algorithm>

namespace aios::userland {

namespace {

char lower_ch(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

std::string lower_str(const std::string& s) {
  std::string out = s;
  for (auto& c : out) c = lower_ch(c);
  return out;
}

bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// --- what each language calls a keyword -----------------------------------
//
// One list per family rather than one list for everything: `fn` is a keyword
// in PHP 7.4 and in Rust and in neither C++ nor Python, and a file painted
// from a merged list tells you nothing about the file you are in.

const char* const KW_C[] = {
  "auto","break","case","char","const","continue","default","do","double","else","enum",
  "extern","float","for","goto","if","inline","int","long","register","restrict","return",
  "short","signed","sizeof","static","struct","switch","typedef","union","unsigned","void",
  "volatile","while","NULL","true","false"};

const char* const KW_CPP[] = {
  "alignas","alignof","asm","auto","bool","break","case","catch","char","char8_t","char16_t",
  "char32_t","class","co_await","co_return","co_yield","concept","const","consteval","constexpr",
  "constinit","const_cast","continue","decltype","default","delete","do","double","dynamic_cast",
  "else","enum","explicit","export","extern","false","float","for","friend","goto","if","inline",
  "int","long","mutable","namespace","new","noexcept","nullptr","operator","private","protected",
  "public","register","reinterpret_cast","requires","return","short","signed","sizeof","static",
  "static_assert","static_cast","struct","switch","template","this","thread_local","throw","true",
  "try","typedef","typeid","typename","union","unsigned","using","virtual","void","volatile","while"};

const char* const KW_CSHARP[] = {
  "abstract","as","async","await","base","bool","break","byte","case","catch","char","checked",
  "class","const","continue","decimal","default","delegate","do","double","else","enum","event",
  "explicit","extern","false","finally","fixed","float","for","foreach","get","goto","if","implicit",
  "in","int","interface","internal","is","lock","long","namespace","new","null","object","operator",
  "out","override","params","private","protected","public","readonly","record","ref","return","sbyte",
  "sealed","set","short","sizeof","stackalloc","static","string","struct","switch","this","throw",
  "true","try","typeof","uint","ulong","unchecked","unsafe","ushort","using","var","virtual","void",
  "volatile","while","yield"};

const char* const KW_JAVA[] = {
  "abstract","assert","boolean","break","byte","case","catch","char","class","const","continue",
  "default","do","double","else","enum","extends","final","finally","float","for","goto","if",
  "implements","import","instanceof","int","interface","long","native","new","null","package",
  "private","protected","public","record","return","short","static","strictfp","super","switch",
  "synchronized","this","throw","throws","transient","true","false","try","var","void","volatile","while"};

const char* const KW_KOTLIN[] = {
  "as","break","class","continue","do","else","false","for","fun","if","in","interface","is","null",
  "object","package","return","super","this","throw","true","try","typealias","typeof","val","var",
  "when","while","by","catch","constructor","delegate","dynamic","field","finally","get","import",
  "init","param","property","receiver","set","setparam","where","data","sealed","suspend","override",
  "private","protected","public","internal","abstract","open","lateinit","companion"};

const char* const KW_SWIFT[] = {
  "associatedtype","class","deinit","enum","extension","fileprivate","func","import","init","inout",
  "internal","let","open","operator","private","protocol","public","rethrows","static","struct",
  "subscript","typealias","var","break","case","continue","default","defer","do","else","fallthrough",
  "for","guard","if","in","repeat","return","switch","where","while","as","Any","catch","false","is",
  "nil","super","self","Self","throw","throws","true","try","async","await","some"};

const char* const KW_GO[] = {
  "break","case","chan","const","continue","default","defer","else","fallthrough","for","func","go",
  "goto","if","import","interface","map","package","range","return","select","struct","switch","type",
  "var","nil","true","false","iota","make","new","len","cap","append","copy","delete","panic","recover"};

const char* const KW_RUST[] = {
  "as","async","await","break","const","continue","crate","dyn","else","enum","extern","false","fn",
  "for","if","impl","in","let","loop","match","mod","move","mut","pub","ref","return","self","Self",
  "static","struct","super","trait","true","type","unsafe","use","where","while","union"};

const char* const KW_PYTHON[] = {
  "and","as","assert","async","await","break","class","continue","def","del","elif","else","except",
  "False","finally","for","from","global","if","import","in","is","lambda","None","nonlocal","not",
  "or","pass","raise","return","True","try","while","with","yield","self","match","case"};

const char* const KW_RUBY[] = {
  "alias","and","begin","break","case","class","def","defined?","do","else","elsif","end","ensure",
  "false","for","if","in","module","next","nil","not","or","redo","rescue","retry","return","self",
  "super","then","true","undef","unless","until","when","while","yield","require","attr_accessor"};

const char* const KW_LUA[] = {
  "and","break","do","else","elseif","end","false","for","function","goto","if","in","local","nil",
  "not","or","repeat","return","then","true","until","while"};

// PHP 7.4: `fn` for arrow functions and typed properties both arrived here, so
// a list that predates them paints modern PHP as though it were a mistake.
const char* const KW_PHP[] = {
  "abstract","and","array","as","break","callable","case","catch","class","clone","const","continue",
  "declare","default","do","echo","else","elseif","empty","enddeclare","endfor","endforeach","endif",
  "endswitch","endwhile","enum","extends","final","finally","fn","for","foreach","function","global",
  "goto","if","implements","include","include_once","instanceof","insteadof","interface","isset",
  "list","match","namespace","new","or","print","private","protected","public","readonly","require",
  "require_once","return","static","switch","throw","trait","try","unset","use","var","while","xor",
  "yield","true","false","null","int","float","string","bool","void","iterable","object","self",
  "parent","mixed","never"};

const char* const KW_JS[] = {
  "async","await","break","case","catch","class","const","continue","debugger","default","delete","do",
  "else","export","extends","false","finally","for","function","if","import","in","instanceof","let",
  "new","null","of","return","static","super","switch","this","throw","true","try","typeof","undefined",
  "var","void","while","with","yield","get","set"};

// TypeScript is JavaScript plus a type language, and Angular adds the words a
// component file is mostly made of.
const char* const KW_TS[] = {
  "abstract","any","as","asserts","async","await","bigint","boolean","break","case","catch","class",
  "const","constructor","continue","declare","default","delete","do","else","enum","export","extends",
  "false","finally","for","from","function","get","if","implements","import","in","infer","instanceof",
  "interface","is","keyof","let","module","namespace","never","new","null","number","object","of","out",
  "override","private","protected","public","readonly","require","return","satisfies","set","static",
  "string","super","switch","symbol","this","throw","true","try","type","typeof","undefined","unique",
  "unknown","var","void","while","yield","abstract","accessor"};

const char* const KW_SQL[] = {
  "select","from","where","insert","into","values","update","set","delete","create","table","drop",
  "alter","add","column","primary","key","foreign","references","index","unique","not","null","default",
  "join","inner","left","right","outer","cross","on","group","by","order","asc","desc","having","limit",
  "offset","distinct","union","all","as","and","or","in","between","like","is","exists","case","when",
  "then","else","end","count","sum","avg","min","max","truncate","begin","commit","rollback","show",
  "describe","int","integer","text","varchar","real","float","double","boolean","date","datetime"};

const char* const KW_SHELL[] = {
  "if","then","else","elif","fi","case","esac","for","while","until","do","done","in","function",
  "select","time","coproc","return","exit","break","continue","local","export","readonly","declare",
  "typeset","unset","shift","source","alias","echo","cd","set","trap"};

const char* const KW_YAML[] = {"true","false","null","yes","no","on","off"};

const char* const KW_CMAKE[] = {
  "if","elseif","else","endif","foreach","endforeach","while","endwhile","function","endfunction",
  "macro","endmacro","set","unset","list","string","file","find_package","include","add_executable",
  "add_library","target_link_libraries","target_include_directories","project","cmake_minimum_required",
  "option","message","install","add_subdirectory","target_compile_options","target_compile_definitions"};

const char* const KW_DOCKER[] = {
  "FROM","RUN","CMD","LABEL","MAINTAINER","EXPOSE","ENV","ADD","COPY","ENTRYPOINT","VOLUME","USER",
  "WORKDIR","ARG","ONBUILD","STOPSIGNAL","HEALTHCHECK","SHELL","AS"};

struct KeywordSet {
  const char* const* words = nullptr;
  size_t count = 0;
};

template <size_t N>
constexpr KeywordSet set_of(const char* const (&a)[N]) { return KeywordSet{a, N}; }

// How a language is written: which comment markers it has, what a string looks
// like, whether $ starts a variable, whether it is markup rather than code.
struct LangSpec {
  const char* name = "Text";
  KeywordSet keywords;
  bool line_slash = false;     // //
  bool line_hash = false;      // #
  bool line_dash = false;      // --
  bool line_semicolon = false; // ;
  bool block_c = false;        // /* */
  bool block_html = false;     // <!-- -->
  bool single_quote = true;    // '...' is a string
  bool backtick = false;       // `...`
  bool dollar_var = false;     // $name
  bool at_decorator = false;   // @Component
  bool preproc_hash = false;   // a leading # is a directive, not a comment
  bool markup = false;         // tags and attributes rather than statements
  bool case_insensitive_kw = false;
};

LangSpec spec_for(SourceLang lang) {
  LangSpec s;
  switch (lang) {
    case SourceLang::C:
      s.name = "C"; s.keywords = set_of(KW_C);
      s.line_slash = s.block_c = s.preproc_hash = true; break;
    case SourceLang::Cpp:
      s.name = "C++"; s.keywords = set_of(KW_CPP);
      s.line_slash = s.block_c = s.preproc_hash = true; break;
    case SourceLang::CSharp:
      s.name = "C#"; s.keywords = set_of(KW_CSHARP);
      s.line_slash = s.block_c = s.at_decorator = true; break;
    case SourceLang::Java:
      s.name = "Java"; s.keywords = set_of(KW_JAVA);
      s.line_slash = s.block_c = s.at_decorator = true; break;
    case SourceLang::Kotlin:
      s.name = "Kotlin"; s.keywords = set_of(KW_KOTLIN);
      s.line_slash = s.block_c = s.at_decorator = true; break;
    case SourceLang::Swift:
      s.name = "Swift"; s.keywords = set_of(KW_SWIFT);
      s.line_slash = s.block_c = s.at_decorator = true; break;
    case SourceLang::Go:
      s.name = "Go"; s.keywords = set_of(KW_GO);
      s.line_slash = s.block_c = s.backtick = true; break;
    case SourceLang::Rust:
      s.name = "Rust"; s.keywords = set_of(KW_RUST);
      s.line_slash = s.block_c = s.at_decorator = true; break;
    case SourceLang::Python:
      s.name = "Python"; s.keywords = set_of(KW_PYTHON);
      s.line_hash = s.at_decorator = true; break;
    case SourceLang::Ruby:
      s.name = "Ruby"; s.keywords = set_of(KW_RUBY);
      s.line_hash = true; break;
    case SourceLang::Lua:
      s.name = "Lua"; s.keywords = set_of(KW_LUA);
      s.line_dash = true; break;
    case SourceLang::Php:
      s.name = "PHP"; s.keywords = set_of(KW_PHP);
      s.line_slash = s.line_hash = s.block_c = s.dollar_var = true; break;
    case SourceLang::JavaScript:
      s.name = "JavaScript"; s.keywords = set_of(KW_JS);
      s.line_slash = s.block_c = s.backtick = s.at_decorator = true; break;
    case SourceLang::TypeScript:
      s.name = "TypeScript"; s.keywords = set_of(KW_TS);
      s.line_slash = s.block_c = s.backtick = s.at_decorator = true; break;
    case SourceLang::Html:
      s.name = "HTML"; s.block_html = s.markup = true; break;
    case SourceLang::Xml:
      s.name = "XML"; s.block_html = s.markup = true; break;
    case SourceLang::Css:
      s.name = "CSS"; s.block_c = true; break;
    case SourceLang::Json:
      s.name = "JSON"; s.single_quote = false; break;
    case SourceLang::Yaml:
      s.name = "YAML"; s.keywords = set_of(KW_YAML); s.line_hash = true; break;
    case SourceLang::Toml:
      s.name = "TOML"; s.line_hash = true; s.line_semicolon = true; break;
    case SourceLang::Markdown:
      s.name = "Markdown"; break;
    case SourceLang::Shell:
      s.name = "Shell"; s.keywords = set_of(KW_SHELL);
      s.line_hash = s.dollar_var = s.backtick = true; break;
    case SourceLang::Sql:
      s.name = "SQL"; s.keywords = set_of(KW_SQL);
      s.line_dash = s.block_c = s.case_insensitive_kw = true; break;
    case SourceLang::Cmake:
      s.name = "CMake"; s.keywords = set_of(KW_CMAKE);
      s.line_hash = s.dollar_var = true; break;
    case SourceLang::Makefile:
      s.name = "Makefile"; s.line_hash = s.dollar_var = true; break;
    case SourceLang::Dockerfile:
      s.name = "Dockerfile"; s.keywords = set_of(KW_DOCKER);
      s.line_hash = s.dollar_var = true; break;
    case SourceLang::Diff:
      s.name = "Diff"; break;
    case SourceLang::PlainText:
    default: s.name = "Text"; break;
  }
  return s;
}

bool in_keyword_set(const KeywordSet& set, const std::string& word, bool fold_case) {
  if (!set.words) return false;
  for (size_t i = 0; i < set.count; ++i) {
    if (fold_case) {
      if (lower_str(set.words[i]) == lower_str(word)) return true;
    } else if (word == set.words[i]) {
      return true;
    }
  }
  return false;
}

void push_span(std::vector<SourceSpan>* out, size_t start, size_t len, TokenClass cls) {
  if (len == 0) return;
  SourceSpan s;
  s.start = (uint32_t)start;
  s.len = (uint32_t)len;
  s.cls = cls;
  out->push_back(s);
}

// --- markup ---------------------------------------------------------------

// HTML, XML and an Angular template. Angular's own attribute forms are called
// out: *ngIf, [value], (click) and {{ interpolation }} are most of what a
// component template is, and painting them as plain text hides the file's
// structure entirely.
void highlight_markup(const std::string& s, HighlightState* st, std::vector<SourceSpan>* out) {
  size_t i = 0;
  while (i < s.size()) {
    if (st->in_block_comment) {
      size_t end = s.find("-->", i);
      size_t len = (end == std::string::npos) ? s.size() - i : (end + 3 - i);
      push_span(out, i, len, TokenClass::Comment);
      if (end == std::string::npos) return;
      st->in_block_comment = false;
      i = end + 3;
      continue;
    }
    if (s.compare(i, 4, "<!--") == 0) {
      st->in_block_comment = true;
      continue;
    }
    // {{ ... }}, Angular's interpolation.
    if (s.compare(i, 2, "{{") == 0) {
      size_t end = s.find("}}", i + 2);
      size_t len = (end == std::string::npos) ? s.size() - i : (end + 2 - i);
      push_span(out, i, len, TokenClass::Variable);
      i += len;
      continue;
    }
    if (s[i] == '<') {
      size_t j = i + 1;
      if (j < s.size() && (s[j] == '/' || s[j] == '!' || s[j] == '?')) j++;
      size_t name_start = j;
      while (j < s.size() && (is_ident_char(s[j]) || s[j] == '-' || s[j] == ':')) j++;
      push_span(out, i, j - i, TokenClass::Tag);
      (void)name_start;
      // Attributes up to the closing angle bracket.
      while (j < s.size() && s[j] != '>') {
        if (s[j] == ' ' || s[j] == '\t') { j++; continue; }
        if (s[j] == '"' || s[j] == '\'') {
          char q = s[j];
          size_t k = j + 1;
          while (k < s.size() && s[k] != q) k++;
          if (k < s.size()) k++;
          push_span(out, j, k - j, TokenClass::String);
          j = k;
          continue;
        }
        size_t a = j;
        while (j < s.size() && s[j] != '=' && s[j] != '>' && s[j] != ' ') j++;
        // *ngIf, [prop], (event), [(ngModel)] -- a binding, not a plain
        // attribute, and worth telling apart at a glance.
        bool binding = a < s.size() && (s[a] == '*' || s[a] == '[' || s[a] == '(' || s[a] == '#');
        push_span(out, a, j - a, binding ? TokenClass::Decorator : TokenClass::Attribute);
        if (j < s.size() && s[j] == '=') j++;
      }
      if (j < s.size() && s[j] == '>') { push_span(out, j, 1, TokenClass::Tag); j++; }
      i = j;
      continue;
    }
    i++;
  }
}

// --- CSS ------------------------------------------------------------------

void highlight_css(const std::string& s, HighlightState* st, std::vector<SourceSpan>* out) {
  size_t i = 0;
  while (i < s.size()) {
    if (st->in_block_comment) {
      size_t end = s.find("*/", i);
      size_t len = (end == std::string::npos) ? s.size() - i : (end + 2 - i);
      push_span(out, i, len, TokenClass::Comment);
      if (end == std::string::npos) return;
      st->in_block_comment = false;
      i = end + 2;
      continue;
    }
    if (s.compare(i, 2, "/*") == 0) { st->in_block_comment = true; continue; }
    if (s[i] == '"' || s[i] == '\'') {
      char q = s[i];
      size_t j = i + 1;
      while (j < s.size() && s[j] != q) j++;
      if (j < s.size()) j++;
      push_span(out, i, j - i, TokenClass::String);
      i = j;
      continue;
    }
    if (s[i] == '#' || s[i] == '.' || s[i] == '&' || s[i] == ':') {
      size_t j = i + 1;
      while (j < s.size() && (is_ident_char(s[j]) || s[j] == '-')) j++;
      if (j > i + 1) { push_span(out, i, j - i, TokenClass::Type); i = j; continue; }
    }
    if (s[i] == '@') {
      size_t j = i + 1;
      while (j < s.size() && is_ident_char(s[j])) j++;
      push_span(out, i, j - i, TokenClass::Keyword);
      i = j;
      continue;
    }
    if (is_digit(s[i])) {
      size_t j = i;
      while (j < s.size() && (is_digit(s[j]) || s[j] == '.' || s[j] == '%' ||
                              (is_ident_char(s[j]) && j < i + 8)))
        j++;
      push_span(out, i, j - i, TokenClass::Number);
      i = j;
      continue;
    }
    if (is_ident_start(s[i])) {
      size_t j = i;
      while (j < s.size() && (is_ident_char(s[j]) || s[j] == '-')) j++;
      // A word followed by a colon is a property; anything else is a value.
      size_t k = j;
      while (k < s.size() && s[k] == ' ') k++;
      push_span(out, i, j - i,
                (k < s.size() && s[k] == ':') ? TokenClass::Attribute : TokenClass::Text);
      i = j;
      continue;
    }
    i++;
  }
}

// --- YAML, TOML, INI ------------------------------------------------------

void highlight_keyed(const std::string& s, const LangSpec& spec, std::vector<SourceSpan>* out) {
  size_t first = s.find_first_not_of(" \t-");
  if (first == std::string::npos) return;
  if (s[first] == '#') { push_span(out, first, s.size() - first, TokenClass::Comment); return; }
  // [section]
  if (s[first] == '[') {
    size_t end = s.find(']', first);
    push_span(out, first, (end == std::string::npos ? s.size() : end + 1) - first, TokenClass::Type);
    return;
  }
  // key: value, or key = value
  size_t sep = s.find_first_of(":=", first);
  if (sep != std::string::npos) push_span(out, first, sep - first, TokenClass::Attribute);
  size_t vstart = (sep == std::string::npos) ? first : sep + 1;
  for (size_t i = vstart; i < s.size();) {
    if (s[i] == '#') { push_span(out, i, s.size() - i, TokenClass::Comment); return; }
    if (s[i] == '"' || s[i] == '\'') {
      char q = s[i];
      size_t j = i + 1;
      while (j < s.size() && s[j] != q) j++;
      if (j < s.size()) j++;
      push_span(out, i, j - i, TokenClass::String);
      i = j;
      continue;
    }
    if (is_digit(s[i])) {
      size_t j = i;
      while (j < s.size() && (is_digit(s[j]) || s[j] == '.')) j++;
      push_span(out, i, j - i, TokenClass::Number);
      i = j;
      continue;
    }
    if (is_ident_start(s[i])) {
      size_t j = i;
      while (j < s.size() && is_ident_char(s[j])) j++;
      std::string word = s.substr(i, j - i);
      if (in_keyword_set(spec.keywords, word, true)) push_span(out, i, j - i, TokenClass::Keyword);
      i = j;
      continue;
    }
    i++;
  }
}

// --- a diff ---------------------------------------------------------------

void highlight_diff(const std::string& s, std::vector<SourceSpan>* out) {
  if (s.empty()) return;
  if (s.rfind("+++", 0) == 0 || s.rfind("---", 0) == 0 || s.rfind("diff ", 0) == 0 ||
      s.rfind("index ", 0) == 0) {
    push_span(out, 0, s.size(), TokenClass::Keyword);
  } else if (s.rfind("@@", 0) == 0) {
    push_span(out, 0, s.size(), TokenClass::Preproc);
  } else if (s[0] == '+') {
    push_span(out, 0, s.size(), TokenClass::Emphasis);
  } else if (s[0] == '-') {
    push_span(out, 0, s.size(), TokenClass::Comment);
  }
}

// --- code -----------------------------------------------------------------

// The general shape most of these languages share: comments, strings, numbers,
// variables, decorators and words that may be keywords.
void highlight_code(const std::string& s, const LangSpec& spec, HighlightState* st,
                    std::vector<SourceSpan>* out) {
  size_t i = 0;

  // A shebang is the first thing about a file, and is not a comment about it.
  if (s.rfind("#!", 0) == 0) {
    push_span(out, 0, s.size(), TokenClass::Preproc);
    return;
  }
  // A C-family preprocessor line is one thing all the way along.
  if (spec.preproc_hash) {
    size_t f = s.find_first_not_of(" \t");
    if (f != std::string::npos && s[f] == '#') {
      push_span(out, f, s.size() - f, TokenClass::Preproc);
      return;
    }
  }

  while (i < s.size()) {
    if (st->in_block_comment) {
      size_t end = s.find("*/", i);
      size_t len = (end == std::string::npos) ? s.size() - i : (end + 2 - i);
      push_span(out, i, len, TokenClass::Comment);
      if (end == std::string::npos) return;
      st->in_block_comment = false;
      i = end + 2;
      continue;
    }
    char c = s[i];

    if (spec.block_c && s.compare(i, 2, "/*") == 0) { st->in_block_comment = true; continue; }
    if (spec.line_slash && s.compare(i, 2, "//") == 0) {
      push_span(out, i, s.size() - i, TokenClass::Comment);
      return;
    }
    if (spec.line_dash && s.compare(i, 2, "--") == 0) {
      push_span(out, i, s.size() - i, TokenClass::Comment);
      return;
    }
    if (spec.line_hash && c == '#') {
      push_span(out, i, s.size() - i, TokenClass::Comment);
      return;
    }
    if (spec.line_semicolon && c == ';') {
      push_span(out, i, s.size() - i, TokenClass::Comment);
      return;
    }
    // Strings. A quote that never closes ends at the end of the line, which is
    // what the person typing it is about to fix.
    if (c == '"' || (c == '\'' && spec.single_quote) || (c == '`' && spec.backtick)) {
      size_t j = i + 1;
      while (j < s.size()) {
        if (s[j] == '\\') { j += 2; continue; }
        if (s[j] == c) { j++; break; }
        j++;
      }
      if (j > s.size()) j = s.size();
      push_span(out, i, j - i, TokenClass::String);
      i = j;
      continue;
    }
    if (spec.dollar_var && c == '$') {
      size_t j = i + 1;
      if (j < s.size() && s[j] == '{') {
        size_t end = s.find('}', j);
        j = (end == std::string::npos) ? s.size() : end + 1;
      } else {
        while (j < s.size() && is_ident_char(s[j])) j++;
      }
      push_span(out, i, j - i, TokenClass::Variable);
      i = j;
      continue;
    }
    if (spec.at_decorator && c == '@' && i + 1 < s.size() && is_ident_start(s[i + 1])) {
      size_t j = i + 1;
      while (j < s.size() && is_ident_char(s[j])) j++;
      push_span(out, i, j - i, TokenClass::Decorator);
      i = j;
      continue;
    }
    if (is_digit(c) || (c == '.' && i + 1 < s.size() && is_digit(s[i + 1]))) {
      size_t j = i;
      while (j < s.size() && (is_digit(s[j]) || s[j] == '.' || s[j] == 'x' || s[j] == 'X' ||
                              s[j] == '_' ||
                              (s[j] >= 'a' && s[j] <= 'f') || (s[j] >= 'A' && s[j] <= 'F')))
        j++;
      push_span(out, i, j - i, TokenClass::Number);
      i = j;
      continue;
    }
    if (is_ident_start(c)) {
      size_t j = i;
      while (j < s.size() && is_ident_char(s[j])) j++;
      std::string word = s.substr(i, j - i);
      if (in_keyword_set(spec.keywords, word, spec.case_insensitive_kw)) {
        push_span(out, i, j - i, TokenClass::Keyword);
      } else {
        // A word straight after '(' opening is a call; a Capitalised word in a
        // typed language reads as a type. Neither is certain, and neither has
        // to be: this is a reading aid, not a symbol table.
        size_t k = j;
        while (k < s.size() && s[k] == ' ') k++;
        if (k < s.size() && s[k] == '(') push_span(out, i, j - i, TokenClass::Function);
        else if (word[0] >= 'A' && word[0] <= 'Z') push_span(out, i, j - i, TokenClass::Type);
      }
      i = j;
      continue;
    }
    i++;
  }
}

// --- markdown as source ---------------------------------------------------

// The emphasis, code and link runs inside one line of Markdown. Used both for
// colouring the source and for drawing the preview.
void markdown_inline(const std::string& s, size_t from, std::vector<SourceSpan>* out) {
  for (size_t i = from; i < s.size();) {
    if (s[i] == '`') {
      size_t end = s.find('`', i + 1);
      size_t len = (end == std::string::npos) ? s.size() - i : (end + 1 - i);
      push_span(out, i, len, TokenClass::Code);
      i += len;
      continue;
    }
    if (s.compare(i, 2, "**") == 0 || s.compare(i, 2, "__") == 0) {
      std::string mark = s.substr(i, 2);
      size_t end = s.find(mark, i + 2);
      size_t len = (end == std::string::npos) ? s.size() - i : (end + 2 - i);
      push_span(out, i, len, TokenClass::Emphasis);
      i += len;
      continue;
    }
    if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] != ' ') {
      size_t end = s.find(s[i], i + 1);
      if (end != std::string::npos) {
        push_span(out, i, end + 1 - i, TokenClass::Emphasis);
        i = end + 1;
        continue;
      }
    }
    if (s[i] == '[') {
      size_t close = s.find(']', i);
      if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
        size_t end = s.find(')', close);
        size_t len = (end == std::string::npos) ? s.size() - i : (end + 1 - i);
        push_span(out, i, len, TokenClass::Link);
        i += len;
        continue;
      }
    }
    i++;
  }
}

// The same inline grammar as markdown_inline, but producing text with the
// markers removed, for drawing rather than for colouring source.
void markdown_runs(const std::string& s, std::vector<MarkdownRun>* out, std::string* plain) {
  out->clear();
  plain->clear();
  std::string buf;
  auto flush = [&]() {
    if (buf.empty()) return;
    out->push_back(MarkdownRun{buf, TokenClass::Text});
    *plain += buf;
    buf.clear();
  };
  auto emit = [&](const std::string& t, TokenClass c) {
    if (t.empty()) return;
    out->push_back(MarkdownRun{t, c});
    *plain += t;
  };

  for (size_t i = 0; i < s.size();) {
    if (s[i] == '`') {
      size_t end = s.find('`', i + 1);
      if (end != std::string::npos) {
        flush();
        emit(s.substr(i + 1, end - i - 1), TokenClass::Code);
        i = end + 1;
        continue;
      }
    }
    if (s.compare(i, 2, "**") == 0 || s.compare(i, 2, "__") == 0) {
      std::string mark = s.substr(i, 2);
      size_t end = s.find(mark, i + 2);
      if (end != std::string::npos) {
        flush();
        emit(s.substr(i + 2, end - i - 2), TokenClass::Emphasis);
        i = end + 2;
        continue;
      }
    }
    if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] != ' ') {
      size_t end = s.find(s[i], i + 1);
      if (end != std::string::npos && end > i + 1) {
        flush();
        emit(s.substr(i + 1, end - i - 1), TokenClass::Emphasis);
        i = end + 1;
        continue;
      }
    }
    if (s[i] == '[') {
      size_t close = s.find(']', i);
      if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
        size_t end = s.find(')', close);
        if (end != std::string::npos) {
          flush();
          // The label is what a reader wants; the target is not shown, the way
          // a link in any other reader is not shown as its own address.
          emit(s.substr(i + 1, close - i - 1), TokenClass::Link);
          i = end + 1;
          continue;
        }
      }
    }
    buf += s[i++];
  }
  flush();
}

void highlight_markdown(const std::string& s, HighlightState* st, std::vector<SourceSpan>* out) {
  std::string trimmed = s;
  size_t first = s.find_first_not_of(" \t");
  if (first == std::string::npos) return;

  // A fence opens and closes a run of lines that are code, whatever they look
  // like -- a '#' inside one is a comment in that code, not a heading.
  if (s.compare(first, 3, "```") == 0 || s.compare(first, 3, "~~~") == 0) {
    st->in_fence = !st->in_fence;
    push_span(out, first, s.size() - first, TokenClass::Rule);
    return;
  }
  if (st->in_fence) {
    push_span(out, 0, s.size(), TokenClass::Code);
    return;
  }
  if (s[first] == '#') {
    push_span(out, first, s.size() - first, TokenClass::Heading);
    return;
  }
  if (s[first] == '>') {
    push_span(out, first, 1, TokenClass::Rule);
    markdown_inline(s, first + 1, out);
    return;
  }
  if (s.compare(first, 3, "---") == 0 || s.compare(first, 3, "***") == 0 ||
      s.compare(first, 3, "___") == 0) {
    push_span(out, first, s.size() - first, TokenClass::Rule);
    return;
  }
  if ((s[first] == '-' || s[first] == '*' || s[first] == '+') && first + 1 < s.size() &&
      s[first + 1] == ' ') {
    push_span(out, first, 1, TokenClass::Rule);
    markdown_inline(s, first + 2, out);
    return;
  }
  if (is_digit(s[first])) {
    size_t j = first;
    while (j < s.size() && is_digit(s[j])) j++;
    if (j < s.size() && (s[j] == '.' || s[j] == ')')) {
      push_span(out, first, j + 1 - first, TokenClass::Rule);
      markdown_inline(s, j + 1, out);
      return;
    }
  }
  markdown_inline(s, 0, out);
}

}  // namespace

// --- what language is this? -----------------------------------------------

SourceLang lang_for_path(const std::string& path) {
  std::string base = path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  std::string low = lower_str(base);

  // Some files are named, not extended.
  if (low == "makefile" || low == "gnumakefile") return SourceLang::Makefile;
  if (low == "dockerfile" || low.rfind("dockerfile.", 0) == 0) return SourceLang::Dockerfile;
  if (low == "cmakelists.txt") return SourceLang::Cmake;
  if (low == ".gitignore" || low == ".dockerignore" || low == ".env") return SourceLang::Shell;

  size_t dot = low.find_last_of('.');
  if (dot == std::string::npos) return SourceLang::PlainText;
  std::string ext = low.substr(dot + 1);

  struct Row { const char* ext; SourceLang lang; };
  static const Row table[] = {
    {"c", SourceLang::C}, {"h", SourceLang::C},
    {"cpp", SourceLang::Cpp}, {"cc", SourceLang::Cpp}, {"cxx", SourceLang::Cpp},
    {"hpp", SourceLang::Cpp}, {"hh", SourceLang::Cpp}, {"hxx", SourceLang::Cpp},
    {"ino", SourceLang::Cpp}, {"m", SourceLang::Cpp}, {"mm", SourceLang::Cpp},
    {"cs", SourceLang::CSharp},
    {"java", SourceLang::Java},
    {"kt", SourceLang::Kotlin}, {"kts", SourceLang::Kotlin},
    {"swift", SourceLang::Swift},
    {"go", SourceLang::Go},
    {"rs", SourceLang::Rust},
    {"py", SourceLang::Python}, {"pyw", SourceLang::Python},
    {"rb", SourceLang::Ruby},
    {"lua", SourceLang::Lua},
    {"php", SourceLang::Php}, {"php7", SourceLang::Php}, {"php8", SourceLang::Php},
    {"phtml", SourceLang::Php}, {"inc", SourceLang::Php},
    {"js", SourceLang::JavaScript}, {"mjs", SourceLang::JavaScript},
    {"cjs", SourceLang::JavaScript}, {"jsx", SourceLang::JavaScript},
    {"ts", SourceLang::TypeScript}, {"tsx", SourceLang::TypeScript},
    {"mts", SourceLang::TypeScript}, {"cts", SourceLang::TypeScript},
    {"html", SourceLang::Html}, {"htm", SourceLang::Html}, {"vue", SourceLang::Html},
    {"svelte", SourceLang::Html},
    {"xml", SourceLang::Xml}, {"svg", SourceLang::Xml}, {"xsd", SourceLang::Xml},
    {"plist", SourceLang::Xml},
    {"css", SourceLang::Css}, {"scss", SourceLang::Css}, {"sass", SourceLang::Css},
    {"less", SourceLang::Css},
    {"json", SourceLang::Json}, {"jsonc", SourceLang::Json},
    {"yaml", SourceLang::Yaml}, {"yml", SourceLang::Yaml},
    {"toml", SourceLang::Toml}, {"ini", SourceLang::Toml}, {"cfg", SourceLang::Toml},
    {"conf", SourceLang::Toml},
    {"md", SourceLang::Markdown}, {"markdown", SourceLang::Markdown},
    {"mdx", SourceLang::Markdown},
    {"sh", SourceLang::Shell}, {"bash", SourceLang::Shell}, {"zsh", SourceLang::Shell},
    {"fish", SourceLang::Shell},
    {"sql", SourceLang::Sql},
    {"cmake", SourceLang::Cmake},
    {"mk", SourceLang::Makefile},
    {"diff", SourceLang::Diff}, {"patch", SourceLang::Diff},
  };
  for (const Row& r : table)
    if (ext == r.ext) return r.lang;
  return SourceLang::PlainText;
}

const char* lang_name(SourceLang lang) { return spec_for(lang).name; }

bool lang_has_preview(SourceLang lang) { return lang == SourceLang::Markdown; }

void highlight_line(SourceLang lang, const std::string& line, HighlightState* state,
                    std::vector<SourceSpan>* out) {
  out->clear();
  if (line.empty()) return;
  HighlightState local;
  HighlightState* st = state ? state : &local;

  if (lang == SourceLang::Markdown) { highlight_markdown(line, st, out); return; }
  if (lang == SourceLang::Diff) { highlight_diff(line, out); return; }

  LangSpec spec = spec_for(lang);
  if (spec.markup) { highlight_markup(line, st, out); return; }
  if (lang == SourceLang::Css) { highlight_css(line, st, out); return; }
  if (lang == SourceLang::Yaml || lang == SourceLang::Toml) {
    highlight_keyed(line, spec, out);
    return;
  }

  // PHP is HTML until <?php says otherwise, and HTML again after ?>. A file
  // painted as though it were PHP throughout gets the template around it wrong.
  if (lang == SourceLang::Php) {
    size_t i = 0;
    while (i < line.size()) {
      if (!st->in_php_block) {
        size_t open = line.find("<?", i);
        if (open == std::string::npos) {
          std::string rest = line.substr(i);
          std::vector<SourceSpan> sub;
          HighlightState markup_state;
          markup_state.in_block_comment = st->in_block_comment;
          highlight_markup(rest, &markup_state, &sub);
          st->in_block_comment = markup_state.in_block_comment;
          for (SourceSpan sp : sub) { sp.start += (uint32_t)i; out->push_back(sp); }
          return;
        }
        if (open > i) {
          std::string chunk = line.substr(i, open - i);
          std::vector<SourceSpan> sub;
          HighlightState markup_state;
          highlight_markup(chunk, &markup_state, &sub);
          for (SourceSpan sp : sub) { sp.start += (uint32_t)i; out->push_back(sp); }
        }
        size_t tag_len = (line.compare(open, 5, "<?php") == 0) ? 5 : 2;
        push_span(out, open, tag_len, TokenClass::Preproc);
        st->in_php_block = true;
        i = open + tag_len;
        continue;
      }
      size_t close = line.find("?>", i);
      std::string chunk = line.substr(i, (close == std::string::npos) ? std::string::npos : close - i);
      std::vector<SourceSpan> sub;
      highlight_code(chunk, spec, st, &sub);
      for (SourceSpan sp : sub) { sp.start += (uint32_t)i; out->push_back(sp); }
      if (close == std::string::npos) return;
      push_span(out, close, 2, TokenClass::Preproc);
      st->in_php_block = false;
      i = close + 2;
    }
    return;
  }

  highlight_code(line, spec, st, out);
}

// --- markdown, rendered ---------------------------------------------------

void markdown_render(const std::vector<std::string>& lines, std::vector<MarkdownLine>* out) {
  out->clear();
  bool in_fence = false;
  for (const std::string& raw : lines) {
    MarkdownLine ml;
    size_t first = raw.find_first_not_of(" \t");

    if (first != std::string::npos &&
        (raw.compare(first, 3, "```") == 0 || raw.compare(first, 3, "~~~") == 0)) {
      in_fence = !in_fence;
      // The fence itself is not shown: the block it opens is drawn as code.
      continue;
    }
    if (in_fence) {
      ml.block = MarkdownBlock::Code;
      ml.text = raw;
      out->push_back(ml);
      continue;
    }
    if (first == std::string::npos) {
      ml.block = MarkdownBlock::Blank;
      out->push_back(ml);
      continue;
    }
    ml.indent = (int)(first / 2);

    if (raw[first] == '#') {
      size_t level = 0;
      while (first + level < raw.size() && raw[first + level] == '#') level++;
      ml.block = level <= 1 ? MarkdownBlock::Heading1
                            : (level == 2 ? MarkdownBlock::Heading2 : MarkdownBlock::Heading3);
      size_t text_at = raw.find_first_not_of(" ", first + level);
      ml.text = (text_at == std::string::npos) ? "" : raw.substr(text_at);
      { std::string plain; markdown_runs(ml.text, &ml.runs, &plain); ml.text = plain; }
      out->push_back(ml);
      continue;
    }
    if (raw.compare(first, 3, "---") == 0 || raw.compare(first, 3, "***") == 0 ||
        raw.compare(first, 3, "___") == 0) {
      ml.block = MarkdownBlock::Rule;
      out->push_back(ml);
      continue;
    }
    if (raw[first] == '>') {
      ml.block = MarkdownBlock::Quote;
      size_t text_at = raw.find_first_not_of("> ", first);
      ml.text = (text_at == std::string::npos) ? "" : raw.substr(text_at);
      { std::string plain; markdown_runs(ml.text, &ml.runs, &plain); ml.text = plain; }
      out->push_back(ml);
      continue;
    }
    if ((raw[first] == '-' || raw[first] == '*' || raw[first] == '+') && first + 1 < raw.size() &&
        raw[first + 1] == ' ') {
      ml.block = MarkdownBlock::Bullet;
      ml.marker = "-";
      ml.text = raw.substr(first + 2);
      { std::string plain; markdown_runs(ml.text, &ml.runs, &plain); ml.text = plain; }
      out->push_back(ml);
      continue;
    }
    if (is_digit(raw[first])) {
      size_t j = first;
      while (j < raw.size() && is_digit(raw[j])) j++;
      if (j < raw.size() && (raw[j] == '.' || raw[j] == ')')) {
        ml.block = MarkdownBlock::Numbered;
        // The list keeps its own numbering rather than being renumbered from
        // one: a document that starts at 3 meant to.
        ml.marker = raw.substr(first, j - first) + ".";
        ml.text = raw.substr(raw.find_first_not_of(" ", j + 1) == std::string::npos
                                 ? raw.size()
                                 : raw.find_first_not_of(" ", j + 1));
        { std::string plain; markdown_runs(ml.text, &ml.runs, &plain); ml.text = plain; }
        out->push_back(ml);
        continue;
      }
    }
    // A table row is drawn as its cells, with the pipes kept as separators.
    if (raw[first] == '|') {
      ml.block = MarkdownBlock::TableRow;
      ml.text = raw.substr(first);
      out->push_back(ml);
      continue;
    }
    ml.block = MarkdownBlock::Paragraph;
    ml.text = raw.substr(first);
    { std::string plain; markdown_runs(ml.text, &ml.runs, &plain); ml.text = plain; }
    out->push_back(ml);
  }
}

}  // namespace aios::userland
