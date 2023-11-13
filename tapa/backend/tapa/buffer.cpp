#include "clang/AST/AST.h"

#include "buffer.h"

using clang::ClassTemplateSpecializationDecl;
using clang::QualType;
using clang::Type;

using llvm::dyn_cast;

int GetIntegerFromTemplateArg(const clang::TemplateArgument& arg) {
  llvm::APSInt integerValue;
  if (arg.getKind() == clang::TemplateArgument::Integral) {
    integerValue = arg.getAsIntegral();
  } else if (arg.getKind() == clang::TemplateArgument::Expression) {
    auto factorExpr = arg.getAsExpr();
    auto factorConstantExpr = llvm::dyn_cast<clang::ConstantExpr>(factorExpr);
    auto child = *factorConstantExpr->child_begin();
    auto childIntegerLiteral = llvm::dyn_cast<clang::IntegerLiteral>(child);
    integerValue = childIntegerLiteral->getValue();
  } else {
    // template argument type should be expression or integral
    assert(1 == 0);
  }
  assert(integerValue.getBitWidth() <= 64);
  return integerValue.getZExtValue();
}

std::string GetRecordName(const clang::QualType& qualType) {
  if (auto recordType = qualType->getAs<clang::RecordType>()) {
    return recordType->getDecl()->getNameAsString();
  } else if (auto builtinType = qualType->getAs<clang::BuiltinType>()) {
    const clang::LangOptions langOptions;
    const clang::PrintingPolicy policy(langOptions);
    return builtinType->getName(policy);
  } else {
    // TODO: Unable to get record name
    assert(1 == 0);
  }
}

void ParseDimensions(const clang::ConstantArrayType* constantArrayType,
                     std::vector<int>& dims, clang::QualType& baseType) {
  dims.push_back(constantArrayType->getSize().getZExtValue());
  auto elementQualType = constantArrayType->getElementType();
  auto elementType =
      llvm::dyn_cast<clang::ConstantArrayType>(elementQualType.getTypePtr());
  if (elementType) {
    ParseDimensions(elementType, dims, baseType);
  } else {
    baseType = elementQualType;
  }
}

json BufferConfig::toJson() {
  auto dims = json::array();
  auto partitionInfo = json::array();

  for (int size : this->dims) {
    dims.push_back(size);
  }

  for (auto partition : this->partition_config) {
    auto partitionDict = json::object();
    if (partition.first == BufferConfig::partition_type_t::NORMAL) {
      partitionDict["type"] = "normal";
    } else if (partition.first == BufferConfig::partition_type_t::BLOCK) {
      partitionDict["type"] = "block";
    } else if (partition.first == BufferConfig::partition_type_t::CYCLIC) {
      partitionDict["type"] = "cyclic";
    } else if (partition.first == BufferConfig::partition_type_t::COMPLETE) {
      partitionDict["type"] = "complete";
    } else {
      // TODO: Raise error if partition type was not recognized
      assert(1 == 0);
    }
    partitionDict["factor"] = partition.second;
    partitionInfo.push_back(partitionDict);
  }

  auto config = json::object();
  config["type"] = this->type;
  config["dims"] = dims;
  config["partitions"] = partitionInfo;
  config["n_sections"] = this->n_sections;
  config["memcore_type"] =
      (this->memcore == memcore_type_t::BRAM) ? "BRAM" : "URAM";
  return config;
}

