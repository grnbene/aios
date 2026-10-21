// What this browser would send when a form on a page is submitted.
//
// The request is the contract between the page and the server, so it is worth
// being able to read it without a server on the other end: this parses a page,
// finds each <form> and the control that would submit it, and prints the url,
// the method and the encoded body.
//
//   aios-form-check <page.html> [base-url]
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/web.hpp"

using namespace aios;
using namespace aios::userland::web;

namespace {

void find_forms(Node* n, std::vector<Node*>& out) {
  if (!n) return;
  if (n->kind == Node::Kind::Element && n->tag == "form") out.push_back(n);
  for (auto& kid : n->children) find_forms(kid.get(), out);
}

// The control a person would press: the first submit control in the form.
Node* first_submit(Node* n) {
  if (!n) return nullptr;
  if (is_submit_control(n)) return n;
  for (auto& kid : n->children)
    if (Node* found = first_submit(kid.get())) return found;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: aios-form-check <page.html> [base-url]\n"); return 2; }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string markup = ss.str();
  const std::string base = argc > 2 ? argv[2] : "http://example.test/page";

  NodePtr dom = parse_html(ByteSpan((const uint8_t*)markup.data(), markup.size()));
  if (!dom) { std::fprintf(stderr, "%s: parsed to nothing\n", argv[1]); return 1; }

  std::vector<Node*> forms;
  find_forms(dom.get(), forms);
  std::printf("# %zu form(s) in %s\n", forms.size(), argv[1]);
  for (size_t i = 0; i < forms.size(); ++i) {
    Node* submitter = first_submit(forms[i]);
    const FormRequest req = form_request(*forms[i], submitter, base, base);
    std::printf("form %zu  submitter=%s\n", i,
                submitter ? submitter->tag.c_str() : "(none)");
    std::printf("  %s %s\n", req.options.method.c_str(), req.url.c_str());
    for (const auto& h : req.options.headers)
      std::printf("  header %s: %s\n", h.first.c_str(), h.second.c_str());
    std::printf("  body %s\n", req.options.body.empty() ? "(empty)" : req.options.body.c_str());
  }
  return 0;
}
