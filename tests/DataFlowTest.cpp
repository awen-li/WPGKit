#include "CallGraphPTA.h"
#include "DataFlowGraph.h"
#include "InterCFG.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace wpgkit;

namespace {
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void add(CallGraphSession &session, const char *code) {
  auto ast = clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c++20"},
                                                      "data.cpp");
  check(ast && !ast->getDiagnostics().hasErrorOccurred(),
        "parse data-flow fixture");
  session.addUnit(ast->getASTContext());
}

CallGraphResult fixture(const char *code, AnalysisOptions options = {}) {
  CallGraphSession session(nullptr, {}, {}, options);
  add(session,
      code); // AST destroyed before either fixed point or DFG construction.
  auto result = session.finalize({"entry"});
  if (!result) {
    std::cerr << llvm::toString(result.takeError());
    std::exit(1);
  }
  auto again = session.finalize({"entry"});
  check(bool(again) && again->dataFlow == result->dataFlow,
        "unchanged solve shares the DFG");
  check(result->dataFlow != nullptr, "result owns DFG");
  return std::move(*result); // Session destroyed before graph queries.
}

std::vector<DataNodeId> nodes(const CallGraphResult &program, DataNodeKind kind,
                              const char *function = "entry") {
  std::vector<DataNodeId> result;
  const auto &graph = *program.dataFlow;
  for (std::size_t i = 0; i < graph.nodes().size(); ++i) {
    const auto &node = graph.nodes()[i];
    const auto *cfg = program.cfg(node.position.body);
    if (node.kind == kind && cfg &&
        program.metadata.text(program.names.at(cfg->function)) == function)
      result.push_back(DataNodeId{i + 1});
  }
  return result;
}

bool path(const DataFlowGraph &graph, DataNodeId source, DataNodeId target) {
  std::set<DataNodeId> seen;
  std::vector<DataNodeId> work{source};
  while (!work.empty()) {
    const auto current = work.back();
    work.pop_back();
    if (current == target)
      return true;
    if (!seen.insert(current).second)
      continue;
    for (const auto &edge : graph.outgoing(current))
      work.push_back(edge.target);
  }
  return false;
}

void localFlow() {
  const auto overwrite =
      fixture("int entry(int a,int b){int x=a; x=b; return x;}");
  const auto params = nodes(overwrite, DataNodeKind::Parameter);
  const auto returned = nodes(overwrite, DataNodeKind::Return).at(0);
  check(!path(*overwrite.dataFlow, params.at(0), returned) &&
            path(*overwrite.dataFlow, params.at(1), returned),
        "scalar overwrite kills the old definition");
  check(overwrite.dataFlow->gaps().empty(),
        "supported scalar overwrite has no extraction gaps");
  const auto branch =
      fixture("int entry(int a,int b,bool c){int x=a; if(c) x=b; return x;}");
  const auto inputs = nodes(branch, DataNodeKind::Parameter);
  const auto output = nodes(branch, DataNodeKind::Return).at(0);
  check(path(*branch.dataFlow, inputs.at(0), output) &&
            path(*branch.dataFlow, inputs.at(1), output),
        "both branch definitions reach the result");
  const auto loop = fixture(
      "int entry(int a,int b,bool c){int x=a; while(c){x=x+b;} return x;}");
  const auto loopInputs = nodes(loop, DataNodeKind::Parameter);
  const auto loopOutput = nodes(loop, DataNodeKind::Return).at(0);
  check(path(*loop.dataFlow, loopInputs.at(0), loopOutput) &&
            path(*loop.dataFlow, loopInputs.at(1), loopOutput),
        "loop joins retain entry and backedge dependencies");
  const auto undefined =
      fixture("int entry(bool c){int x; if(c) x=1; return x;}");
  check(std::any_of(undefined.dataFlow->gaps().begin(),
                    undefined.dataFlow->gaps().end(),
                    [](const auto &gap) {
                      return gap.kind == DataGapKind::UninitializedRead;
                    }),
        "a missing local definition stays unknown");
  const auto unevaluated = fixture(
      "int entry(){int x; using T=decltype(x+1);"
      "return sizeof(T) + sizeof(x+1) + noexcept(x+1) + __builtin_constant_p(x+1);}");
  check(std::none_of(unevaluated.dataFlow->gaps().begin(),
                     unevaluated.dataFlow->gaps().end(),
                     [](const auto &gap) {
                       return gap.kind == DataGapKind::UninitializedRead;
                     }),
        "unevaluated operands do not introduce uninitialized reads");
}

