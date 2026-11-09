# WPGKit

**Whole-Program Graph Toolkit** is a reusable C/C++ program-analysis library
and command-line tool. It constructs call graphs with incremental points-to
analysis, owned CFGs, an interprocedural CFG, and a data-flow graph.

## Build all deliverables

Requirements: CMake 3.24+, a C++20 compiler, and matching **LLVM/Clang 18.1.3**
development headers and libraries, including `clang-cpp`. Linux is the tested
platform. Other Clang versions require a compatibility update and validation.

```sh
mkdir -p .work-tmp/tmp
export TMPDIR="$PWD/.work-tmp/tmp" TMP="$PWD/.work-tmp/tmp" TEMP="$PWD/.work-tmp/tmp"
cmake -S . -B .work-tmp/build -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/path/to/llvm-18/lib/cmake/llvm
cmake --build .work-tmp/build -j 2
ctest --test-dir .work-tmp/build --output-on-failure -j 2
```

If Clang headers are installed separately, also pass
`-DWPGKIT_CLANG_INCLUDE_DIR=/path/to/clang/include`. The headers and libraries
must match the pinned version. No dependencies are downloaded during the build.

One build produces all three requested forms, using the same compiled PIC
objects for both libraries:

| Artifact | Purpose |
|---|---|
| `libwpgkit.a` | Static WPGKit library |
| `libwpgkit.so` | Shared WPGKit library, with versioned SONAME |
| `wpgkit-analyze` | Program analysis with JSON output |
| `wpgkit-link-inventory` | Companion GNU linker-map inventory tool |

`BUILD_TESTING=OFF` omits tests. Static WPGKit linkage still requires LLVM,
Clang, and thread-library dependencies; it does not imply a fully static
executable. The public C++ API uses the `wpgkit` namespace and
`<Header.h>` includes.

## Run the small example

```sh
cmake -S examples/program -B .work-tmp/example \
  -DCMAKE_CXX_COMPILER=/path/to/llvm-18/bin/clang++
cmake --build .work-tmp/example
.work-tmp/build/wpgkit-analyze --profile .work-tmp/example entry \
  examples/program/main.cpp > .work-tmp/example/report.json
```

The selected `entry` reaches three functions and two call sites, including
an indirect callback. The report includes CG edges, CFG/inter-CFG and DFG
statistics, unresolved effects, and profiling. The small example runs in
seconds; large programs depend strongly on parsing and graph size.

Command forms:

```text
wpgkit-analyze [--profile] COMPDB_DIR QUALIFIED_ROOT SOURCE...
wpgkit-analyze [--profile] --link-map COMPDB_DIR LINK_MAP QUALIFIED_ROOT
wpgkit-analyze [--profile] --link-maps COMPDB_DIR QUALIFIED_ROOT LINK_MAP...
wpgkit-link-inventory COMPDB_DIR LINK_MAP...
```

Inputs are exact compilation commands and explicit source files or objects
selected through GNU linker maps. Analysis does not compile or link the input
program; its compilation database and any linker maps must already exist.

## Install and use from another project

```sh
cmake --install .work-tmp/build --prefix "$PWD/.work-tmp/install"
cmake -S examples/client -B .work-tmp/client \
  -DCMAKE_PREFIX_PATH="$PWD/.work-tmp/install" \
  -DLLVM_DIR=/path/to/llvm-18/lib/cmake/llvm
cmake --build .work-tmp/client
.work-tmp/client/client-static .work-tmp/example examples/program/main.cpp
.work-tmp/client/client-shared .work-tmp/example examples/program/main.cpp
```

Use the Clang header-path override here as well if your installation needs it.
The installed package supplies:

```cmake
project(MyAnalysis LANGUAGES C CXX)
find_package(WPGKit 0.1 CONFIG REQUIRED)
add_executable(my-analysis main.cpp)
target_link_libraries(my-analysis PRIVATE WPGKit::Static)
# WPGKit::Shared selects the shared library.
# WPGKit::Analysis selects the shared library by default.
```

