-- Requires built ExoML extractor. Exotract fails closed without typed CDecl.
local exotract = require("exotract")
local lib = exotract.exolib.new("Fixture", {
  path = "$GLOBAL{EXOTRACT_ROOT}/tests/fixture.h",
  summary = "C11 single-header fixture",
  cache = false,
  cstd = "C11",
  exoml_bind = "$GLOBAL{EXOML_BIND}",
  clang_resource_dir = "$GLOBAL{CLANG_RESOURCE_DIR}",
  header_only = {
    style = "implementation-macro",
    declaration_guard = "EXOTRACT_FIXTURE_H",
    implementation_guard = "EXOTRACT_FIXTURE_IMPLEMENTED",
    implementation = {
      defines = { "EXOTRACT_IMPL_FIXTURE" },
      translation_units = 1
    }
  },
  preprocessor = { main = exotract.preprocessor.BUILTIN },
  parser = { main = exotract.parser.BUILTIN }
})
local ast = lib:Preprocess():Parse()
return ast:Bind({safe_exports="unsafe_explicit"}).Internals