void memoryFlow() {
  const auto alias =
      fixture("int entry(int a,int b){int x=a; int *p=&x; *p=b; return x;}");
  const auto params = nodes(alias, DataNodeKind::Parameter);
  const auto returned = nodes(alias, DataNodeKind::Return).at(0);
  check(
      path(*alias.dataFlow, params.at(0), returned) &&
          path(*alias.dataFlow, params.at(1), returned),
      "aliasable storage conservatively retains initializer and pointer store");
  const auto separate = fixture(
      "int entry(int a,int b){int x=a; int y=b; int *p=&x; int *q=&y; return *p;}");
  const auto separateParams = nodes(separate, DataNodeKind::Parameter);
  const auto separateReturn = nodes(separate, DataNodeKind::Return).at(0);
  check(path(*separate.dataFlow, separateParams.at(0), separateReturn) &&
            !path(*separate.dataFlow, separateParams.at(1), separateReturn),
        "disjoint known pointees do not share a memory dependency");
  const auto reference =
      fixture("int entry(int a,int b){int x=a; int &r=x; r=b; return x;}");
  check(path(*reference.dataFlow,
             nodes(reference, DataNodeKind::Parameter).at(1),
             nodes(reference, DataNodeKind::Return).at(0)),
        "reference writes use the existing PTA referent");
  const auto readOnly =
      fixture("int entry(int a,int b,int *p){int x=a; int y=b;"
              "int *q=&x; int *r=&y; int unused=*p; return x;}");
  check(!path(*readOnly.dataFlow,
              nodes(readOnly, DataNodeKind::Parameter).at(1),
              nodes(readOnly, DataNodeKind::Return).at(0)),
        "unknown reads alone do not merge disjoint writes");
  const auto global =
      fixture("int initialize(); int g=initialize(); int entry(){return g;}");
  check(global.unknownMemoryEffects && !global.dataFlow->gaps().empty(),
        "dynamic global initialization retains unknown memory effects");
}

void scalarUpdates() {
  const auto program = fixture(
      "int entry(int a,int b){int x=a; int old=x++; x*=b; return x+old;}");
  const auto inputs = nodes(program, DataNodeKind::Parameter);
  const auto output = nodes(program, DataNodeKind::Return).at(0);
  check(program.dataFlow->gaps().empty() &&
            path(*program.dataFlow, inputs.at(0), output) &&
            path(*program.dataFlow, inputs.at(1), output),
        "scalar updates retain both read and write dependencies");
  const auto overwritten =
      fixture("int entry(int a,int b){int x=a; ++x; x=b; return x;}");
  const auto params = nodes(overwritten, DataNodeKind::Parameter);
  const auto result = nodes(overwritten, DataNodeKind::Return).at(0);
  check(!path(*overwritten.dataFlow, params.at(0), result) &&
            path(*overwritten.dataFlow, params.at(1), result),
        "overwriting an incremented scalar kills the prior definition");
}

void fieldIdentity() {
  const auto program = fixture(R"cpp(
    struct Pair { int first, second; };
    int entry(int a, int b) {
      Pair left{a, a};
      Pair right{b, b};
      Pair *alias = &left;
      return alias->first;
    }
  )cpp");
  const auto inputs = nodes(program, DataNodeKind::Parameter);
  const auto output = nodes(program, DataNodeKind::Return).at(0);
  check(
      path(*program.dataFlow, inputs.at(0), output) &&
          !path(*program.dataFlow, inputs.at(1), output),
      "field dependencies retain base object identity through pointer aliases");
  const auto code = R"cpp(
    struct Pair { int first, second; };
    int entry(int a, int b) { Pair pair{a, b}; return pair.first; }
  )cpp";
  for (const bool sensitive : {false, true}) {
    const auto fields = fixture(code, {.fieldSensitive = sensitive});
    const auto parameters = nodes(fields, DataNodeKind::Parameter);
    const auto returned = nodes(fields, DataNodeKind::Return).at(0);
    check(path(*fields.dataFlow, parameters.at(0), returned) &&
              path(*fields.dataFlow, parameters.at(1), returned) != sensitive,
          "field mode controls cross-field dependencies");
    check(std::any_of(fields.dataFlow->edges().begin(),
                      fields.dataFlow->edges().end(),
                      [](const auto &edge) {
                        return edge.kind == DataEdgeKind::Location;
                      }) == sensitive,
          "collapsed fields do not advertise distinct update locations");
  }
}

