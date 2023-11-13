#include "task.h"

#include <cstdlib>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "clang/AST/AST.h"
#include "clang/Lex/Lexer.h"

#include "nlohmann/json.hpp"

#include "buffer.h"
#include "mmap.h"
#include "stream.h"

using std::initializer_list;
using std::string;
using std::to_string;
using std::unordered_map;
using std::vector;

using clang::CharSourceRange;
using clang::CXXBindTemporaryExpr;
using clang::CXXMemberCallExpr;
using clang::CXXMethodDecl;
using clang::CXXOperatorCallExpr;
using clang::DeclRefExpr;
using clang::DeclStmt;
using clang::Expr;
using clang::ExprWithCleanups;
using clang::FunctionDecl;
using clang::ImplicitCastExpr;
using clang::Lexer;
using clang::MaterializeTemporaryExpr;
using clang::SourceLocation;
using clang::SourceRange;
using clang::Stmt;
using clang::StringLiteral;
using clang::TapaTargetAttr;
using clang::TemplateArgument;
using clang::TemplateSpecializationType;
using clang::VarDecl;

using llvm::dyn_cast;
using llvm::join;
using llvm::StringRef;

using nlohmann::json;

namespace tapa {
namespace internal {

static std::map<TapaTargetAttr::TargetType,
                std::map<TapaTargetAttr::VendorType, Target*>>
    target_map{
        {TapaTargetAttr::TargetType::HLS,
         {
             {TapaTargetAttr::VendorType::Xilinx,
              XilinxHLSTarget::GetInstance()},
         }},
    };

extern const string* top_name;

// Given a Stmt, find the first tapa::task in its children.
const ExprWithCleanups* GetTapaTask(const Stmt* stmt) {
  for (auto child : stmt->children()) {
    if (auto expr = dyn_cast<ExprWithCleanups>(child)) {
      if (expr->getType().getAsString() == "struct tapa::task") {
        return expr;
      }
    }
  }
  return nullptr;
}

// Given a Stmt, find all tapa::task::invoke's via DFS and update invokes.
void GetTapaInvokes(const Stmt* stmt,
                    vector<const CXXMemberCallExpr*>& invokes) {
  for (auto child : stmt->children()) {
    GetTapaInvokes(child, invokes);
  }
  if (const auto invoke = dyn_cast<CXXMemberCallExpr>(stmt)) {
    if (invoke->getRecordDecl()->getQualifiedNameAsString() == "tapa::task" &&
        invoke->getMethodDecl()->getNameAsString() == "invoke") {
      invokes.push_back(invoke);
    }
  }
}

// Given a Stmt, return all tapa::task::invoke's via DFS.
vector<const CXXMemberCallExpr*> GetTapaInvokes(const Stmt* stmt) {
  vector<const CXXMemberCallExpr*> invokes;
  GetTapaInvokes(stmt, invokes);
  return invokes;
}

bool IsTapaTopLevel(const FunctionDecl* func) {
  return *top_name == func->getNameAsString();
}

thread_local const FunctionDecl* Visitor::rewriting_func{nullptr};
thread_local const FunctionDecl* Visitor::current_task{nullptr};
thread_local Target* Visitor::current_target{nullptr};

void Visitor::VisitTask(const clang::FunctionDecl* func) {
  current_task = func;

  TapaTargetAttr::TargetType target;
  TapaTargetAttr::VendorType vendor;
  if (auto attr = func->getAttr<TapaTargetAttr>()) {
    target = attr->getTarget();
    vendor = attr->getVendor();
  } else {
    target = TapaTargetAttr::TargetType::HLS;
    vendor = TapaTargetAttr::VendorType::Xilinx;
  }

  auto& metadata = GetMetadata();
  metadata["target"] = TapaTargetAttr::ConvertTargetTypeToStr(target);
  metadata["vendor"] = TapaTargetAttr::ConvertVendorTypeToStr(vendor);

  if (target_map.find(target) == target_map.end() ||
      target_map[target].find(vendor) == target_map[target].end()) {
    static const auto diagnostic_id =
        this->context_.getDiagnostics().getCustomDiagID(
            clang::DiagnosticsEngine::Error, "unsupported target: %0");
    this->context_.getDiagnostics()
        .Report(func->getLocation(), diagnostic_id)
        .AddString(std::string(metadata["target"]) + " by " +
                   std::string(metadata["vendor"]));
    current_target = XilinxHLSTarget::GetInstance();
  } else {
    current_target = target_map[target][vendor];
  }

  TraverseDecl(func->getASTContext().getTranslationUnitDecl());
}

// Apply tapa s2s transformations on a function.
bool Visitor::VisitFunctionDecl(FunctionDecl* func) {
  rewriting_func = nullptr;

  if (func->hasBody() && func->isGlobal() &&
      context_.getSourceManager().isWrittenInMainFile(func->getBeginLoc())) {
    if (rewriters_.size() == 0) {
      funcs_.push_back(func);
    } else {
      if (rewriters_.count(func) > 0) {
        rewriting_func = func;
        // Run this before the function body is purged
        HandleAttrOnNodeWithBody(func, func->getBody(), func->getAttrs());

        if (func == current_task) {
          if (auto task = GetTapaTask(func->getBody())) {
            // Run this before "extern C" is injected by
            // `ProcessUpperLevelTask`.
            if (IsTapaTopLevel(func)) {
              GetMetadata()["frt_interface"] = GetFrtInterface(func);
            }
            ProcessUpperLevelTask(task, func);
          } else {
            ProcessLowerLevelTask(func);
          }
        } else {
          current_target->RewriteFuncArguments(func, GetRewriter(),
                                               IsTapaTopLevel(func));
          if (func->hasBody()) {
            auto range = func->getBody()->getSourceRange();
            GetRewriter().ReplaceText(range, ";");
          }
        }
      }
    }
  }

  // Let the recursion continue.
  return clang::RecursiveASTVisitor<Visitor>::VisitFunctionDecl(func);
}

bool Visitor::VisitAttributedStmt(clang::AttributedStmt* stmt) {
  if (current_task && rewriting_func == current_task &&
      rewriters_.count(current_task) > 0) {
    HandleAttrOnNodeWithBody(stmt, GetLoopBody(stmt->getSubStmt()),
                             stmt->getAttrs());
  }
  return clang::RecursiveASTVisitor<Visitor>::VisitAttributedStmt(stmt);
}

// Apply tapa s2s transformations on a upper-level task.
void Visitor::ProcessUpperLevelTask(const ExprWithCleanups* task,
                                    const FunctionDecl* func) {
  current_target->RewriteFuncArguments(func, GetRewriter(),
                                       IsTapaTopLevel(func));

  if (IsTapaTopLevel(func)) {
    current_target->RewriteTopLevelFunc(func, GetRewriter());
  } else {
    current_target->RewriteMiddleLevelFunc(func, GetRewriter());
  }

  // Obtain the connection schema from the task.
  // metadata: {tasks, fifos}
  // tasks: {task_name: [{step, {args: port_name: {var_type, var_name}}}]}
  // fifos: {fifo_name: {depth, produced_by, consumed_by}}
  auto& metadata = GetMetadata();
  metadata["fifos"] = json::object();
  metadata["buffers"] = json::object();

  for (const auto param : func->parameters()) {
    const auto param_name = param->getNameAsString();
    auto add_mmap_meta = [&](const string& name) {
      metadata["ports"].push_back(
          {{"name", name},
           {"cat", IsTapaType(param, "async_mmap") ? "async_mmap" : "mmap"},
           {"width",
            GetTypeWidth(GetTemplateArg(param->getType(), 0)->getAsType())},
           {"type", GetMmapElemType(param) + "*"}});
    };
    // TODO: extend to support streams as well
    auto add_stream_meta = [&](const string& name) {
      metadata["ports"].push_back(
          {{"name", name},
           {"cat", IsTapaType(param, "istream") ? "istream" : "ostream"},
           {"width",
            GetTypeWidth(GetTemplateArg(param->getType(), 0)->getAsType())},
           {"type", GetStreamElemType(param)}});
    };
    // buffer support methods
    auto add_buffer_meta = [&](const string& name) {
      BufferConfig bufferConfig = ParseBufferType(param->getType(), false);
      auto config = bufferConfig.toJson();
      auto qualType = bufferConfig.qualType;
      config["name"] = name;
      config["cat"] = IsTapaType(param, "ibuffer") ? "ibuffer" : "obuffer";
      config["width"] = GetTypeWidthBuffer(qualType);
      metadata["ports"].push_back(config);
    };
    if (IsTapaType(param, "(async_)?mmap")) {
      add_mmap_meta(param_name);
    } else if (IsTapaType(param, "mmaps")) {
      for (int i = 0; i < GetArraySize(param); ++i) {
        add_mmap_meta(param_name + "[" + to_string(i) + "]");
      }
    } else if (IsStreamInterface(param)) {
      add_stream_meta(param_name);
    } else if (IsBufferInterface(param)) {
      // TODO: Note that `buffers` similar to `streams` gets treated as
      // scalars. Do we need to change that?
      add_buffer_meta(param_name);
    } else {
      metadata["ports"].push_back({{"name", param_name},
                                   {"cat", "scalar"},
                                   {"width", GetTypeWidth(param->getType())},
                                   {"type", param->getType().getAsString()}});
    }
  }

  // Process stream declarations.
  unordered_map<string, const VarDecl*> fifo_decls;
  unordered_map<string, const VarDecl*> buffer_decls;
  for (const auto child : func->getBody()->children()) {
    if (const auto decl_stmt = dyn_cast<DeclStmt>(child)) {
      if (const auto var_decl = dyn_cast<VarDecl>(*decl_stmt->decl_begin())) {
        if (auto decl = GetTapaStreamDecl(var_decl->getType())) {
          const auto args = decl->getTemplateArgs().asArray();
          const string elem_type = GetTemplateArgName(args[0]);
          const uint64_t fifo_depth{*args[1].getAsIntegral().getRawData()};
          const string var_name{var_decl->getNameAsString()};
          metadata["fifos"][var_name]["depth"] = fifo_depth;
          fifo_decls[var_name] = var_decl;
        } else if (auto decl = GetTapaStreamsDecl(var_decl->getType())) {
          const auto args = decl->getTemplateArgs().asArray();
          const string elem_type = GetTemplateArgName(args[0]);
          const uint64_t fifo_depth = *args[2].getAsIntegral().getRawData();
          for (int i = 0; i < GetArraySize(decl); ++i) {
            const string var_name = ArrayNameAt(var_decl->getNameAsString(), i);
            metadata["fifos"][var_name]["depth"] = fifo_depth;
            fifo_decls[var_name] = var_decl;
          }
        } else if (auto decl = GetTapaBufferDecl(var_decl->getType())) {
          // TODO: do we need to provide all the buffer config related
          // information here? we may not need to
          BufferConfig bufferConfig =
              ParseBufferType(var_decl->getType(), false);
          auto config = bufferConfig.toJson();
          auto qualType = bufferConfig.qualType;
          config["width"] = GetTypeWidthBuffer(qualType);
          const string var_name{var_decl->getNameAsString()};
          // produced_by and consumed_by comes later, so I think we can directly
          // set this
          config["is_instantiated"] = true;
          metadata["buffers"][var_name] = config;
          buffer_decls[var_name] = var_decl;
        } else if (auto decl = GetTapaBuffersDecl(var_decl->getType())) {
          BufferConfig bufferConfig =
              ParseBufferType(var_decl->getType(), true);
          auto config = bufferConfig.toJson();
          auto qualType = bufferConfig.qualType;
          config["width"] = GetTypeWidthBuffer(qualType);
          auto baseName = var_decl->getNameAsString();
          for (int i = 0; i < bufferConfig.length; i++) {
            const string var_name = ArrayNameAt(baseName, i);
            config["is_instantiated"] = true;
            metadata["buffers"][var_name] = config;
            buffer_decls[var_name] = var_decl;
          }
        }
      }
    }
  }

  // Instanciate tasks.
  vector<const CXXMemberCallExpr*> invokes = GetTapaInvokes(task);

  unordered_map<string, int> istreams_access_pos;
  unordered_map<string, int> ostreams_access_pos;
  unordered_map<string, int> ibuffers_access_pos;
  unordered_map<string, int> obuffers_access_pos;
  unordered_map<string, int> mmaps_access_pos;
  unordered_map<const Expr*, int> seq_access_pos;

  for (auto invoke : invokes) {
    int step = -1;
    bool has_name = false;
    uint64_t vec_length = 1;
    if (const auto method = dyn_cast<CXXMethodDecl>(invoke->getCalleeDecl())) {
      auto args = method->getTemplateSpecializationArgs()->asArray();
      if (args.size() > 0 && args[0].getKind() == TemplateArgument::Integral) {
        step =
            *reinterpret_cast<const int*>(args[0].getAsIntegral().getRawData());
      } else {
        step = 0;  // default to join
      }
      if (args.size() > 1 && args[1].getKind() == TemplateArgument::Integral) {
        vec_length = *args[1].getAsIntegral().getRawData();
      }
      if (args.rbegin()->getKind() == TemplateArgument::Integral) {
        has_name = true;
      }
    } else {
      static const auto diagnostic_id =
          this->context_.getDiagnostics().getCustomDiagID(
              clang::DiagnosticsEngine::Error, "unexpected invocation: %0");
      this->context_.getDiagnostics()
          .Report(invoke->getCallee()->getBeginLoc(), diagnostic_id)
          .AddString(invoke->getStmtClassName());
    }
    const FunctionDecl* task = nullptr;
    string task_name;
    auto get_name = [&](const string& name, uint64_t i,
                        const DeclRefExpr* decl_ref) -> string {
      if (IsTapaType(decl_ref, "(mmaps|(i|o)?streams|(i|o)?buffers)")) {
        const auto ts_type =
            decl_ref->getType()->getAs<TemplateSpecializationType>();
        assert(ts_type != nullptr);
        assert(ts_type->getNumArgs() > 1);
        const auto length = this->EvalAsInt(ts_type->getArg(1).getAsExpr());
        if (i >= length) {
          auto& diagnostics = context_.getDiagnostics();
          static const auto diagnostic_id =
              diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Remark,
                                          "invocation #%0 accesses '%1[%2]'");
          auto diagnostics_builder =
              diagnostics.Report(decl_ref->getBeginLoc(), diagnostic_id);
          diagnostics_builder.AddString(to_string(i));
          diagnostics_builder.AddString(decl_ref->getNameInfo().getAsString());
          diagnostics_builder.AddString(to_string(i % length));
          diagnostics_builder.AddString(decl_ref->getType().getAsString());
          diagnostics_builder.AddSourceRange(
              GetCharSourceRange(decl_ref->getSourceRange()));
        }
        return ArrayNameAt(name, i % length);
      }
      return name;
    };
    for (uint64_t i_vec = 0; i_vec < vec_length; ++i_vec) {
      for (unsigned i = 0; i < invoke->getNumArgs(); ++i) {
        const auto arg = invoke->getArg(i);
        const auto decl_ref = dyn_cast<DeclRefExpr>(arg);  // a variable
        clang::Expr::EvalResult arg_eval_as_int_result;
        const bool arg_is_int =
            arg->EvaluateAsInt(arg_eval_as_int_result, this->context_);

        // element in an array
        auto op_call = dyn_cast<CXXOperatorCallExpr>(arg);
        if (op_call == nullptr) {
          const auto materialize_temporary =
              dyn_cast<MaterializeTemporaryExpr>(arg);
          if (materialize_temporary) {
            const auto bind_temporary = dyn_cast<CXXBindTemporaryExpr>(
                materialize_temporary->GetTemporaryExpr());
            if (bind_temporary) {
              op_call =
                  dyn_cast<CXXOperatorCallExpr>(bind_temporary->getSubExpr());
            }
          }
        }

        const auto arg_is_seq = IsTapaType(arg, "seq");
        if (decl_ref || op_call || arg_is_int || arg_is_seq) {
          string arg_name;
          if (decl_ref) {
            arg_name = decl_ref->getNameInfo().getName().getAsString();
          }
          if (op_call) {
            auto decl_ref = dyn_cast<DeclRefExpr>(op_call->getArg(0));
            if (decl_ref == nullptr) {
              const auto implicit_cast =
                  dyn_cast<ImplicitCastExpr>(op_call->getArg(0));
              if (implicit_cast) {
                decl_ref = dyn_cast<DeclRefExpr>(implicit_cast->getSubExpr());
              }
            }
            const auto array_name =
                decl_ref->getNameInfo().getName().getAsString();
            const auto array_idx = this->EvalAsInt(op_call->getArg(1));
            arg_name = ArrayNameAt(array_name, array_idx);
          }
          if (arg_is_int) {
            arg_name = "64'd" +
                       std::to_string(uint64_t(
                           arg_eval_as_int_result.Val.getInt().getExtValue()));
          }
          if (i == 0) {
            task_name = arg_name;
            metadata["tasks"][task_name].push_back({{"step", step}});
            task = decl_ref->getDecl()->getAsFunction();
          } else {
            assert(task != nullptr);
            auto param = task->getParamDecl(has_name ? i - 2 : i - 1);
            auto param_name = param->getNameAsString();
            string param_cat;

            // register this argument to task
            auto register_arg = [&](string arg = "", string port = "") {
              if (arg.empty())
                arg = arg_name;  // use global arg_name by default
              if (port.empty()) port = param_name;
              (*metadata["tasks"][task_name].rbegin())["args"][port] = {
                  {"cat", param_cat}, {"arg", arg}};
            };

            // regsiter stream info to task
            auto register_fifo_consumer = [&, ast_arg = arg](string arg = "") {
              // use global arg_name by default
              if (arg.empty()) arg = arg_name;
              if (metadata["fifos"][arg].contains("consumed_by")) {
                static const auto diagnostic_id =
                    this->context_.getDiagnostics().getCustomDiagID(
                        clang::DiagnosticsEngine::Error,
                        "tapa::stream '%0' consumed more than once");
                auto diagnostics_builder =
                    this->context_.getDiagnostics().Report(
                        ast_arg->getBeginLoc(), diagnostic_id);
                diagnostics_builder.AddString(arg);
                diagnostics_builder.AddSourceRange(GetCharSourceRange(ast_arg));
              }
              metadata["fifos"][arg]["consumed_by"] = {
                  task_name, metadata["tasks"][task_name].size() - 1};
            };
            auto register_fifo_producer = [&, ast_arg = arg](string arg = "") {
              // use global arg_name by default
              if (arg.empty()) arg = arg_name;
              if (metadata["fifos"][arg].contains("produced_by")) {
                static const auto diagnostic_id =
                    this->context_.getDiagnostics().getCustomDiagID(
                        clang::DiagnosticsEngine::Error,
                        "tapa::stream '%0' produced more than once");
                auto diagnostics_builder =
                    this->context_.getDiagnostics().Report(
                        ast_arg->getBeginLoc(), diagnostic_id);
                diagnostics_builder.AddString(arg);
                diagnostics_builder.AddSourceRange(GetCharSourceRange(ast_arg));
              }
              metadata["fifos"][arg]["produced_by"] = {
                  task_name, metadata["tasks"][task_name].size() - 1};
            };
            // regsiter buffer info to task
            auto register_buffer_consumer = [&, ast_arg = arg](
                                                string arg = "",
                                                nlohmann::json & config) {
              // use global arg_name by default
              if (arg.empty()) arg = arg_name;
              if (metadata["buffers"][arg].contains("consumed_by")) {
                static const auto diagnostic_id =
                    this->context_.getDiagnostics().getCustomDiagID(
                        clang::DiagnosticsEngine::Error,
                        "tapa::buffer '%0' consumed more than once");
                auto diagnostics_builder =
                    this->context_.getDiagnostics().Report(
                        ast_arg->getBeginLoc(), diagnostic_id);
                diagnostics_builder.AddString(arg);
                diagnostics_builder.AddSourceRange(GetCharSourceRange(ast_arg));
              }
              config["consumed_by"] = {task_name,
                                       metadata["tasks"][task_name].size() - 1};
              metadata["buffers"][arg].update(config);
            };
            auto register_buffer_producer = [&, ast_arg = arg](
                                                string arg = "",
                                                nlohmann::json & config) {
              // use global arg_name by default
              if (arg.empty()) arg = arg_name;
              if (metadata["buffers"][arg].contains("produced_by")) {
                static const auto diagnostic_id =
                    this->context_.getDiagnostics().getCustomDiagID(
                        clang::DiagnosticsEngine::Error,
                        "tapa::buffer '%0' produced more than once");
                auto diagnostics_builder =
                    this->context_.getDiagnostics().Report(
                        ast_arg->getBeginLoc(), diagnostic_id);
                diagnostics_builder.AddString(arg);
                diagnostics_builder.AddSourceRange(GetCharSourceRange(ast_arg));
              }
              config["produced_by"] = {task_name,
                                       metadata["tasks"][task_name].size() - 1};
              metadata["buffers"][arg].update(config);
            };
            if (IsTapaType(param, "mmap")) {
              param_cat = "mmap";
              // vector invocation can map mmaps to mmap
              register_arg(
                  get_name(arg_name, mmaps_access_pos[arg_name]++, decl_ref));

            } else if (IsTapaType(param, "async_mmap")) {
              param_cat = "async_mmap";
              // vector invocation can map mmaps to async_mmap
              register_arg(
                  get_name(arg_name, mmaps_access_pos[arg_name]++, decl_ref));
            } else if (IsTapaType(param, "istream")) {
              param_cat = "istream";
              // vector invocation can map istreams to istream
              auto arg =
                  get_name(arg_name, istreams_access_pos[arg_name]++, decl_ref);
              register_fifo_consumer(arg);
              register_arg(arg);
            } else if (IsTapaType(param, "ostream")) {
              param_cat = "ostream";
              // vector invocation can map ostreams to ostream
              auto arg =
                  get_name(arg_name, ostreams_access_pos[arg_name]++, decl_ref);
              register_fifo_producer(arg);
              register_arg(arg);
            } else if (IsTapaType(param, "istreams")) {
              param_cat = "istream";
              for (int i = 0; i < GetArraySize(param); ++i) {
                auto arg = get_name(arg_name, istreams_access_pos[arg_name]++,
                                    decl_ref);
                register_fifo_consumer(arg);
                register_arg(arg, ArrayNameAt(param_name, i));
              }
            } else if (IsTapaType(param, "ostreams")) {
              param_cat = "ostream";
              for (int i = 0; i < GetArraySize(param); ++i) {
                auto arg = get_name(arg_name, ostreams_access_pos[arg_name]++,
                                    decl_ref);
                register_fifo_producer(arg);
                register_arg(arg, ArrayNameAt(param_name, i));
              }
            } else if (IsTapaType(param, "ibuffer")) {
              param_cat = "ibuffer";
              // get the BufferConfig
              BufferConfig bufferConfig =
                  ParseBufferType(param->getType(), false);
              auto config = bufferConfig.toJson();
              auto qualType = bufferConfig.qualType;
              config["width"] = GetTypeWidthBuffer(qualType);
              // vector invocation can map ibuffers to ibuffer
              auto arg =
                  get_name(arg_name, ibuffers_access_pos[arg_name]++, decl_ref);
              register_buffer_consumer(arg, config);
              register_arg(arg);
            } else if (IsTapaType(param, "obuffer")) {
              param_cat = "obuffer";
              // get the BufferConfig
              BufferConfig bufferConfig =
                  ParseBufferType(param->getType(), false);
              auto config = bufferConfig.toJson();
              auto qualType = bufferConfig.qualType;
              config["width"] = GetTypeWidthBuffer(qualType);
              // vector invocation can map obuffers to obuffer
              auto arg =
                  get_name(arg_name, obuffers_access_pos[arg_name]++, decl_ref);
              register_buffer_producer(arg, config);
              register_arg(arg);
            } else if (IsTapaType(param, "ibuffers")) {
              // get the BufferConfig
              BufferConfig bufferConfig =
                  ParseBufferType(param->getType(), true);
              auto config = bufferConfig.toJson();
              auto qualType = bufferConfig.qualType;
              config["width"] = GetTypeWidthBuffer(qualType);
              param_cat = "ibuffer";
              for (int i = 0; i < GetArraySize(param); ++i) {
                auto arg = get_name(arg_name, ibuffers_access_pos[arg_name]++,
                                    decl_ref);
                register_buffer_consumer(arg, config);
                register_arg(arg, ArrayNameAt(param_name, i));
              }
            } else if (IsTapaType(param, "obuffers")) {
              // get the BufferConfig
              BufferConfig bufferConfig =
                  ParseBufferType(param->getType(), true);
              auto config = bufferConfig.toJson();
              auto qualType = bufferConfig.qualType;
              config["width"] = GetTypeWidthBuffer(qualType);
              param_cat = "obuffer";
              for (int i = 0; i < GetArraySize(param); ++i) {
                auto arg = get_name(arg_name, obuffers_access_pos[arg_name]++,
                                    decl_ref);
                register_buffer_producer(arg, config);
                register_arg(arg, ArrayNameAt(param_name, i));
              }
            } else if (arg_is_seq) {
              param_cat = "scalar";
              register_arg("64'd" + std::to_string(seq_access_pos[arg]++));
            } else {
              param_cat = "scalar";
              register_arg();
            }
          }
          continue;
        } else if (const auto string_literal = dyn_cast<StringLiteral>(arg)) {
          if (i == 1 && has_name) {
            (*metadata["tasks"][task_name].rbegin())["name"] =
                string_literal->getString();
            continue;
          }
        }
        static const auto diagnostic_id =
            this->context_.getDiagnostics().getCustomDiagID(
                clang::DiagnosticsEngine::Error, "unexpected argument: %0");
        auto diagnostics_builder = this->context_.getDiagnostics().Report(
            arg->getBeginLoc(), diagnostic_id);
        diagnostics_builder.AddString(arg->getStmtClassName());
        diagnostics_builder.AddSourceRange(GetCharSourceRange(arg));
      }
    }
  }

