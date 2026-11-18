# Shared by the build-tree and relocated consumers. Target tests guard optional
# libraries, never their actual execution checks.
function(flintstone_sdk_smoke name source)
  add_executable(${name} "${source}")
  target_link_libraries(${name} PRIVATE Flintstone::sdk)
  foreach(lib IN ITEMS Novostone::ActionSemantics Moonstone::ExoML Moonstone::PlaidRuntime
      Moonstone::Qmr Sandstone::ArchML Wetstone::wetstone Weighstone::Weighstone
      qobjld::qobjld Tourchstone::dynload Tourchstone::dyninvoke)
    if(TARGET ${lib})
      string(MAKE_C_IDENTIFIER "${lib}" id)
      target_compile_definitions(${name} PRIVATE "HAVE_${id}")
    endif()
  endforeach()
endfunction()
