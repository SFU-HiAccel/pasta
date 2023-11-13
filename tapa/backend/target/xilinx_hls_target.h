#ifndef TAPA_XILINX_HLS_TARGET_H_
#define TAPA_XILINX_HLS_TARGET_H_

#include "base_target.h"

namespace tapa {
namespace internal {

class XilinxHLSTarget : public BaseTarget {
 public:
  virtual void AddCodeForTopLevelFunc(ADD_FOR_FUNC_ARGS_DEF);
  virtual void AddCodeForTopLevelStream(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForMiddleLevelStream(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForLowerLevelStream(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForTopLevelBuffer(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForMiddleLevelBuffer(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForLowerLevelBuffer(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForTopLevelMmap(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForMiddleLevelMmap(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForLowerLevelMmap(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForTopLevelAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForMiddleLevelAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForLowerLevelAsyncMmap(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForTopLevelScalar(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void AddCodeForMiddleLevelScalar(ADD_FOR_PARAMS_ARGS_DEF);
  virtual void RewriteTopLevelFunc(REWRITE_FUNC_ARGS_DEF);
  virtual void RewriteMiddleLevelFunc(REWRITE_FUNC_ARGS_DEF);
  virtual void RewriteFuncArguments(REWRITE_FUNC_ARGS_DEF, bool top);
  virtual void RewritePipelinedDecl(REWRITE_DECL_ARGS_DEF,
                                    const clang::Stmt *body);
  virtual void RewritePipelinedStmt(REWRITE_STMT_ARGS_DEF,
                                    const clang::Stmt *body);
  virtual void RewriteUnrolledStmt(REWRITE_STMT_ARGS_DEF,
                                   const clang::Stmt *body);

  static tapa::internal::Target *GetInstance() {
    static XilinxHLSTarget instance;
    return &instance;
  }

  XilinxHLSTarget(XilinxHLSTarget const &) = delete;
  void operator=(XilinxHLSTarget const &) = delete;

 protected:
  XilinxHLSTarget() {}
};

}  // namespace internal
}  // namespace tapa

#endif  // TAPA_XILINX_HLS_TARGET_H_