void calls() {
  const auto direct = fixture(
      "int choose(int a,int b){return b;} int entry(int a,int b){return choose(a,b);}");
  const auto params = nodes(direct, DataNodeKind::Parameter);
  const auto returned = nodes(direct, DataNodeKind::Return).at(0);
  check(!path(*direct.dataFlow, params.at(0), returned) &&
            path(*direct.dataFlow, params.at(1), returned),
        "actual/formal and return dependencies preserve the used argument");
  for (const auto &edge : direct.dataFlow->edges()) {
    check(direct.dataFlow->node(edge.source) &&
              direct.dataFlow->node(edge.target),
          "edges refer to owned nodes");
    if (edge.kind == DataEdgeKind::ActualFormal ||
        edge.kind == DataEdgeKind::Return)
      check(direct.interCFG->connection(edge.connection) != nullptr,
            "interprocedural edges reuse connection IDs");
    const auto incoming = direct.dataFlow->incoming(edge.target);
    check(std::find(incoming.begin(), incoming.end(), edge) != incoming.end(),
          "reverse queries agree with forward edges");
  }
  const auto indirect = fixture(
      "int id(int x){return x;} int entry(int x){auto p=id; return p(x);}");
  check(path(*indirect.dataFlow, nodes(indirect, DataNodeKind::Parameter).at(0),
             nodes(indirect, DataNodeKind::Return).at(0)),
        "PTA-discovered calls connect value flow");
  const auto pointerCall = fixture("int entry(int (*p)()){return p();}");
  check(path(*pointerCall.dataFlow,
             nodes(pointerCall, DataNodeKind::Parameter).at(0),
             nodes(pointerCall, DataNodeKind::Return).at(0)),
        "an indirect call consumes its callee pointer value");
  const auto recursion = fixture(
      "int f(int x){if(x) return f(x-1); return x;} int entry(int x){return f(x);}");
  check(path(*recursion.dataFlow,
             nodes(recursion, DataNodeKind::Parameter).at(0),
             nodes(recursion, DataNodeKind::Return).at(0)),
        "recursive calls form finite cyclic dependencies");
  const auto unknown = fixture(
      "int external(int*); int entry(int x){int y=x; external(&y); return y;}");
  check(std::any_of(unknown.dataFlow->gaps().begin(),
                    unknown.dataFlow->gaps().end(),
                    [](const auto &gap) {
                      return gap.kind == DataGapKind::UnknownCall;
                    }),
        "unknown calls remain explicit");
  check(path(*unknown.dataFlow, nodes(unknown, DataNodeKind::Parameter).at(0),
             nodes(unknown, DataNodeKind::Return).at(0)),
        "unknown effects preserve known input dependencies");
}