  for (auto fifo = metadata["fifos"].begin();
       fifo != metadata["fifos"].end();) {
    const auto is_consumed = fifo.value().contains("consumed_by");
    const auto is_produced = fifo.value().contains("produced_by");
    const auto& fifo_name = fifo.key();
    const auto fifo_decl = fifo_decls.find(fifo_name);
    auto& diagnostics = context_.getDiagnostics();
    if (!is_consumed && !is_produced) {
      static const auto diagnostic_id = diagnostics.getCustomDiagID(
          clang::DiagnosticsEngine::Warning, "unused stream: %0");
      auto diagnostics_builder =
          diagnostics.Report(fifo_decl->second->getBeginLoc(), diagnostic_id);
      diagnostics_builder.AddString(fifo_name);
      diagnostics_builder.AddSourceRange(
          GetCharSourceRange(fifo_decl->second->getSourceRange()));
      fifo = metadata["fifos"].erase(fifo);
    } else {
      ++fifo;
      if (fifo_decl != fifo_decls.end() && is_consumed != is_produced) {
        static const auto consumed_diagnostic_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "consumed but not produced stream: %0");
        static const auto produced_diagnostic_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "produced but not consumed stream: %0");
        auto diagnostics_builder = diagnostics.Report(
            fifo_decl->second->getBeginLoc(),
            is_consumed ? consumed_diagnostic_id : produced_diagnostic_id);
        diagnostics_builder.AddString(fifo_name);
        diagnostics_builder.AddSourceRange(
            GetCharSourceRange(fifo_decl->second->getSourceRange()));
      }
    }
  }

  for (auto buffer = metadata["buffers"].begin();
       buffer != metadata["buffers"].end();) {
    const auto is_consumed = buffer.value().contains("consumed_by");
    const auto is_produced = buffer.value().contains("produced_by");
    const auto& buffer_name = buffer.key();
    const auto buffer_decl = buffer_decls.find(buffer_name);
    auto& diagnostics = context_.getDiagnostics();
    if (!is_consumed && !is_produced) {
      static const auto diagnostic_id = diagnostics.getCustomDiagID(
          clang::DiagnosticsEngine::Warning, "unused buffer: %0");
      auto diagnostics_builder =
          diagnostics.Report(buffer_decl->second->getBeginLoc(), diagnostic_id);
      diagnostics_builder.AddString(buffer_name);
      diagnostics_builder.AddSourceRange(
          GetCharSourceRange(buffer_decl->second->getSourceRange()));
      buffer = metadata["buffers"].erase(buffer);
    } else {
      ++buffer;
      if (buffer_decl != buffer_decls.end() && is_consumed != is_produced) {
        static const auto consumed_diagnostic_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "consumed but not produced buffer: %0");
        static const auto produced_diagnostic_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "produced but not consumed buffer: %0");
        auto diagnostics_builder = diagnostics.Report(
            buffer_decl->second->getBeginLoc(),
            is_consumed ? consumed_diagnostic_id : produced_diagnostic_id);
        diagnostics_builder.AddString(buffer_name);
        diagnostics_builder.AddSourceRange(
            GetCharSourceRange(buffer_decl->second->getSourceRange()));
      }
    }
  }
}

