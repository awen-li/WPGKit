#include "CallGraphPTA.h"
#include "InterCFG.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

using namespace wpgkit;

namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void add(CallGraphSession &session, const std::string &code,
         const char *file = "fprintf.cpp") {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      code, {"-std=c++20", "-Wno-format", "-Wno-format-extra-args"}, file);
  check(ast != nullptr, "parse fprintf fixture");
  session.addUnit(ast->getASTContext());
} // Format strings and the AST are destroyed before model application.

CallGraphResult solve(CallGraphSession &session, bool final = true) {
  auto result =
      final ? session.finalize({"entry"}) : session.analyze({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError());
    std::exit(1);
  }
  return std::move(*result);
}

const CallSiteResult &call(const CallGraphResult &result, const char *name) {
  for (const auto &site : result.calls)
    for (const auto target : site.targets)
      if (result.metadata.text(result.names.at(target)) == name)
        return site;
  check(false, "expected call target missing");
  std::abort();
}

void contracts() {
  const auto contract = ExternalModules::formattedSummary(
      ExternalModel::Fprintf, FormatEffect::OutputOnly);
  check(
      contract.complete() && contract.has(CallEffect::WriteStream) &&
          contract.has(CallEffect::FloatingEnvironment) &&
          contract.has(CallEffect::WriteErrno) &&
          contract.has(CallEffect::ReadArguments) &&
          contract.has(CallEffect::InputToReturn) &&
          contract.has(CallEffect::InputToMemory) &&
          !contract.has(CallEffect::WriteArguments) &&
          !contract.has(CallEffect::WriteGlobals) &&
          !contract.has(CallEffect::Callback),
      "ordinary stdio preserves I/O and result effects without arbitrary pointer writes");
  const auto dynamic = ExternalModules::formattedSummary(ExternalModel::Fprintf,
                                                         FormatEffect::Dynamic);
  check(dynamic.complete() && dynamic.has(CallEffect::WriteArguments) &&
            !dynamic.has(CallEffect::Callback) &&
            !dynamic.has(CallEffect::WriteGlobals),
        "dynamic standard formats retain bounded count writes");
  check(!ExternalModules::isIntrinsic(ExternalModel::Fprintf),
        "runtime output model must wait for later supplied implementations");
}

using FormatCase = std::pair<const char *, const char *>;

void outputFormats() {
  // Check actual stdio formats, including escaped percent and positional/length
  // modifiers. Bytes after a NUL are not interpreted as format directives.
  for (const auto &[format, arguments] :
       {FormatCase{"\"hello\"", ""},
        {"\"%s %d %p\"", ", \"text\", 4, (void*)&cb"},
        {"\"%%n\"", ""},
        {"\"%2$.*1$s\"", ", 4, \"text\""},
        {"\"%ls\"", ", L\"wide\""},
        {"\"ok\\0%n\"", ", &n"}}) {
    CallGraphSession session;
    add(session,
        std::string{
            "#include <cstdio>\nvoid target() {} "
            "void entry(FILE *stream) { int n = 0; auto cb = &target; fprintf(stream,"} +
            format + arguments + "); cb(); }");
    const auto before = solve(session, false);
    check(before.statistics.modeledCalls == 0 &&
              call(before, "fprintf").unresolved,
          "runtime model remains provisional before finalization");
    const auto result = solve(session);
    const auto &output = call(result, "fprintf");
    check(
        !output.unresolved && output.externalEffects.complete() &&
            output.externalEffects.has(CallEffect::WriteStream) &&
            !call(result, "target").unresolved &&
            result.statistics.unknownCalls == 0,
        "output model leaves unrelated callback targets precise after AST release");
    const auto &graph = *result.interCFG;
    bool retained = false;
    for (const auto &summary : graph.summaries())
      if (summary.site == output.id)
        retained |= summary.effects.complete() &&
                    summary.effects.has(CallEffect::WriteStream);
    check(retained, "inter-CFG retains the known output summary");
    check(solve(session).statistics == result.statistics,
          "repeated finalization does not replay a runtime model");
  }
}

void countAndUnsupportedFormats() {
  for (const auto *format : {"\"%n\"", "\"%hn\"", "\"%lln\"", "\"%1$n\"",
                             "\"%Q\"", "\"bad%\"", "format"}) {
    CallGraphSession session;
    const auto type = std::string(format) == "\"%hn\""    ? "short"
                      : std::string(format) == "\"%lln\"" ? "long long"
                                                          : "int";
    add(session,
        std::string{
            "#include <cstdio>\nvoid entry(FILE *stream, const char *format) {"} +
            type + " n = 0; fprintf(stream," + format + ", &n); }");
    const auto result = solve(session);
    const auto &output = call(result, "fprintf");
    const bool unsupported =
        std::string(format) == "\"%Q\"" || std::string(format) == "\"bad%\"";
    check(
        output.unresolved == unsupported &&
            output.externalEffects.complete() != unsupported &&
            output.externalEffects.has(CallEffect::WriteArguments),
        "count writes have a complete bounded contract; unsupported literals stay open");
  }
}