BufferConfig ParseBufferType(const clang::QualType& bufferType,
                             bool isArrayType) {
  using partition_t = BufferConfig::partition_t;
  using partition_type_t = BufferConfig::partition_type_t;
  using memcore_type_t = BufferConfig::memcore_type_t;

  std::string name;
  std::vector<int> dims;
  clang::QualType baseType;
  std::vector<partition_t> partition_scheme;
  memcore_type_t memcore_type = memcore_type_t::BRAM;
  int arrayLength = 0;

  // TODO: This is qualififed type, should I strip it similar to
  // how GetStreamElemType works?
  // let's get the first template argument
  auto bufferTypeShapeTemplateArgument = GetTemplateArg(bufferType, 0);
  auto bufferTypeShapeQualType = bufferTypeShapeTemplateArgument->getAsType();
  auto bufferTypeShapeType = bufferTypeShapeQualType.getTypePtr();
  auto constantArrayType =
      llvm::dyn_cast<clang::ConstantArrayType>(bufferTypeShapeType);
  if (!constantArrayType) {
    // TODO: raise error, it must be clang::ConstantArrayTYpe
    assert(1 == 0);
  }

  int nextArg = 1;

  if (isArrayType) {
    auto lengthTemplateArgument = GetTemplateArg(bufferType, nextArg++);
    arrayLength = GetIntegerFromTemplateArg(*lengthTemplateArgument);
  }

  ParseDimensions(constantArrayType, dims, baseType);
  auto bufferNumSectionsTemplateArgument =
      GetTemplateArg(bufferType, nextArg++);
  int n_sections =
      GetIntegerFromTemplateArg(*bufferNumSectionsTemplateArgument);

  // add all as normal first, later we shall modify
  for (int i = 0; i < dims.size(); i++) {
    partition_scheme.push_back(partition_t(partition_type_t::NORMAL, 0));
  }

  for (int argIndex = nextArg; argIndex <= nextArg + 2; argIndex++) {
    auto templateArgument = GetTemplateArg(bufferType, argIndex);
    if (!templateArgument) break;
    auto configType = templateArgument->getAsType();
    auto configName = GetRecordName(configType);
    if (configName == "array_partition") {
      auto configTemplateSpecializationType =
          configType->getAs<clang::TemplateSpecializationType>();
      if (!configTemplateSpecializationType) assert(1 == 0);
      const int numArgs = configTemplateSpecializationType->getNumArgs();
      for (int i = 0; i < numArgs; i++) {
        auto partitionSchemeType =
            configTemplateSpecializationType->getArg(i).getAsType();
        std::string partitionType = GetRecordName(partitionSchemeType);
        if (partitionType == "complete") {
          partition_scheme[i] =
              partition_t(BufferConfig::partition_type_t::COMPLETE, 0);
        } else if (partitionType == "normal") {
          partition_scheme[i] =
              partition_t(BufferConfig::partition_type_t::NORMAL, 0);
        } else if (partitionType == "block" || partitionType == "cyclic") {
          auto templateSpecializationType =
              partitionSchemeType->getAs<clang::TemplateSpecializationType>();
          if (!templateSpecializationType) assert(1 == 0);
          const int numArgs = templateSpecializationType->getNumArgs();
          assert(numArgs == 1);
          auto factorArgTemplateArgument =
              templateSpecializationType->getArg(0);
          const int factorNum =
              GetIntegerFromTemplateArg(factorArgTemplateArgument);
          partition_scheme[i] =
              partition_t(partitionType == "block" ? partition_type_t::BLOCK
                                                   : partition_type_t::CYCLIC,
                          factorNum);
        } else {
          // TODO: raise an error
          assert(1 == 0);
        }
      }
    } else if (configName == "memcore") {
      auto configTemplateSpecializationType =
          configType->getAs<clang::TemplateSpecializationType>();
      if (!configTemplateSpecializationType) assert(1 == 0);
      const int numArgs = configTemplateSpecializationType->getNumArgs();
      auto memoryCore = configTemplateSpecializationType->getArg(0).getAsType();
      std::string memoryCoreType = GetRecordName(memoryCore);
      memcore_type = memoryCoreType == "uram" ? memcore_type_t::URAM
                                              : memcore_type_t::BRAM;
    } else {
      break;
    }
  }

  name = GetRecordName(baseType);

  return BufferConfig{name,        baseType,         dims,
                      n_sections,  partition_scheme, memcore_type,
                      isArrayType, arrayLength};
}

const ClassTemplateSpecializationDecl* GetTapaBufferDecl(const Type* type) {
  if (type != nullptr) {
    if (const auto record = type->getAsRecordDecl()) {
      if (const auto decl = dyn_cast<ClassTemplateSpecializationDecl>(record)) {
        if (IsBuffer(decl)) {
          return decl;
        }
      }
    }
  }
  return nullptr;
}

const ClassTemplateSpecializationDecl* GetTapaBufferDecl(
    const QualType& qual_type) {
  return GetTapaBufferDecl(
      qual_type.getUnqualifiedType().getCanonicalType().getTypePtr());
}

const ClassTemplateSpecializationDecl* GetTapaBuffersDecl(const Type* type) {
  if (type != nullptr) {
    if (const auto record = type->getAsRecordDecl()) {
      if (const auto decl = dyn_cast<ClassTemplateSpecializationDecl>(record)) {
        if (IsTapaType(decl, "(i|o)?buffers")) {
          return decl;
        }
      }
    }
  }
  return nullptr;
}

const ClassTemplateSpecializationDecl* GetTapaBuffersDecl(
    const QualType& qual_type) {
  return GetTapaBuffersDecl(
      qual_type.getUnqualifiedType().getCanonicalType().getTypePtr());
}