#ifndef TAPA_BUFFER_H_
#define TAPA_BUFFER_H_

#include <iostream>
#include <string>

#include "nlohmann/json.hpp"

#include "type.h"

using nlohmann::json;

inline std::string GetSrcVar(const std::string& name) { return name + ".src"; }

inline std::string GetSinkVar(const std::string& name) {
  return name + ".sink";
}

inline std::string GetDataVar(const std::string& name) {
  return name + ".data";
}

template <typename T>
inline bool IsBufferInterface(T obj) {
  return IsTapaType(obj, "(i|o)buffer");
}

template <typename T>
inline bool IsBufferInstance(T obj) {
  return IsTapaType(obj, "buffer");
}

template <typename T>
inline bool IsBuffer(T obj) {
  return IsTapaType(obj, "(i|o)?buffer");
}

int GetIntegerFromTemplateArg(const clang::TemplateArgument& arg);

std::string GetRecordName(const clang::QualType& qualType);

struct BufferConfig {
  enum partition_type_t { NORMAL, COMPLETE, BLOCK, CYCLIC };

  enum memcore_type_t { BRAM, URAM };

  using partition_t = std::pair<partition_type_t, int>;

  const std::string type;
  const clang::QualType qualType;
  const std::vector<int> dims;
  const int n_sections;
  std::vector<partition_t> partition_config;
  memcore_type_t memcore;
  bool isArrayType = false;
  int length = 0;

  BufferConfig() = default;
  json toJson();
};

void ParseDimensions(const clang::ConstantArrayType* constantArrayType,
                     std::vector<int>& dims, clang::QualType& baseType);

BufferConfig ParseBufferType(const clang::QualType& bufferType,
                             bool isArrayType = false);

const clang::ClassTemplateSpecializationDecl* GetTapaBufferDecl(
    const clang::Type* type);
const clang::ClassTemplateSpecializationDecl* GetTapaBufferDecl(
    const clang::QualType& qual_type);
const clang::ClassTemplateSpecializationDecl* GetTapaBuffersDecl(
    const clang::Type* type);
const clang::ClassTemplateSpecializationDecl* GetTapaBuffersDecl(
    const clang::QualType& qual_type);

#endif  // TAPA_BUFFER_H_