The [external client](examples/client/main.cpp) runs analysis and traverses
results after AST release. The installation regression copies the package to
a different prefix, builds clients against all three CMake targets, and runs
the relocated CLI. `add_subdirectory` also exposes these targets without an
installation.

## Analysis and ownership

1. Parse each selected compilation command once.
2. Capture owned CFG topology and shared def/use and CG/PTA facts.
3. Release the native AST and CFG before parsing the next unit.
4. Incrementally extend the call graph and inter-CFG and propagate points-to
   facts until the joint fixed point converges.
5. Construct the DFG from shared facts, call bindings, and memory partitions.

Each function retains one representative CFG and fact set. Results own their
metadata and share immutable graph storage; they survive extraction-session
and AST destruction. Graph attributes use numeric IDs, enums, and flags.

`CallGraphPTA::runFiles` and `runLinkMaps` provide complete input-to-result
entry points. `CallGraphSession` supports explicit AST imports and incremental
solving. `finalize` applies remaining conservative external effects and closes
imports. The lower-level `Andersen` component also exposes points-to queries;
the high-level result currently exposes graphs and PTA statistics, rather than
a complete variable-to-points-to query interface.

`AnalysisOptions::fieldSensitive` is **false by default**. In that mode,
fields, nested fields and array elements share their base object's abstract
storage; different base objects remain distinct. Enable it before extraction
when a client needs separate field and constant-index identities:

```cpp
wpgkit::AnalysisOptions options{.fieldSensitive = true};
wpgkit::CallGraphSession session(nullptr, {}, {}, options);
// Or pass options as the last argument of run/runFiles/runLinkMap/runLinkMaps.
```

The options are copied at session creation and cannot change an active analysis.
Both modes use the same incremental solver. The standalone CLI uses the default.
The low-level `Andersen` constructor accepts the same options.

A read-only extraction observer lets a downstream analysis collect its own
owned facts at existing CFG/DFG identities during the same parse. Consumers
must not retain native AST references. The [public session API](include/CallGraphPTA.h)
documents result ownership and observer lifetimes.

External-function models are refinements. The unified query returns a reviewed
summary or a conservative fallback. An empty catalog remains usable. Runtime
API names use hash lookup without a recognition cache. Distinct targets sharing
one model do not repeatedly install the same effects at an indirect call site.

`--profile` reports AST parsing, CFG/fact extraction, CG/PTA/inter-CFG, DFG, and
AST-release time separately, with process RSS measurements. Parent scopes
include their children; do not sum the parent and child times. RSS is
process-wide and sampling can miss brief peaks. No analyzer memory cap is imposed.

## Benchmark results

The following measurements were recorded on 5 November 2026 using a Release
build of the CLI and shared library, LLVM/Clang 18.1.3, and `--profile`. Each
benchmark ran once on Linux, pinned to one CPU core of an Intel Core Ultra 7
265K. No memory or time cap was imposed, and no process swap was observed.

Inputs came from IREE revision `1b60b62ed4b3f2a3c95f42b397889444c7df318c`
and its bundled LLVM revision `c6ff6d78618a5709d518d5f7f2a59ec62273fd10`.
Compilation commands and linker maps selected inputs for `iree-flatcc-cli`,
`mlir-tblgen`, and `iree-compile` with `libIREECompiler`, respectively.

| Measurement | FlatCC | MLIR TableGen | IREE |
|---|---:|---:|---:|
| Translation units | 26 | 109 | 1,620 |
| Unique input files (sources + included files) | 199 | 774 | 4,400 |
| Input SLOC (sources + included files) | 34,678 | 215,520 | 2,101,112 |
| Captured function CFGs, including unreached bodies | 7,939 | 55,436 | 1,853,933 |
| Reached function bodies in the inter-CFG | 646 | 9,155 | 772,039 |
| **Total wall time** | **0.709 s** | **47.25 s** | **88.01 min** |
| **Peak process RSS** | **122.65 MiB** | **528.39 MiB** | **19.70 GiB** |
| AST loading | 0.328 s | 37.57 s | 50.66 min |
| CFG/fact extraction | 0.219 s | 8.15 s | 10.84 min |
| CG/PTA + inter-CFG and result construction | 0.007 s | 0.135 s | 24.76 min |
| DFG construction | 0.014 s | 0.131 s | 10.84 s |
| AST release | 0.009 s | 0.724 s | 70.30 s |
| Unresolved call sites | 110 | 707 | 31,386 |