// Apply tapa s2s transformations on a lower-level task.
void Visitor::ProcessLowerLevelTask(const FunctionDecl* func) {
  current_target->RewriteLowerLevelFunc(func, GetRewriter());
}

string Visitor::GetFrtInterface(const FunctionDecl* func) {
  auto func_body_source_range = func->getBody()->getSourceRange();
  auto& source_manager = context_.getSourceManager();
  auto main_file_id = source_manager.getMainFileID();

  vector<string> content;
  content.reserve(5 + func->getNumParams());

  // Content before the function body.
  content.push_back(join(
      initializer_list<StringRef>{
          "#include <sstream>",
          "#include <stdexcept>",
          "#include <frt.h>",
          "\n",
      },
      "\n"));
  content.push_back(GetRewriter().getRewrittenText(
      SourceRange(source_manager.getLocForStartOfFile(main_file_id),
                  func_body_source_range.getBegin())));

  // Function body.
  content.push_back(join(
      initializer_list<StringRef>{
          "\n#define TAPAB_APP \"TAPAB_",
          func->getNameAsString(),
          "\"\n",
      },
      ""));
  content.push_back(R"(#define TAPAB "TAPAB"
  const char* _tapa_bitstream = nullptr;
  if ((_tapa_bitstream = getenv(TAPAB_APP)) ||
      (_tapa_bitstream = getenv(TAPAB))) {
    fpga::Instance _tapa_instance(_tapa_bitstream);
    int _tapa_arg_index = 0;
    for (const auto& _tapa_arg_info : _tapa_instance.GetArgsInfo()) {
      if (false) {)");
  for (auto param : func->parameters()) {
    const auto name = param->getNameAsString();
    if (IsTapaType(param, "(async_)?mmaps?")) {
      // TODO: Leverage kernel information.
      bool write_device = true;
      bool read_device =
          !GetTemplateArg(param->getType(), 0)->getAsType().isConstQualified();
      auto direction =
          write_device ? (read_device ? "ReadWrite" : "WriteOnly") : "ReadOnly";
      auto add_param = [&content, direction](StringRef name, StringRef var) {
        content.push_back(join(
            initializer_list<StringRef>{
                R"(
      } else if (_tapa_arg_info.name == ")",
                name,
                R"(") {
        auto _tapa_arg = fpga::)",
                direction,
                R"(()",
                var,
                R"(.get(), )",
                var,
                R"(.size());
        _tapa_instance.SetArg(_tapa_arg_index, _tapa_arg);)",
            },
            ""));
      };
      if (IsTapaType(param, "(async_)?mmaps")) {
        const uint64_t array_size = GetArraySize(param);
        for (uint64_t i = 0; i < array_size; ++i) {
          add_param(GetArrayElem(name, i), ArrayNameAt(name, i));
        }
      } else {
        add_param(name, name);
      }
    } else if (IsTapaType(param, "(i|o)streams?")) {
      content.push_back("\n#error stream not supported yet\n");
    } else {
      content.push_back(join(
          initializer_list<StringRef>{
              R"(
      } else if (_tapa_arg_info.name == ")",
              name,
              R"(") {
        _tapa_instance.SetArg(_tapa_arg_index, )",
              name,
              R"();)",
          },
          ""));
    }
  }
  content.push_back(
      R"(
      } else {
        std::stringstream ss;
        ss << "unknown argument: " << _tapa_arg_info;
        throw std::runtime_error(ss.str());
      }
      ++_tapa_arg_index;
    }
    _tapa_instance.WriteToDevice();
    _tapa_instance.Exec();
    _tapa_instance.ReadFromDevice();
    _tapa_instance.Finish();
  } else {
    throw std::runtime_error("no bitstream found; please set `" TAPAB_APP
                             "` or `" TAPAB "`");
  }
)");

  // Content after the function body.
  content.push_back(GetRewriter().getRewrittenText(
      SourceRange(func_body_source_range.getEnd(),
                  source_manager.getLocForEndOfFile(main_file_id))));

  // Join everything together (without excessive copying).
  return llvm::join(content.begin(), content.end(), "");
}

