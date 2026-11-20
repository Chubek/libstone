set(DOMVIRT_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DOMVIRT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DOMVIRT_CODEGEN_SCRIPT "${PROJECT_SOURCE_DIR}/tools/codegen/generate_bindlets.py")
set(DOMVIRT_BINDLET_STUBS_DIR "${PROJECT_SOURCE_DIR}/domainlet/bindlets")

file(MAKE_DIRECTORY "${DOMVIRT_GEN_DIR}/bindlet_bridge")
add_custom_command(OUTPUT "${DOMVIRT_GEN_DIR}/machine_sources.hpp"
    BYPRODUCTS "${DOMVIRT_GEN_DIR}/machines/x86_64.aml" "${DOMVIRT_GEN_DIR}/machines/aarch64.aml" "${DOMVIRT_GEN_DIR}/machines/riscv64.aml"
    COMMAND ${Python_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/archml_bundle.py"
        "${PROJECT_SOURCE_DIR}" "${DOMVIRT_GEN_DIR}/machine_sources.hpp"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/archml_bundle.py"
        "${PROJECT_SOURCE_DIR}/machines/x86_64.aml" "${PROJECT_SOURCE_DIR}/machines/aarch64.aml"
        "${PROJECT_SOURCE_DIR}/machines/riscv64.aml" "${ARCHML_DIR}/stdspec/riscv64.aml"
        "${ARCHML_DIR}/stdspec/amd64.aml" "${ARCHML_DIR}/stdspec/arm64.aml"
    VERBATIM)

set(BINDLET_STUB_FILES)
file(GLOB_RECURSE BINDLET_STUB_FILES
    "${DOMVIRT_BINDLET_STUBS_DIR}/*.py"
    "${DOMVIRT_BINDLET_STUBS_DIR}/*.pyi"
)

set(CODEGEN_OUTPUTS
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_core.hpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_core.cpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_boot.hpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_boot.cpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_interrupts.hpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_interrupts.cpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_io.hpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_io.cpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_virtio.hpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_virtio.cpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_control.hpp"
    "${DOMVIRT_GEN_DIR}/bindlet_bridge/bindlet_vmm_control.cpp"
)

add_custom_command(
    OUTPUT ${CODEGEN_OUTPUTS}
    COMMAND ${Python_EXECUTABLE} "${DOMVIRT_CODEGEN_SCRIPT}"
        --stubs-dir "${DOMVIRT_BINDLET_STUBS_DIR}"
        --output-dir "${DOMVIRT_GEN_DIR}/bindlet_bridge"
    DEPENDS ${BINDLET_STUB_FILES} "${DOMVIRT_CODEGEN_SCRIPT}"
    COMMENT "Generating bindlet bridge C++ wrappers from Python stubs"
    VERBATIM
)

add_custom_target(bindlet_codegen DEPENDS ${CODEGEN_OUTPUTS})

set(HYPERVISOR_SOURCES
    ${DOMVIRT_SRC_DIR}/hypervisor/kvm_driver.cpp
    ${DOMVIRT_SRC_DIR}/hypervisor/memory_manager.cpp
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    list(APPEND HYPERVISOR_SOURCES
        ${DOMVIRT_SRC_DIR}/hypervisor/kvm_vm.cpp
        ${DOMVIRT_SRC_DIR}/hypervisor/vcpu.cpp
        ${DOMVIRT_SRC_DIR}/hypervisor/snapshot.cpp)
endif()

set(VIRTUAL_SOURCES
    ${DOMVIRT_SRC_DIR}/virtual/riscv64_cpu.cpp
    ${DOMVIRT_SRC_DIR}/virtual/aarch64_cpu.cpp
    ${DOMVIRT_SRC_DIR}/virtual/virtual_cpu.cpp
    ${DOMVIRT_SRC_DIR}/virtual/virtual_backend.cpp
)

set(BINDLET_BRIDGE_SOURCES
    ${DOMVIRT_SRC_DIR}/bindlet_bridge/bridge.cpp
    ${DOMVIRT_SRC_DIR}/bindlet_bridge/shared_memory.cpp
    ${DOMVIRT_SRC_DIR}/bindlet_bridge/lazy_register.cpp
    ${DOMVIRT_SRC_DIR}/bindlet_bridge/dispatcher.cpp
    ${DOMVIRT_SRC_DIR}/bindlet_bridge/file_transfer.cpp
    ${DOMVIRT_SRC_DIR}/bindlet_bridge/control_socket.cpp
    ${CODEGEN_OUTPUTS}
)

set(VIRTIO_SOURCES
    ${DOMVIRT_SRC_DIR}/virtio/virtio_devices.cpp
    ${DOMVIRT_SRC_DIR}/virtio/virtqueue.cpp
    ${DOMVIRT_SRC_DIR}/virtio/virtio_net.cpp
    ${DOMVIRT_SRC_DIR}/virtio/virtio_blk.cpp
    ${DOMVIRT_SRC_DIR}/virtio/virtio_console.cpp
    ${DOMVIRT_SRC_DIR}/virtio/transport.cpp
)

set(BOOT_SOURCES
    ${DOMVIRT_SRC_DIR}/boot/loader.cpp
    ${DOMVIRT_SRC_DIR}/boot/acpi.cpp
    ${DOMVIRT_SRC_DIR}/boot/bootparams.cpp
)

set(INTERRUPTS_SOURCES
    ${DOMVIRT_SRC_DIR}/interrupts/msi_routing.cpp
    ${DOMVIRT_SRC_DIR}/interrupts/irqfd.cpp
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    list(APPEND INTERRUPTS_SOURCES ${DOMVIRT_SRC_DIR}/interrupts/irqchip.cpp)
endif()

set(CLI_SOURCES
    ${DOMVIRT_SRC_DIR}/cli/boot_display.cpp
    ${DOMVIRT_SRC_DIR}/cli/main.cpp
)

set(DOMVIRT_SHELL_SOURCES
    ${DOMVIRT_SRC_DIR}/shell/shell.cpp
)

set(DOMVIRT_EMBEDDED_SOURCES
    ${DOMVIRT_SRC_DIR}/boot/embedded_images.cpp
)

set(DOMVIRT_DISPLAY_SOURCES
    ${DOMVIRT_SRC_DIR}/cli/boot_display_render.cpp
    ${DOMVIRT_SRC_DIR}/cli/font_renderer.cpp
)

if(DOMWEAVE_EMBED_DOMVIRT)
    find_package(Python 3.8 REQUIRED COMPONENTS Interpreter)
    set(DOMVIRT_EMBED_DIR "${DOMVIRT_GEN_DIR}/embedded")
    set(DOMVIRT_EMBED_CPP "${DOMVIRT_EMBED_DIR}/embedded_images_data.cpp")
    add_custom_command(
        OUTPUT "${DOMVIRT_EMBED_CPP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DOMVIRT_EMBED_DIR}"
        COMMAND ${Python_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/embed_binary.py"
            --input "${PROJECT_BINARY_DIR}/domios/domios.img"
            --output "${DOMVIRT_EMBED_CPP}"
            --symbol domios_image
            --additional-input "${PROJECT_BINARY_DIR}/domix/x86_64/domix.img"
            --additional-symbol domix_image
            --additional-input "${PROJECT_BINARY_DIR}/domix/aarch64/domix.img"
            --additional-symbol domix_aarch64_image
            --additional-input "${PROJECT_BINARY_DIR}/domix/riscv64/domix.img"
            --additional-symbol domix_riscv64_image
        DEPENDS
            "${PROJECT_BINARY_DIR}/domios/domios.img"
            "${PROJECT_BINARY_DIR}/domix/x86_64/domix.img"
            "${PROJECT_BINARY_DIR}/domix/aarch64/domix.img"
            "${PROJECT_BINARY_DIR}/domix/riscv64/domix.img"
            "${PROJECT_SOURCE_DIR}/tools/embed_binary.py"
        VERBATIM
    )
    list(APPEND DOMVIRT_EMBEDDED_SOURCES "${DOMVIRT_EMBED_CPP}")
endif()

set(DOMVIRT_ALL_SOURCES
    ${DOMVIRT_GEN_DIR}/machine_sources.hpp
    ${DOMVIRT_SRC_DIR}/machine/profile.cpp
    ${DOMVIRT_SRC_DIR}/machine/runtime.cpp
    ${DOMVIRT_SRC_DIR}/machine/pc_devices.cpp
    ${DOMVIRT_SRC_DIR}/testing/test_session.cpp
    ${HYPERVISOR_SOURCES}
    ${VIRTUAL_SOURCES}
    ${BINDLET_BRIDGE_SOURCES}
    ${VIRTIO_SOURCES}
    ${BOOT_SOURCES}
    ${INTERRUPTS_SOURCES}
    ${DOMVIRT_DISPLAY_SOURCES}
    ${DOMVIRT_EMBEDDED_SOURCES}
)

add_library(domvirt_core STATIC ${DOMVIRT_ALL_SOURCES})
add_library(DOMWeave::domvirt_core ALIAS domvirt_core)

target_include_directories(domvirt_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${DOMVIRT_GEN_DIR}>
    PUBLIC
        ${Python_INCLUDE_DIRS}
    PRIVATE
        ${DOMVIRT_SRC_DIR}
        ${PROJECT_SOURCE_DIR}/domios/src
        ${LIBSTONE_DIR}/third_party/metatk/third_party/stb
)

target_link_libraries(domvirt_core
    PUBLIC
        nlohmann_json
        Sandstone::ArchML
        Threads::Threads
        DOMWeave::domios
        Zydis::Zydis
    PRIVATE
        Python::Python
)

target_compile_definitions(domvirt_core PRIVATE
    _GNU_SOURCE
    DOMVIRT_VERSION="${PROJECT_VERSION}"
    DOMVIRT_DEFAULT_DOMIOS_IMAGE="${PROJECT_BINARY_DIR}/domios/domios.img"
    ARCHVIRT_PC_BIOS="${PROJECT_BINARY_DIR}/bios/archvirt-bios.bin"
    DOMVIRT_DEFAULT_DOMIX_KERNEL="${PROJECT_BINARY_DIR}/domix/kernel"
    DOMVIRT_DEFAULT_DOMIX_IMAGE="${PROJECT_BINARY_DIR}/domix/domix.img"
    DOMVIRT_DOMIX_DIR="${PROJECT_BINARY_DIR}/domix"
    DOMVIRT_FONT_PATH="${IMGUI_DIR}/misc/fonts/Cousine-Regular.ttf"
)
if(DOMWEAVE_EMBED_DOMVIRT)
    target_compile_definitions(domvirt_core PRIVATE DOMVIRT_EMBED_IMAGES=1)
endif()
add_dependencies(domvirt_core domios_image domix-image pc_bios)

domvirt_set_compiler_flags(domvirt_core)
domvirt_set_release_flags(domvirt_core)

add_library(domvirt SHARED)
add_library(DOMWeave::domvirt ALIAS domvirt)

target_sources(domvirt PRIVATE
    ${DOMVIRT_SRC_DIR}/domvirt.cpp
)

target_link_libraries(domvirt
    PUBLIC
        domvirt_core
        DOMWeave::domios
)
target_include_directories(domvirt PRIVATE ${DOMVIRT_SRC_DIR} ${Python_INCLUDE_DIRS})
target_compile_definitions(domvirt PRIVATE
    DOMVIRT_VERSION="${PROJECT_VERSION}"
    DOMVIRT_DEFAULT_DOMIOS_IMAGE="${PROJECT_BINARY_DIR}/domios/domios.img"
    DOMVIRT_DEFAULT_DOMIX_KERNEL="${PROJECT_BINARY_DIR}/domix/kernel"
    DOMVIRT_DEFAULT_DOMIX_IMAGE="${PROJECT_BINARY_DIR}/domix/domix.img"
)

domvirt_set_compiler_flags(domvirt)
domvirt_set_release_flags(domvirt)

add_executable(domvirt_cli ${CLI_SOURCES})
target_link_libraries(domvirt_cli PRIVATE domvirt_core DOMWeave::domios)
target_link_libraries(domvirt_cli PRIVATE archvirt_imgui)
target_include_directories(domvirt_cli PRIVATE
    ${DOMVIRT_SRC_DIR}
    ${DOMVIRT_THIRD_PARTY_DIR}/stb
    ${PROJECT_SOURCE_DIR}/domios/src
)
target_compile_definitions(domvirt_cli PRIVATE
    DOMVIRT_VERSION="${PROJECT_VERSION}"
    DOMVIRT_DEFAULT_DOMIOS_IMAGE="${PROJECT_BINARY_DIR}/domios/domios.img"
    DOMVIRT_DEFAULT_DOMIX_KERNEL="${PROJECT_BINARY_DIR}/domix/kernel"
    DOMVIRT_DEFAULT_DOMIX_IMAGE="${PROJECT_BINARY_DIR}/domix/domix.img"
    DOMVIRT_FONT_PATH="${IMGUI_DIR}/misc/fonts/Cousine-Regular.ttf"
)
set_target_properties(domvirt_cli PROPERTIES
    OUTPUT_NAME "archvirt"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_custom_command(TARGET domvirt_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/domvirt"
    COMMAND ${CMAKE_COMMAND} -E create_symlink ../archvirt "${CMAKE_BINARY_DIR}/domvirt/domvirt")
add_custom_target(archvirt DEPENDS domvirt_cli)
domvirt_set_compiler_flags(domvirt_cli)
domvirt_set_release_flags(domvirt_cli)

add_executable(domvirt_shell ${DOMVIRT_SHELL_SOURCES})
target_link_libraries(domvirt_shell PRIVATE domvirt_core DOMWeave::domios readline)
target_include_directories(domvirt_shell PRIVATE ${DOMVIRT_SRC_DIR})
target_compile_definitions(domvirt_shell PRIVATE DOMVIRT_VERSION="${PROJECT_VERSION}")
set_target_properties(domvirt_shell PROPERTIES
    OUTPUT_NAME "domvirt-shell"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/domvirt"
)
domvirt_set_compiler_flags(domvirt_shell)
domvirt_set_release_flags(domvirt_shell)