void fieldsAndModels() {
  const auto fields = fixture(
      "struct S{int x; S(int a):x(a){}}; int entry(int a){S s(a); return s.x;}");
  check(path(*fields.dataFlow, nodes(fields, DataNodeKind::Parameter).at(0),
             nodes(fields, DataNodeKind::Return).at(0)),
        "member initializers share the PTA field identity");
  const auto aggregate =
      fixture("struct S{int x;}; int entry(int a){S s{a}; return s.x;}");
  check(path(*aggregate.dataFlow,
             nodes(aggregate, DataNodeKind::Parameter).at(0),
             nodes(aggregate, DataNodeKind::Return).at(0)),
        "aggregate initializers retain scalar field definitions");
  const auto model = fixture(
      "namespace std {template<class T> T&& move(T &x){return static_cast<T&&>(x);}}"
      "int entry(int x){int y=std::move(x); return y;}");
  check(path(*model.dataFlow, nodes(model, DataNodeKind::Parameter).at(0),
             nodes(model, DataNodeKind::Return).at(0)),
        "external forwarding summary connects inputs to outputs");
  check(nodes(model, DataNodeKind::Parameter, "std::move").empty(),
        "modeled intrinsics do not also execute their saved body facts");
  const auto allocation = fixture("int *entry(int n){return new int[n];}");
  check(std::any_of(allocation.dataFlow->gaps().begin(),
                    allocation.dataFlow->gaps().end(),
                    [](const auto &gap) {
                      return gap.kind == DataGapKind::UnsupportedEffect &&
                             gap.site.isValid();
                    }),
        "unextracted implicit allocation arguments remain explicit unknowns");
}

const CallSiteResult &destructorCall(const CallGraphResult &program) {
  const auto found = std::find_if(
      program.calls.begin(), program.calls.end(), [&](const auto &call) {
        return std::any_of(
            call.targets.begin(), call.targets.end(), [&](auto target) {
              return program.metadata.text(program.names.at(target)) ==
                     "Guard::~Guard";
            });
      });
  check(found != program.calls.end(), "destructor has a CG/PTA call identity");
  return *found;
}

std::size_t checkDestructorBindings(const CallGraphResult &program) {
  const auto &call = destructorCall(program);
  check(!call.unresolved, "destructor is already resolved by CG/PTA");
  check(std::none_of(program.dataFlow->gaps().begin(),
                     program.dataFlow->gaps().end(),
                     [&](const auto &gap) {
                       return gap.site == call.id &&
                              (gap.kind == DataGapKind::UnmappedCall ||
                               gap.kind == DataGapKind::UnknownCall);
                     }),
        "resolved destructor has a DFG call record and binding");
  check(nodes(program, DataNodeKind::Parameter, "Guard::~Guard").empty(),
        "implicit receiver does not become an explicit parameter");
  std::set<CFGPosition> occurrences;
  for (std::size_t i = 0; i < program.interCFG->connections().size(); ++i) {
    const auto &connection = program.interCFG->connections()[i];
    if (connection.site != call.id)
      continue;
    occurrences.insert(connection.source);
    const auto bindings = std::count_if(
        program.dataFlow->edges().begin(), program.dataFlow->edges().end(),
        [&](const auto &edge) {
          if (edge.connection != CallConnectionId{i + 1} ||
              edge.kind != DataEdgeKind::ActualFormal)
            return false;
          const auto *source = program.dataFlow->node(edge.source);
          const auto *target = program.dataFlow->node(edge.target);
          return source->kind == DataNodeKind::Receiver &&
                 target->kind == DataNodeKind::Receiver &&
                 source->position.body == call.body &&
                 source->position.block.isValid() &&
                 target->position ==
                     *program.interCFG->entry(connection.callee);
        });
    check(bindings == 1,
          "each destructor occurrence has its own receiver binding");
  }
  check(!occurrences.empty(), "destructor is connected in the inter-CFG");
  return occurrences.size();
}

void implicitDestructor() {
  const auto simple =
      fixture("int global; struct Guard { ~Guard() { global = 1; } };"
              "void entry() { Guard guard; }");
  checkDestructorBindings(simple);
  check(simple.dataFlow->gaps().empty(),
        "simple internal destruction needs no unknown-effect fallback");
  const auto program = fixture(
      "int global; struct Guard { int value; ~Guard() { global = value; } };"
      "int entry(int input, int unrelated, bool early) {"
      "{ Guard guard{input}; if (early) return 0; } return global; }");
  check(
      checkDestructorBindings(program) >= 2,
      "early and normal exits reuse the call identity with distinct bindings");
  const auto params = nodes(program, DataNodeKind::Parameter);
  const auto outputs = nodes(program, DataNodeKind::Return);
  check(std::any_of(outputs.begin(), outputs.end(),
                    [&](auto result) {
                      return path(*program.dataFlow, params[0], result);
                    }),
        "destructor field read reaches its global write and the caller result");
  check(
      std::none_of(outputs.begin(), outputs.end(),
                   [&](auto result) {
                     return path(*program.dataFlow, params[1], result);
                   }),
      "unrelated inputs do not flow through an artificial unknown destructor");
}