SourceLocation Visitor::GetEndOfLoc(SourceLocation loc) {
  return loc.getLocWithOffset(Lexer::MeasureTokenLength(
      loc, GetRewriter().getSourceMgr(), GetRewriter().getLangOpts()));
}
CharSourceRange Visitor::GetCharSourceRange(SourceRange range) {
  return CharSourceRange::getCharRange(range.getBegin(),
                                       GetEndOfLoc(range.getEnd()));
}
CharSourceRange Visitor::GetCharSourceRange(const Stmt* stmt) {
  return GetCharSourceRange(stmt->getSourceRange());
}

int64_t Visitor::EvalAsInt(const Expr* expr) {
  clang::Expr::EvalResult result;
  if (expr->EvaluateAsInt(result, this->context_)) {
    return result.Val.getInt().getExtValue();
  }
  static const auto diagnostic_id =
      this->context_.getDiagnostics().getCustomDiagID(
          clang::DiagnosticsEngine::Error,
          "fail to evaluate as integer at compile time");
  this->context_.getDiagnostics()
      .Report(expr->getBeginLoc(), diagnostic_id)
      .AddSourceRange(this->GetCharSourceRange(expr));
  return -1;
}

template <typename T>
void Visitor::HandleAttrOnNodeWithBody(
    const T* node, const clang::Stmt* body,
    llvm::ArrayRef<const clang::Attr*> attrs) {
#define HANDLE_ATTR(FUNC_DECL, FUNC_STMT)                                      \
  if (std::is_base_of<clang::Decl, T>()) {                                     \
    current_target->FUNC_DECL((const clang::Decl*)(node), attr, GetRewriter(), \
                              body);                                           \
  } else if (std::is_base_of<clang::Stmt, T>()) {                              \
    current_target->FUNC_STMT((const clang::Stmt*)(node), attr, GetRewriter(), \
                              body);                                           \
  }

  for (const auto* attr : attrs) {
    if (clang::isa<clang::TapaPipelineAttr>(attr)) {
      HANDLE_ATTR(RewritePipelinedDecl, RewritePipelinedStmt);
    } else if (clang::isa<clang::TapaUnrollAttr>(attr)) {
      HANDLE_ATTR(RewriteUnrolledDecl, RewriteUnrolledStmt);
    }
  }
}

}  // namespace internal
}  // namespace tapa
