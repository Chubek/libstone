-- Exotract contract for the attached MuLIB MicroJS single-header C11 library.
-- Declaration-only discovery is performed WITHOUT MULIB_IMPL_* enabled.
-- The implementation switches are for an independent generated TU only.
-- No runtime ownership facts are marked reviewed by this file.
local exotract = require("exotract")
local compiler = exotract.global.get("C_COMPILER") or "cc"

local EXOLIB = exotract.exolib.new("MicroJS", {
  path = "$GLOBAL{MULIB_KERNEL_DIR}/mulib-microjs.h",
  summary = "Embeddable garbage-collected JavaScript core backed by MiniVM",
  documentation = "$GLOBAL{MULIB_DOC_DIR}/mulib-microjs.mudox",
  cstd = "C11",
  compiler = compiler,
  cache = true, -- requires 'exotract lua ... --cache-dir <LMDB_DIR>'
  exoml_bind = "$GLOBAL{EXOML_BIND}", -- see integration instructions
  clang_resource_dir = "$GLOBAL{CLANG_RESOURCE_DIR}",
  revision = "$GLOBAL{MULIB_REVISION}",
  provenance_files = { "$GLOBAL{MULIB_MANIFEST}" },
  includes = { "$GLOBAL{MULIB_KERNEL_DIR}" },
  header_only = {
    style = "implementation-macro",
    declaration_guard = "MULIB_MICROJS_H",
    implementation_guard = "MULIB_MICROJS_IMPLEMENTED",
    implementation = {
      defines = { "MULIB_IMPL_MICROJS", "MULIB_IMPL_MINIVM" },
      translation_units = 1
    },
    dependencies = { "mulib-minivm.h" }
  },
  preprocessor = {
    main = exotract.preprocessor.BUILTIN,
    fallback = { exotract.command.exec({compiler,"-E","-std=c11","%file"}) }
  },
  parser = {
    main = exotract.parser.BUILTIN
  }
})

local AST = EXOLIB:Preprocess():Parse()
AST:Cache()
-- Preserve unsafe flags until an ExoML-reviewed semantic contract authorizes
-- individual exports. The default On1x output is a typed metadata manifest,
-- NOT executable On1x declarations guessed from C strings.
local Binder = AST:Bind({
  engine = exotract.exobind.ON1X,
  safe_exports = "unsafe_explicit",
  script = "$GLOBAL{EXOTRACT_ROOT}/binders/on1x.lua"
})
return Binder.Internals
