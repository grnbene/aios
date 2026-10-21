#pragma once
// The byte-level BPE tokenizer the Qwen2 family uses, read from the model's own
// tokenizer.json: the vocabulary, the merge ranks, the added (special) tokens.
// Every input decomposes into bytes, so nothing is unknown.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::userland {

class BpeTokenizer {
 public:
  static Result<BpeTokenizer> from_json(const std::string& tokenizer_json);

  // Added tokens (<|im_start|> and the like) written literally in the text
  // become their single ids; everything else is split, byte-mapped and merged.
  std::vector<int32_t> encode(const std::string& text) const;
  std::string decode(const std::vector<int32_t>& ids) const;
  // The bytes one token stands for (a token may be half a UTF-8 sequence).
  const std::string& token_bytes(int32_t id) const;
  int32_t token_id(const std::string& content) const;   // -1 when absent
  size_t size() const { return bytes_.size(); }

 private:
  void encode_piece(const std::string& piece, std::vector<int32_t>& out) const;
  void encode_plain(const std::string& text, std::vector<int32_t>& out) const;

  std::unordered_map<std::string, int32_t> vocab_;      // byte-mapped token text -> id
  std::unordered_map<std::string, int32_t> ranks_;      // "a b" -> merge rank
  std::unordered_map<std::string, int32_t> added_;      // literal added token -> id
  std::vector<std::string> added_by_len_;               // longest first
  std::vector<std::string> bytes_;                      // id -> raw bytes
  std::string byte_char_[256];                          // byte -> its UTF-8 stand-in
};

}  // namespace aios::userland
