// document.h — thin wrapper around a parsed JSON document plus the shard's
// field-mapping schema. This is the ONLY place the engine touches the
// vendored nlohmann::json single-header library directly for document
// parsing concerns (other modules that need ad-hoc JSON access, e.g. the
// shard's aggregation/range-filter value lookups, may still include
// third_party/json.hpp directly — that's fine, it's a header-only library).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "json.hpp"  // vendored nlohmann/json single header; on the include
                      // path via CMake's third_party/ include dir (see
                      // CMakeLists.txt's dse_core target_include_directories)

namespace dse::document {

// A single field's mapping, as configured at CreateShard time. Every field a
// document is indexed under must appear here (schemaless documents are
// allowed to have EXTRA fields beyond the mapping — those are stored as part
// of `_source` but not indexed/searchable, matching a permissive dynamic-
// mapping-free subset of Elasticsearch's behavior).
struct FieldMapping {
  std::string field;
  std::string type;   // "text" | "keyword" | "number" | "vector"
  int vector_dim = 0;  // only meaningful when type == "vector"
};

// Document wraps one parsed JSON object and provides typed field accessors
// used by the indexing and query paths.
class Document {
 public:
  // Parses `json_text` as a JSON object. Throws std::invalid_argument if the
  // text is not valid JSON or is not a JSON object at the top level.
  static Document Parse(const std::string& json_text);

  const nlohmann::json& Raw() const { return json_; }

  // Returns the field's value as a string if the field is present AND its
  // JSON value is a string; std::nullopt otherwise (missing field, or
  // present but not a string — e.g. a number stored under a "keyword" field
  // by mistake is not silently stringified here, callers that want lenient
  // coercion should use GetAsIndexableString instead).
  std::optional<std::string> GetString(const std::string& field) const;

  // Returns the field's value as a double if present and numeric (JSON
  // number or a JSON boolean coerced to 0/1); std::nullopt otherwise.
  std::optional<double> GetNumber(const std::string& field) const;

  // Returns the field's value as a float vector if present and is a JSON
  // array of numbers; std::nullopt otherwise.
  std::optional<std::vector<float>> GetVector(const std::string& field) const;

  // Lenient string coercion used when building the text that gets analyzed
  // for indexing "text"/"keyword" fields: strings pass through as-is,
  // numbers/booleans are stringified, arrays of strings are space-joined,
  // everything else (objects, null, missing) yields std::nullopt.
  std::optional<std::string> GetAsIndexableString(const std::string& field) const;

 private:
  explicit Document(nlohmann::json json) : json_(std::move(json)) {}
  nlohmann::json json_;
};

}  // namespace dse::document
