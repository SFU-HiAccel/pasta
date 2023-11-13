#include "base_target.h"

#include "../tapa/type.h"

namespace tapa {
namespace internal {

void BaseTarget::AddCodeForTopLevelFunc(ADD_FOR_FUNC_ARGS_DEF) {}

void BaseTarget::AddCodeForStream(ADD_FOR_PARAMS_ARGS_DEF) {}
void BaseTarget::AddCodeForTopLevelStream(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForStream(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForMiddleLevelStream(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForStream(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForLowerLevelStream(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForStream(ADD_FOR_PARAMS_ARGS);
}

void BaseTarget::AddCodeForBuffer(ADD_FOR_PARAMS_ARGS_DEF) {}
void BaseTarget::AddCodeForTopLevelBuffer(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForBuffer(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForMiddleLevelBuffer(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForBuffer(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForLowerLevelBuffer(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForBuffer(ADD_FOR_PARAMS_ARGS);
}

void BaseTarget::AddCodeForAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF) {}
void BaseTarget::AddCodeForTopLevelAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForAsyncMmap(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForMiddleLevelAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForAsyncMmap(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForLowerLevelAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForAsyncMmap(ADD_FOR_PARAMS_ARGS);
}

void BaseTarget::AddCodeForMmap(ADD_FOR_PARAMS_ARGS_DEF) {}
void BaseTarget::AddCodeForTopLevelMmap(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForMmap(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForMiddleLevelMmap(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForMmap(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForLowerLevelMmap(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForMmap(ADD_FOR_PARAMS_ARGS);
}

void BaseTarget::AddCodeForScalar(ADD_FOR_PARAMS_ARGS_DEF) {}
void BaseTarget::AddCodeForTopLevelScalar(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForScalar(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForMiddleLevelScalar(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForScalar(ADD_FOR_PARAMS_ARGS);
}
void BaseTarget::AddCodeForLowerLevelScalar(ADD_FOR_PARAMS_ARGS_DEF) {
  AddCodeForScalar(ADD_FOR_PARAMS_ARGS);
}

#define LINES_FUNCTIONS                                                      \
  auto add_line = [&lines](llvm::StringRef line) { lines.push_back(line); }; \
  auto add_pragma = [&lines](std::initializer_list<llvm::StringRef> args) {  \
    lines.push_back("#pragma " + llvm::join(args, " "));                     \
  };

std::vector<std::string> BaseTarget::GenerateCodeForTopLevelFunc(
    const clang::FunctionDecl *func) {
  std::vector<std::string> lines = {""};
  LINES_FUNCTIONS;

  for (const auto param : func->parameters()) {
    if (IsTapaType(param, "(i|o)streams?")) {
      AddCodeForTopLevelStream(param, add_line, add_pragma);
    } else if (IsTapaType(param, "async_mmaps?")) {
      AddCodeForTopLevelAsyncMmap(param, add_line, add_pragma);
    } else if (IsTapaType(param, "mmaps?")) {
      AddCodeForTopLevelMmap(param, add_line, add_pragma);
    } else {
      AddCodeForTopLevelScalar(param, add_line, add_pragma);
    }
    add_line("");  // Separate each parameter.
  }

  add_line("");
  AddCodeForTopLevelFunc(func, add_line, add_pragma);

  return lines;
}

std::vector<std::string> BaseTarget::GenerateCodeForMiddleLevelFunc(
    const clang::FunctionDecl *func) {
  std::vector<std::string> lines = {""};
  LINES_FUNCTIONS;

  for (const auto param : func->parameters()) {
    if (IsTapaType(param, "(i|o)streams?")) {
      AddCodeForMiddleLevelStream(param, add_line, add_pragma);
    } else if (IsTapaType(param, "(i|o)buffers?")) {
      AddCodeForMiddleLevelBuffer(param, add_line, add_pragma);
    } else if (IsTapaType(param, "async_mmaps?")) {
      AddCodeForMiddleLevelAsyncMmap(param, add_line, add_pragma);
    } else if (IsTapaType(param, "mmaps?")) {
      AddCodeForMiddleLevelMmap(param, add_line, add_pragma);
    } else {
      AddCodeForMiddleLevelScalar(param, add_line, add_pragma);
    }
    add_line("");  // Separate each parameter.
  }

  return lines;
}

std::vector<std::string> BaseTarget::GenerateCodeForLowerLevelFunc(
    const clang::FunctionDecl *func) {
  std::vector<std::string> lines = {""};
  LINES_FUNCTIONS;

  for (const auto param : func->parameters()) {
    if (IsTapaType(param, "(i|o)streams?")) {
      AddCodeForLowerLevelStream(param, add_line, add_pragma);
    } else if (IsTapaType(param, "(i|o)buffers?")) {
      AddCodeForLowerLevelBuffer(param, add_line, add_pragma);
    } else if (IsTapaType(param, "async_mmaps?")) {
      AddCodeForLowerLevelAsyncMmap(param, add_line, add_pragma);
    } else if (IsTapaType(param, "mmaps?")) {
      AddCodeForLowerLevelMmap(param, add_line, add_pragma);
    } else {
      AddCodeForLowerLevelScalar(param, add_line, add_pragma);
    }
    add_line("");  // Separate each parameter.
  }

  return lines;
}

void BaseTarget::RewriteTopLevelFunc(REWRITE_FUNC_ARGS_DEF) {
  auto lines = GenerateCodeForTopLevelFunc(func);
  rewriter.InsertTextAfterToken(func->getBody()->getBeginLoc(),
                                llvm::join(lines, "\n"));
}
void BaseTarget::RewriteMiddleLevelFunc(REWRITE_FUNC_ARGS_DEF) {
  auto lines = GenerateCodeForMiddleLevelFunc(func);
  rewriter.InsertTextAfterToken(func->getBody()->getBeginLoc(),
                                llvm::join(lines, "\n"));
}
void BaseTarget::RewriteLowerLevelFunc(REWRITE_FUNC_ARGS_DEF) {
  auto lines = GenerateCodeForLowerLevelFunc(func);
  rewriter.InsertTextAfterToken(func->getBody()->getBeginLoc(),
                                llvm::join(lines, "\n"));
}

void BaseTarget::RewriteFuncArguments(REWRITE_FUNC_ARGS_DEF, bool top) {}
void BaseTarget::RewritePipelinedDecl(REWRITE_DECL_ARGS_DEF,
                                      const clang::Stmt *body) {}
void BaseTarget::RewritePipelinedStmt(REWRITE_STMT_ARGS_DEF,
                                      const clang::Stmt *body) {}

void BaseTarget::RewriteUnrolledDecl(REWRITE_DECL_ARGS_DEF,
                                     const clang::Stmt *body) {}
void BaseTarget::RewriteUnrolledStmt(REWRITE_STMT_ARGS_DEF,
                                     const clang::Stmt *body) {}

}  // namespace internal
}  // namespace tapa