void destructorExpressions() {
  const auto temporary = fixture(
      "int global; struct Guard { int value; Guard(int x):value(x){}"
      "~Guard(){global=value;} }; int entry(int x){Guard{x}; return global;}");
  checkDestructorBindings(temporary);
  check(path(*temporary.dataFlow,
             nodes(temporary, DataNodeKind::Parameter).at(0),
             nodes(temporary, DataNodeKind::Return).at(0)),
        "temporary destruction retains constructor-to-field-to-global flow");
  const auto explicitCall =
      fixture("int global; struct Guard { int value; ~Guard(){global=value;} };"
              "void entry(Guard *p){p->~Guard();}");
  checkDestructorBindings(explicitCall);
  check(
      path(*explicitCall.dataFlow,
           nodes(explicitCall, DataNodeKind::Parameter).at(0),
           nodes(explicitCall, DataNodeKind::Receiver, "Guard::~Guard").at(0)),
      "explicit destructor binds its pointer operand to this");
  const auto deleted =
      fixture("int global; struct Guard { int value; ~Guard(){global=value;} };"
              "void entry(Guard *p){delete p;}");
  checkDestructorBindings(deleted);
  check(path(*deleted.dataFlow, nodes(deleted, DataNodeKind::Parameter).at(0),
             nodes(deleted, DataNodeKind::Receiver, "Guard::~Guard").at(0)),
        "delete binds its object pointer separately from deallocation");
}

void unavailableDestructor() {
  const auto program =
      fixture("struct Guard { ~Guard(); }; void entry(){Guard guard;}");
  const auto &call = destructorCall(program);
  check(call.unresolved, "an unavailable destructor body remains unresolved");
  check(std::any_of(program.dataFlow->gaps().begin(),
                    program.dataFlow->gaps().end(),
                    [&](const auto &gap) {
                      return gap.site == call.id &&
                             gap.kind == DataGapKind::UnknownCall;
                    }),
        "DFG preserves CG/PTA's unknown-call status");
  check(std::none_of(program.dataFlow->gaps().begin(),
                     program.dataFlow->gaps().end(),
                     [&](const auto &gap) {
                       return gap.site == call.id &&
                              gap.kind == DataGapKind::UnmappedCall;
                     }),
        "missing body does not imply a missing destructor call record");
  check(std::any_of(program.dataFlow->edges().begin(),
                    program.dataFlow->edges().end(),
                    [&](const auto &edge) {
                      return edge.kind == DataEdgeKind::Summary &&
                             program.dataFlow->node(edge.source)->kind ==
                                 DataNodeKind::Receiver;
                    }),
        "unknown destructor effects retain the receiver input");
}

void snapshots() {
  CallGraphSession session;
  add(session, "int helper(int); int entry(int x){return helper(x);}");
  auto before = session.analyze({"entry"});
  check(bool(before) && !before->dataFlow->gaps().empty(),
        "missing body remains an explicit gap");
  const auto oldEdges = before->dataFlow->edges().size();
  add(session, "int helper(int x){return x;}");
  auto after = std::move(session).finalize({"entry"});
  check(bool(after) && after->dataFlow != before->dataFlow,
        "new call binding creates a new DFG snapshot");
  check(before->dataFlow->edges().size() == oldEdges &&
            !before->dataFlow->gaps().empty(),
        "new body import does not mutate the earlier DFG");
  check(
      after->dataFlow->gaps().empty() &&
          path(*after->dataFlow, nodes(*after, DataNodeKind::Parameter).at(0),
               nodes(*after, DataNodeKind::Return).at(0)),
      "cross-unit dependencies survive consuming the session and releasing extraction buffers");
}
} // namespace

int main() {
  fieldIdentity();
  scalarUpdates();
  localFlow();
  memoryFlow();
  calls();
  fieldsAndModels();
  implicitDestructor();
  destructorExpressions();
  unavailableDestructor();
  snapshots();
}
