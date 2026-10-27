#include "document/document.h"

#include <sstream>
#include <stdexcept>

namespace dse::document {

Document Document::Parse(const std::string& json_text) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::parse_error& e) {
    throw std::invalid_argument(std::string("invalid JSON document: ") + e.what());
  }
  if (!parsed.is_object()) {
    throw std::invalid_argument("document JSON must be a top-level object");
  }
  return Document(std::move(parsed));
}

std::optional<std::string> Document::GetString(const std::string& field) const {
  auto it = json_.find(field);
  if (it == json_.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

std::optional<double> Document::GetNumber(const std::string& field) const {
  auto it = json_.find(field);
  if (it == json_.end()) return std::nullopt;
  if (it->is_number()) return it->get<double>();
  if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
  return std::nullopt;
}

std::optional<std::vector<float>> Document::GetVector(const std::string& field) const {
  auto it = json_.find(field);
  if (it == json_.end() || !it->is_array()) return std::nullopt;
  std::vector<float> out;
  out.reserve(it->size());
  for (const auto& el : *it) {
    if (!el.is_number()) return std::nullopt;
    out.push_back(el.get<float>());
  }
  return out;
}

std::optional<std::string> Document::GetAsIndexableString(const std::string& field) const {
  auto it = json_.find(field);
  if (it == json_.end()) return std::nullopt;
  if (it->is_string()) return it->get<std::string>();
  if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
  if (it->is_number_float()) return std::to_string(it->get<double>());
  if (it->is_boolean()) return it->get<bool>() ? std::string("true") : std::string("false");
  if (it->is_array()) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& el : *it) {
      if (!el.is_string()) return std::nullopt;
      if (!first) oss << ' ';
      oss << el.get<std::string>();
      first = false;
    }
    return oss.str();
  }
  return std::nullopt;
}

}  // namespace dse::document