Input SLOC counts nonblank, noncomment lines in the union of files from Ninja
dependency records for the exact selected compilation outputs. All selected
outputs have dependency records. Each canonical file path is counted once with
`cloc` 1.98's C++ filter, including headers, generated files, and system headers.
This measures whole-file source before preprocessing; it includes inactive
conditional branches and does not multiply lines for template instantiations.

| Input SLOC breakdown | FlatCC | MLIR TableGen | IREE |
|---|---:|---:|---:|
| Selected translation-unit source files | 13,651 | 49,357 | 577,393 |
| Repository headers and other included files | 11,270 | 41,471 | 199,925 |
| Generated included files | 0 | 750 | 1,194,143 |
| System headers and included files | 9,757 | 123,942 | 129,651 |
| **Total** | **34,678** | **215,520** | **2,101,112** |

These categories are disjoint; generated translation-unit sources belong to
the first row. CFGs count distinct function identities, including header
functions and instantiated specializations. Captured CFGs also include bodies
not reached from the selected roots; only reached bodies enter the inter-CFG.
The saved reports do not provide CFG counts by definition file or template
kind, so they cannot quantify each category's contribution to the CFG total.

Total time also includes input discovery and reporting; it excludes building
the benchmark programs. Peak RSS covers the entire analyzer process. All
selected units were parsed once and their ASTs released, with no failed CFG
lifts. Unresolved calls and incomplete effects remain explicit in the results;
these measurements do not establish complete linked-program coverage. Each
entry is a single measurement, not an average over repeated trials.

## Scope and limitations

- Roots determine which captured functions participate in the analysis. A
  compilation database alone does not prove linked-program closure.
- Missing bodies, unknown indirect targets, unsupported effects, shared
  libraries, and unmapped linked objects remain explicit in results.
- CFG-derived local pointer definitions distinguish supported nonescaping
  locals. Aliasable memory and DFG memory partitions retain conservative,
  flow-insensitive merging; calling contexts are merged by the solver.
- With field sensitivity enabled, supported projections use numeric base-object
  and field identities.
  Constant indices of direct arrays have separate subobjects. Unknown indices,
  unsupported address expressions, and projections deeper than eight levels
  remain conservative. These bounds do not imply complete C++ alias modeling.
- In that mode DFG `Location` edges preserve singleton storage identities
  alongside coarse dependency partitions. Field-insensitive graphs retain
  `Memory` dependencies but omit `Location` edges, so collapsed fields cannot
  be mistaken for distinct update locations. A singleton abstract location alone does not justify
  a strong update: clients must also establish concrete-object uniqueness.
  Trivial record copies and named lambda captures reuse these shared facts.
- Inter-CFG return connections carry matching IDs. A consumer that needs
  context-sensitive traversal must balance these IDs. Exception/unwinding
  connections are approximate.
- The DFG represents possible data dependencies, not control dependence.
  DFG gaps must be considered by downstream analyses.
- External models do not cover all runtime behavior, callbacks, or C++
  constructs. `closedWithinInputs()` is not a proof of full C++ soundness or
  linked-program closure.

## Tests

The component suite covers pointer propagation, incremental CG construction,
CFG/inter-CFG and DFG ownership, external models and empty-catalog fallback,
compilation commands, linker maps, profiling, and installed clients. Optional
pinned IREE regression scripts are under `tests/integration`; large compiler
benchmarks are not run by the default suite.

License: Apache-2.0. Existing source attribution is retained; see [NOTICE](NOTICE).
