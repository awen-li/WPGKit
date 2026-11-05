# WPGKit development

Use C++20, small concrete classes, explicit ownership, and composition. Keep
functions focused and at most 100 formatted lines. Follow `.clang-format`.

The pipeline is AST -> owned CFG and CG/PTA facts -> AST release -> incremental
CG/inter-CFG/PTA -> DFG. Parse each selected command once. Keep graph attributes
numeric and display text in owned metadata. Do not introduce a generic IR,
repeat the local def/use computation, or restart the solver between updates.

Keep external API recognition and effects in `src/external/`. Missing models
and an empty catalog must retain conservative effects. Use the existing hash
index for runtime names; do not add a recognition cache or a model-selection
command-line option. Do not impose memory caps.

Every normal build produces static and shared libraries and the CLI from one
set of PIC objects. Preserve installed CMake targets and test both library
forms through the external-client installation regression. Retain provenance
and license notices. Test a component before building new capabilities on it.
Do not run full linked-IREE benchmarks for routine changes.

Keep all build outputs, logs, scratch files, and downloaded dependencies under
`.work-tmp/`, which stays ignored. Create `.work-tmp/tmp` and set TMPDIR, TMP,
and TEMP to its absolute path for tools that create temporary files.
