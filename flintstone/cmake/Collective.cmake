# Keep the component libraries in their own packages. The aliases only
# collect their usage requirements and can be used without the whole SDK.
set(FLINTSTONE_SDK_TOURCHSTONE_NATIVE OFF)
add_library(flintstone_sdk INTERFACE)
add_library(Flintstone::sdk ALIAS flintstone_sdk)
set_target_properties(flintstone_sdk PROPERTIES EXPORT_NAME sdk)
target_link_libraries(flintstone_sdk INTERFACE flintstone_native flintstone_codegen)

function(flintstone_sdk_component name)
  add_library(flintstone_sdk_${name} INTERFACE)
  add_library(Flintstone::${name} ALIAS flintstone_sdk_${name})
  set_target_properties(flintstone_sdk_${name} PROPERTIES EXPORT_NAME ${name})
  foreach(lib IN LISTS ARGN)
    target_link_libraries(flintstone_sdk_${name} INTERFACE
      $<BUILD_INTERFACE:${lib}> $<INSTALL_INTERFACE:${lib}>)
  endforeach()
  target_link_libraries(flintstone_sdk INTERFACE flintstone_sdk_${name})
  set(FLINTSTONE_SDK_COMPONENTS "${FLINTSTONE_SDK_COMPONENTS};${name}" PARENT_SCOPE)
  install(TARGETS flintstone_sdk_${name} EXPORT Flintstone-${name}Targets)
  install(EXPORT Flintstone-${name}Targets NAMESPACE Flintstone::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Flintstone)
endfunction()

flintstone_sdk_component(firestone Firestone::firestone)
flintstone_sdk_component(blackstone Blackstone::blackstone)
set(tourchstone_libraries XBind::xbind XBind::QaMRppSpec)
foreach(lib IN ITEMS Tourchstone::dynload Tourchstone::dyninvoke)
  if(TARGET ${lib})
    list(APPEND tourchstone_libraries ${lib})
  endif()
endforeach()
if(TARGET Tourchstone::dynload OR TARGET Tourchstone::dyninvoke)
  set(FLINTSTONE_SDK_TOURCHSTONE_NATIVE ON)
endif()
flintstone_sdk_component(tourchstone ${tourchstone_libraries})
if(TARGET Sandstone::sandstone)
  set(libraries Sandstone::sandstone)
  foreach(lib IN ITEMS Sandstone::ArchML Sandstone::assembler Sandstone::spec_core Sandstone::sandslab)
    if(TARGET ${lib})
      list(APPEND libraries ${lib})
    endif()
  endforeach()
  flintstone_sdk_component(assembly ${libraries})
endif()
if(TARGET Limestone::core)
  flintstone_sdk_component(limestone Limestone::core Limestone::vmweave Limestone::exolayer
    Limestone::limeburg_text Limestone::schedrow_text Limestone::regtl_text
    Limestone::limeburg_infobank Limestone::tunah_unisel Limestone::tunah_bin2bin
    Limestone::tunah_stages Limestone::traceml_api Limestone::machineir_native)
endif()
if(TARGET qobjld::qobjld)
  flintstone_sdk_component(yellowstone qobjld::qobjld ArchBFD::cpp)
endif()
if(TARGET Novostone::ActionSemantics)
  flintstone_sdk_component(novostone Novostone::Datalog Novostone::CCDB Novostone::DatabaseBasedIR
    Novostone::QueryBasedIR Novostone::SpeculativeIR Novostone::CostCC Novostone::ActionSemantics)
endif()
if(TARGET Moonstone::Core)
  set(libraries Moonstone::Core)
  foreach(lib IN ITEMS Moonstone::Chisel Moonstone::Syren Moonstone::Supersonic Moonstone::ExoML
      Moonstone::ExoMLExtract Moonstone::Exotract Moonstone::ExotractLua Moonstone::Plaid
      Moonstone::PlaidRuntime Moonstone::Prolific Moonstone::ProlificLua Moonstone::Qmr
      Moonstone::QmrCompiler Nerotypical::Sequent)
    if(TARGET ${lib})
      list(APPEND libraries ${lib})
    endif()
  endforeach()
  flintstone_sdk_component(moonstone ${libraries})
endif()
if(TARGET Wetstone::wetstone)
  flintstone_sdk_component(wetstone Wetstone::wetstone Flintstone::flintstone)
endif()
if(TARGET Weighstone::Weighstone)
  flintstone_sdk_component(weighstone Weighstone::Weighstone)
endif()

install(TARGETS flintstone_sdk EXPORT FlintstoneSDKTargets)
install(EXPORT FlintstoneSDKTargets NAMESPACE Flintstone::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Flintstone)