void indirectFormats() {
  CallGraphSession indirect;
  add(indirect, "#include <cstdio>\nvoid entry(FILE *stream) {"
                "auto print = &fprintf; print(stream, \"%d\", 42); }");
  const auto indirectResult = solve(indirect);
  check(!call(indirectResult, "fprintf").unresolved &&
            indirectResult.statistics.modeledCalls == 1,
        "indirect fprintf targets use facts captured before target discovery");

  // The format has a different position in each family. Check indirect calls
  // too: their callee may become known only after the AST has been destroyed.
  for (const auto &[name, prefix] : {FormatCase{"printf", ""},
                                     {"sprintf", "buffer,"},
                                     {"snprintf", "buffer,sizeof(buffer),"}}) {
    for (const auto *format : {"\"%d\"", "\"%n\"", "format"}) {
      const bool checked = std::string(format) == "\"%d\"";
      CallGraphSession formatted;
      add(formatted,
          std::string{"#include <cstdio>\nvoid entry(const char *format) {"
                      "char buffer[32]; int value = 0; auto output = &"} +
              name + "; output(" + prefix + format +
              (checked ? ", value); }" : ", &value); }"));
      const auto formattedResult = solve(formatted);
      const auto &output = call(formattedResult, name);
      check(
          output.externalEffects.complete() && !output.unresolved,
          "indirect standard formatting uses its argument positions for bounded writes");
      check(
          output.externalEffects.has(std::string(name) == "printf"
                                         ? CallEffect::WriteStream
                                         : CallEffect::WriteArguments),
          "formatted buffer and stream outputs retain distinct memory effects");
    }
  }
}

void recognitionAndReplacement() {
  CallGraphSession unrelated;
  add(unrelated, "namespace user { int fprintf(void*,const char*,...); }"
                 "void entry() { user::fprintf(nullptr, \"message\"); }");
  const auto unrelatedResult = solve(unrelated);
  check(call(unrelatedResult, "user::fprintf").unresolved &&
            unrelatedResult.statistics.modeledCalls == 0,
        "same-spelled user function does not receive the stdio model");

  CallGraphSession replacement;
  add(replacement,
      "#include <cstdio>\nvoid entry(FILE *stream) { fprintf(stream, \"message\"); }",
      "caller.cpp");
  check(solve(replacement, false).statistics.modeledCalls == 0,
        "output model is not applied before late definitions can arrive");
  add(replacement,
      "#include <cstdio>\nvoid replacement_effect() {}"
      "extern \"C\" int fprintf(FILE *, const char *, ...) { replacement_effect(); return 0; }",
      "replacement.cpp");
  const auto supplied = solve(replacement);
  check(supplied.statistics.modeledCalls == 0 &&
            !call(supplied, "replacement_effect").unresolved,
        "supplied fprintf implementation and its effects take precedence");
}

void countStoreAliases() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <cstdio>
    void target() {}
    void entry() {
      auto slot = &target;
      printf("%hhn", reinterpret_cast<signed char *>(&slot));
      slot();
    }
  )cpp");
  const auto result = solve(session);
  check(
      !call(result, "printf").unresolved && call(result, "target").unresolved,
      "count stores weaken aliased pointer bytes without making printf itself unknown");
}

void variadicLists() {
  CallGraphSession session;
  add(session, R"cpp(
    #include <cstdio>
    #include <cstdarg>
    void output(FILE *stream, const char *format, ...) {
      char buffer[64]; va_list ap, copied;
      va_start(ap, format); va_copy(copied, ap);
      vfprintf(stream, format, ap);
      vsnprintf(buffer, sizeof(buffer), format, copied);
      va_end(ap); va_end(copied);
    }
    void entry(FILE *stream, const char *format) {
      int count = 0; output(stream, format, &count);
    }
  )cpp");
  const auto result = solve(session);
  for (const auto *name : {"__builtin_va_start", "__builtin_va_copy",
                           "__builtin_va_end", "vfprintf", "vsnprintf"})
    check(!call(result, name).unresolved &&
              call(result, name).externalEffects.complete(),
          "va_list lifecycle and formatting have bounded memory contracts");
  check(solve(session).statistics == result.statistics,
        "va_list summaries do not replay on unchanged solving");
}

void unsupportedListABI() {
  CallGraphSession session;
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      R"cpp(
        # 1 "system.h" 1 3
        extern "C" int vsnprintf(char *, unsigned long long, const char *,
                                  __builtin_va_list);
        # 1 "user.cpp" 2
        void entry(const char *format, ...) {
          __builtin_va_list ap;
          __builtin_va_start(ap, format);
          char buffer[16]; vsnprintf(buffer, sizeof(buffer), format, ap);
          __builtin_va_end(ap);
        }
      )cpp",
      {"-std=c++20", "--target=x86_64-pc-windows-msvc"}, "windows-list.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse pointer-valued va_list ABI");
  session.addUnit(ast->getASTContext());
  ast.reset();
  const auto result = solve(session);
  for (const auto *name :
       {"__builtin_va_start", "__builtin_va_end", "vsnprintf"})
    check(call(result, name).unresolved,
          "unrepresented list lvalues must retain unknown effects");
}
} // namespace

int main() {
  contracts();
  outputFormats();
  countAndUnsupportedFormats();
  indirectFormats();
  recognitionAndReplacement();
  countStoreAliases();
  variadicLists();
  unsupportedListABI();
}
