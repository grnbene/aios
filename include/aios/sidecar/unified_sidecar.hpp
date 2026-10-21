#pragma once
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/sidecar/aios_sidecar_c_abi.h"
#include "aios/sidecar/binary_telemetry.hpp"
#include <string>
#include <vector>
#include <memory>

namespace aios::sidecar {

BinaryTelemetryCore& get_global_telemetry();

// High-level C++ wrappers around AIOS sidecar subsystems

struct EmlMetadata {
  std::string from;
  std::string to;
  std::string subject;
  std::string date;
  std::string body;
  bool has_html = false;
  size_t attachment_count = 0;
};

struct ICalEventSummary {
  std::string id;
  std::string title;
  std::string location;
  std::string date_str;
  std::string time_str;
  std::string description;
};

struct ContactSummary {
  std::string id;
  std::string full_name;
  std::string email;
  std::string phone;
  std::string company;
};

struct SvgRenderOptions {
  int width = 128;
  int height = 128;
  uint32_t current_color = 0xFF000000;
};

struct CheckpointMetadata {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint64_t body_size = 0;
  uint64_t checksum = 0;
  bool valid = false;
};

Result<EmlMetadata> parse_eml_string(const std::string& eml_raw);
Result<std::vector<ICalEventSummary>> parse_ical_string(const std::string& ics_raw);
Result<std::vector<ContactSummary>> parse_vcard_string(const std::string& vcf_raw);
Result<ByteBuffer> render_svg_to_png(const std::string& svg_xml, const SvgRenderOptions& opts);
Result<ByteBuffer> render_html_to_pdf(const std::string& html, const std::string& css, int viewport_w = 800);
Result<CheckpointMetadata> inspect_checkpoint(ByteSpan bytes);

}  // namespace aios::sidecar
