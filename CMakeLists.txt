# bbkernel — RISC-V Linux kernel + rootfs builder
#
# Submodules (in bb-tests/thirdparty/):
#   linux/    — firesim/linux v6.6 fork
#   busybox/  — busybox 1.36
#   opensbi/  — OpenSBI firmware
#
# Outputs (in bb-tests/output/kernel/<chip>/ for chip builds):
#   fw_payload*.bin — OpenSBI + Linux + initramfs (complete boot image)
#   fw_payload*.hex — Verilog hex format for P2E DDR loading
#
# Usage:
#   cmake -B <build_dir> -S .
#   cmake --build <build_dir> --target kernel-build
#
cmake_minimum_required(VERSION 3.19)
project(bbkernel NONE)

set(KERNEL_DIR     ${CMAKE_CURRENT_SOURCE_DIR})
set(THIRDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty)
set(LINUX_DIR      ${THIRDPARTY_DIR}/linux)
set(BUSYBOX_DIR    ${THIRDPARTY_DIR}/busybox)
set(OPENSBI_DIR    ${THIRDPARTY_DIR}/opensbi)
set(CONFIG_DIR     ${KERNEL_DIR}/config)
set(SHARED_OVERLAY_DIR ${KERNEL_DIR}/overlay)
set(PLATFORM_DIR   ${KERNEL_DIR}/opensbi-platform)
# Keep generic images in output/kernel, and isolate chip-specific images so
# identically named model payloads from different chips cannot overwrite one
# another.
set(OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/kernel)
if(BUCKYBALL_KERNEL_CHIP)
  set(OUTPUT_DIR ${OUTPUT_DIR}/${BUCKYBALL_KERNEL_CHIP})
endif()

set(CROSS_COMPILE  riscv64-unknown-linux-gnu-)
set(ARCH           riscv)
set(NPROC          4)

set(BUCKYBALL_VISIBLE_HART_COUNT 64 CACHE STRING "Linux-visible Buckyball hart count")
set(BUCKYBALL_TOTAL_HART_COUNT ${BUCKYBALL_VISIBLE_HART_COUNT} CACHE STRING "Total hardware Buckyball hart count")
set(BUCKYBALL_KERNEL_MODEL "" CACHE STRING "Model artifact copied into initramfs /root")
set(BUCKYBALL_GUEST_MEMORY_MIB 512 CACHE STRING "Linux guest memory budget in MiB")
if(NOT BUCKYBALL_GUEST_MEMORY_MIB MATCHES "^[1-9][0-9]*$" OR BUCKYBALL_GUEST_MEMORY_MIB GREATER 16384)
  message(FATAL_ERROR "BUCKYBALL_GUEST_MEMORY_MIB must be in 1..16384 (P2E DDR capacity)")
endif()
set(MEMORY_BUDGET_SCRIPT ${KERNEL_DIR}/scripts/memory-budget.py)
set(BUCKYBALL_MODEL_STORAGE initramfs CACHE STRING "Model storage: initramfs or pmem")
if(NOT BUCKYBALL_MODEL_STORAGE MATCHES "^(initramfs|pmem)$")
  message(FATAL_ERROR "BUCKYBALL_MODEL_STORAGE must be initramfs or pmem")
endif()
if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem" AND NOT BUCKYBALL_KERNEL_MODEL)
  message(FATAL_ERROR "pmem storage requires a model")
endif()
set(PMEM_SCRIPT ${KERNEL_DIR}/scripts/pmem-image.py)
math(EXPR BUCKYBALL_GUEST_MEMORY_BYTES "${BUCKYBALL_GUEST_MEMORY_MIB} * 1048576")
set(BUCKYBALL_FDT_SIZE 262144)
math(EXPR BUCKYBALL_FDT_ADDR "2147483648 + ${BUCKYBALL_GUEST_MEMORY_BYTES} - ${BUCKYBALL_FDT_SIZE}")
set(BUCKYBALL_PMEM_BASE 0)
set(BUCKYBALL_PMEM_SIZE 0)
if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
  if(BUCKYBALL_GUEST_MEMORY_MIB GREATER_EQUAL 16384)
    message(FATAL_ERROR "pmem requires reserved DDR beyond the Linux guest memory")
  endif()
  math(EXPR BUCKYBALL_PMEM_BASE "2147483648 + ${BUCKYBALL_GUEST_MEMORY_BYTES}")
  math(EXPR BUCKYBALL_PMEM_SIZE "17179869184 - ${BUCKYBALL_GUEST_MEMORY_BYTES}")
endif()
set(BUCKYBALL_KERNEL_CHIP "" CACHE STRING
  "Chip name; pk overlay when alone, model artifact under stack/models/build/<chip> when with model")
set(BUCKYBALL_CHIP_KERNEL_DIR "" CACHE PATH "Per-chip kernel dir (overlay/, etc.)")
set(BUCKYBALL_PK_WORKLOAD_TOML "" CACHE FILEPATH "workloads-pk.toml used to filter *-linux install when chip is set")
set(BUCKYBALL_MODEL_DATASET "" CACHE STRING
  "Relative dataset path under /root (e.g. datasets/MNIST); empty = smoke image only")
option(BUCKYBALL_KERNEL_INTERACTIVE
  "Keep shared /init shell; do not auto-run chip/model workloads" OFF)
unset(BUCKYBALL_HIDDEN_HART_BASE CACHE)
set(BUCKYBALL_HIDDEN_HART_BASE ${BUCKYBALL_VISIBLE_HART_COUNT})

if(BUCKYBALL_KERNEL_MODEL AND BUCKYBALL_KERNEL_CHIP STREQUAL "")
  message(FATAL_ERROR
    "BUCKYBALL_KERNEL_CHIP is required when BUCKYBALL_KERNEL_MODEL is set")
endif()

# pk regression mode: chip alone (no model)
if(BUCKYBALL_KERNEL_CHIP AND NOT BUCKYBALL_KERNEL_MODEL)
  if(BUCKYBALL_CHIP_KERNEL_DIR STREQUAL "")
    message(FATAL_ERROR "BUCKYBALL_CHIP_KERNEL_DIR is required when BUCKYBALL_KERNEL_CHIP is set")
  endif()
  if(NOT EXISTS ${BUCKYBALL_CHIP_KERNEL_DIR}/overlay/init)
    message(FATAL_ERROR
      "Chip OS overlay init not found: ${BUCKYBALL_CHIP_KERNEL_DIR}/overlay/init")
  endif()
  if(BUCKYBALL_PK_WORKLOAD_TOML STREQUAL "" OR NOT EXISTS ${BUCKYBALL_PK_WORKLOAD_TOML})
    message(FATAL_ERROR
      "BUCKYBALL_PK_WORKLOAD_TOML missing or not found: ${BUCKYBALL_PK_WORKLOAD_TOML}")
  endif()
endif()

set(FW_PAYLOAD_NAME fw_payload)
if(NOT BUCKYBALL_VISIBLE_HART_COUNT STREQUAL "64" OR NOT BUCKYBALL_TOTAL_HART_COUNT STREQUAL "64")
  set(FW_PAYLOAD_NAME ${FW_PAYLOAD_NAME}-v${BUCKYBALL_VISIBLE_HART_COUNT}-t${BUCKYBALL_TOTAL_HART_COUNT})
endif()
if(BUCKYBALL_KERNEL_MODEL)
  set(FW_PAYLOAD_NAME ${FW_PAYLOAD_NAME}-${BUCKYBALL_KERNEL_MODEL})
elseif(BUCKYBALL_KERNEL_CHIP)
  set(FW_PAYLOAD_NAME ${FW_PAYLOAD_NAME}-${BUCKYBALL_KERNEL_CHIP}-pk)
endif()

if(NOT BUCKYBALL_GUEST_MEMORY_MIB STREQUAL "512")
  set(FW_PAYLOAD_NAME ${FW_PAYLOAD_NAME}-mem${BUCKYBALL_GUEST_MEMORY_MIB}M)
endif()
if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
  set(FW_PAYLOAD_NAME ${FW_PAYLOAD_NAME}-pmem)
endif()
set(OPENSBI_FW    ${OUTPUT_DIR}/${FW_PAYLOAD_NAME}.bin)
set(BUSYBOX_BIN   ${CMAKE_BINARY_DIR}/busybox/busybox)
set(ROOTFS_DIR    ${CMAKE_BINARY_DIR}/rootfs)
set(ROOTFS_STAMP  ${CMAKE_BINARY_DIR}/rootfs.stamp)
set(INITRAMFS_DEVNODES ${CONFIG_DIR}/initramfs-devnodes.list)

# ── busybox ───────────────────────────────────────────────────────────────────
add_custom_command(
    OUTPUT  ${BUSYBOX_BIN}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/busybox
    # Copy config into the out-of-tree build dir (not into source tree)
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CONFIG_DIR}/busybox-config
            ${CMAKE_BINARY_DIR}/busybox/.config
    COMMAND make -C ${BUSYBOX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${CMAKE_BINARY_DIR}/busybox
            oldconfig
    COMMAND make -C ${BUSYBOX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${CMAKE_BINARY_DIR}/busybox
            -j${NPROC}
    DEPENDS ${CONFIG_DIR}/busybox-config
    COMMENT "Building busybox (static RISC-V)"
    VERBATIM
)

add_custom_target(busybox-build DEPENDS ${BUSYBOX_BIN})

# ── initramfs source tree ────────────────────────────────────────────────────
set(WORKLOADS_OUTPUT_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/${BUCKYBALL_KERNEL_CHIP}/workloads)
get_filename_component(REPO_ROOT ${KERNEL_DIR}/../../../.. ABSOLUTE)
set(MODEL_BUILD_DIR ${REPO_ROOT}/stack/models/build/${BUCKYBALL_KERNEL_CHIP}/${BUCKYBALL_KERNEL_MODEL})
set(MODEL_OUTPUT_DIR ${MODEL_BUILD_DIR}/artifact)
set(MODEL_RUN_CONFIG ${REPO_ROOT}/examples/models/${BUCKYBALL_KERNEL_CHIP}/${BUCKYBALL_KERNEL_MODEL}/configs/running-param.toml)
set(BUCKYBALL_TILE_TASKS 0)
if(BUCKYBALL_KERNEL_MODEL AND EXISTS ${MODEL_OUTPUT_DIR}/layout.json)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MODEL_OUTPUT_DIR}/layout.json)
  file(READ ${MODEL_OUTPUT_DIR}/layout.json TASK_LAYOUT)
  string(JSON TASK_RUNTIME ERROR_VARIABLE TASK_RUNTIME_ERROR GET "${TASK_LAYOUT}" execution p2e task_runtime)
  if(TASK_RUNTIME_ERROR)
    string(JSON TASK_RUNTIME ERROR_VARIABLE TASK_RUNTIME_ERROR GET "${TASK_LAYOUT}" execution task_runtime)
  endif()
  if(NOT TASK_RUNTIME_ERROR AND TASK_RUNTIME STREQUAL "tile")
    set(BUCKYBALL_TILE_TASKS 1)
  endif()
endif()

set(INSTALL_WORKLOADS_SCRIPT ${KERNEL_DIR}/scripts/install-workloads.sh)
set(INSTALL_MODEL_SCRIPT ${KERNEL_DIR}/scripts/install-model.py)
set(INSTALL_DATASET_SCRIPT ${KERNEL_DIR}/scripts/install-dataset.sh)
set(MODEL_DATASETS_DIR ${MODEL_BUILD_DIR}/generated/datasets)
set(EXTRACT_TOML_TESTS_SCRIPT ${KERNEL_DIR}/scripts/extract_toml_tests.py)
set(PK_ALLOWLIST ${CMAKE_BINARY_DIR}/pk-workloads.allowlist)
set(CHIP_INIT "")

if(BUCKYBALL_KERNEL_CHIP AND NOT BUCKYBALL_KERNEL_MODEL)
  set(CHIP_INIT ${BUCKYBALL_CHIP_KERNEL_DIR}/overlay/init)
  execute_process(
    COMMAND python3 ${EXTRACT_TOML_TESTS_SCRIPT}
            ${BUCKYBALL_PK_WORKLOAD_TOML}
            ${PK_ALLOWLIST}
    RESULT_VARIABLE _extract_rc
    OUTPUT_VARIABLE _extract_out
    ERROR_VARIABLE _extract_err)
  if(NOT _extract_rc EQUAL 0)
    message(FATAL_ERROR
      "Failed to extract pk workload list from ${BUCKYBALL_PK_WORKLOAD_TOML}:\n${_extract_err}")
  endif()
  message(STATUS "${_extract_out}")
endif()

set(MODEL_INIT "")
set(WORKER_INSTALL_COMMAND)
set(WORKER_CPU_ARGS)
set(WORKER_DEPENDS)
set(BUDGET_ROOTFS_COMMAND)
set(BUDGET_IMAGE_COMMAND)
if(BUCKYBALL_KERNEL_MODEL)
  if(NOT EXISTS ${MODEL_OUTPUT_DIR}/layout.json)
    message(FATAL_ERROR "Model artifact missing: ${MODEL_OUTPUT_DIR}; run bbdev model --build first")
  endif()
  file(READ ${MODEL_OUTPUT_DIR}/layout.json MODEL_LAYOUT)
  string(JSON MODEL_P2E ERROR_VARIABLE MODEL_P2E_ERROR GET "${MODEL_LAYOUT}" execution p2e)
  if(NOT MODEL_P2E_ERROR)
    string(JSON MODEL_EXECUTION GET "${MODEL_P2E}" kind)
  else()
    string(JSON MODEL_EXECUTION GET "${MODEL_LAYOUT}" execution kind)
  endif()
  if(MODEL_EXECUTION STREQUAL "host-worker")
    string(JSON WORKER_TASK_RUNTIME ERROR_VARIABLE WORKER_TASK_ERROR GET "${MODEL_P2E}" task_runtime)
    if(WORKER_TASK_ERROR OR NOT WORKER_TASK_RUNTIME STREQUAL "tile")
      message(FATAL_ERROR "host-worker requires execution.p2e.task_runtime=tile")
    endif()
    if(NOT BUCKYBALL_TOTAL_HART_COUNT GREATER BUCKYBALL_VISIBLE_HART_COUNT)
      message(FATAL_ERROR "host-worker requires hidden harts: total-hart-count must exceed visible-hart-count")
    endif()
    string(JSON WORKER_COUNT GET "${MODEL_P2E}" workers)
    string(JSON WORKER_PROTOCOL GET "${MODEL_P2E}" protocol)
    if(NOT WORKER_COUNT EQUAL 8 OR NOT WORKER_PROTOCOL STREQUAL "bbmux1")
      message(FATAL_ERROR "host-worker requires workers=8 and protocol=bbmux1")
    endif()
    if(BUCKYBALL_VISIBLE_HART_COUNT LESS WORKER_COUNT)
      message(FATAL_ERROR "host-worker requires at least ${WORKER_COUNT} Linux-visible CPUs")
    endif()
    set(WORKER_CPU_SCRIPT ${KERNEL_DIR}/scripts/worker-cpus.py)
    set(WORKER_CHIP_PB ${REPO_ROOT}/examples/chips/${BUCKYBALL_KERNEL_CHIP}/configs/generated/chip.pb)
    set(WORKER_PROTO_DIR ${REPO_ROOT}/bbdev/api/steps/config/scripts)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${WORKER_CHIP_PB} ${WORKER_CPU_SCRIPT} ${WORKER_PROTO_DIR}/chip_pb2.py)
    execute_process(COMMAND python3 ${WORKER_CPU_SCRIPT}
      --chip-pb ${WORKER_CHIP_PB} --proto-dir ${WORKER_PROTO_DIR}
      --workers ${WORKER_COUNT} --visible ${BUCKYBALL_VISIBLE_HART_COUNT}
      OUTPUT_VARIABLE WORKER_CPUS OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE WORKER_CPU_RESULT)
    if(NOT WORKER_CPU_RESULT EQUAL 0)
      message(FATAL_ERROR "Cannot resolve worker affinity from chip.pb")
    endif()
    message(STATUS "Worker CPUs in Tile order: ${WORKER_CPUS}")
    set(WORKER_CPU_ARGS --worker-cpus ${WORKER_CPUS})
    if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
      execute_process(COMMAND python3 ${PMEM_SCRIPT} check --source ${MODEL_OUTPUT_DIR}
        --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB} RESULT_VARIABLE BUDGET_RESULT)
    else()
      execute_process(COMMAND python3 ${MEMORY_BUDGET_SCRIPT}
        --resources ${MODEL_OUTPUT_DIR} --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB}
        RESULT_VARIABLE BUDGET_RESULT)
    endif()
    if(NOT BUDGET_RESULT EQUAL 0)
      message(FATAL_ERROR "Model resources exceed the selected storage budget")
    endif()
    set(GUEST_SOURCE ${REPO_ROOT}/stack/serving/loader/guest)
    set(GUEST_BINARY ${CMAKE_BINARY_DIR}/guest/worker-mux)
    set(GUEST_EXTRA_TARGET)
    set(GUEST_EXTRA_BINARY)
    if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
      set(GUEST_EXTRA_TARGET dax-check)
      set(GUEST_EXTRA_BINARY ${CMAKE_BINARY_DIR}/guest/dax-check)
    endif()
    add_custom_command(OUTPUT ${GUEST_BINARY} ${GUEST_EXTRA_BINARY}
      COMMAND ${CMAKE_COMMAND} -S ${GUEST_SOURCE} -B ${CMAKE_BINARY_DIR}/guest
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64
        -DCMAKE_CXX_COMPILER=${CROSS_COMPILE}g++
      COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/guest --target worker-mux ${GUEST_EXTRA_TARGET}
      DEPENDS ${GUEST_SOURCE}/CMakeLists.txt ${GUEST_SOURCE}/mux.cpp ${GUEST_SOURCE}/dax-check.cpp VERBATIM)
    set(TRANSPORT_CHECK ${CMAKE_BINARY_DIR}/transport-check.stamp)
    add_custom_command(OUTPUT ${TRANSPORT_CHECK}
      COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${REPO_ROOT}
        python3 ${KERNEL_DIR}/tests/transport.py
      COMMAND ${CMAKE_COMMAND} -E touch ${TRANSPORT_CHECK}
      DEPENDS ${REPO_ROOT}/stack/serving/transport.py ${KERNEL_DIR}/tests/transport.py
      VERBATIM)
    set(WORKER_DEPENDS ${GUEST_BINARY} ${GUEST_EXTRA_BINARY} ${TRANSPORT_CHECK} ${WORKER_CPU_SCRIPT} ${WORKER_CHIP_PB})
    set(WORKER_INSTALL_COMMAND
      COMMAND ${CMAKE_COMMAND} -E make_directory ${ROOTFS_DIR}/root/guest
      COMMAND ${CMAKE_COMMAND} -E copy ${GUEST_BINARY} ${ROOTFS_DIR}/root/guest/worker-mux)
    if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
      list(APPEND WORKER_INSTALL_COMMAND COMMAND ${CMAKE_COMMAND} -E copy
        ${GUEST_EXTRA_BINARY} ${ROOTFS_DIR}/root/guest/dax-check)
    endif()
    set(BUDGET_ROOTFS_COMMAND COMMAND python3 ${MEMORY_BUDGET_SCRIPT}
      --rootfs ${ROOTFS_DIR} --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB})
    set(BUDGET_IMAGE_COMMAND COMMAND python3 ${MEMORY_BUDGET_SCRIPT}
      --rootfs ${ROOTFS_DIR} --image ${CMAKE_BINARY_DIR}/linux/arch/riscv/boot/Image
      --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB})
  elseif(NOT MODEL_EXECUTION STREQUAL "native")
    message(FATAL_ERROR "Kernel model images require native or host-worker P2E execution")
  endif()
  if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem" AND NOT MODEL_EXECUTION STREQUAL "host-worker")
    message(FATAL_ERROR "pmem storage requires host-worker execution")
  endif()
  file(GLOB MODEL_PROGRAMS ${MODEL_OUTPUT_DIR}/*-run)
  list(LENGTH MODEL_PROGRAMS MODEL_PROGRAM_COUNT)
  if(NOT MODEL_PROGRAM_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one model executable in ${MODEL_OUTPUT_DIR}")
  endif()
  list(GET MODEL_PROGRAMS 0 MODEL_PROGRAM)
  get_filename_component(MODEL_RUN_BINARY ${MODEL_PROGRAM} NAME)

  set(INSTALL_DATASET_COMMAND bash -c "true")
  set(MODEL_DATASET_DEPENDS "")
  if(NOT BUCKYBALL_MODEL_DATASET STREQUAL "")
    if(BUCKYBALL_MODEL_DATASET MATCHES "^/" OR BUCKYBALL_MODEL_DATASET MATCHES "\\.\\.")
      message(FATAL_ERROR
        "BUCKYBALL_MODEL_DATASET must be relative without '..': ${BUCKYBALL_MODEL_DATASET}")
    endif()
    if(NOT BUCKYBALL_MODEL_DATASET MATCHES "^datasets/.+")
      message(FATAL_ERROR
        "BUCKYBALL_MODEL_DATASET must look like datasets/<name>: ${BUCKYBALL_MODEL_DATASET}")
    endif()
    set(HOST_DATASET_DIR ${MODEL_DATASETS_DIR})
    if(NOT EXISTS ${HOST_DATASET_DIR})
      message(FATAL_ERROR "Host dataset dir not found: ${HOST_DATASET_DIR}")
    endif()
    set(INSTALL_DATASET_COMMAND
        bash ${INSTALL_DATASET_SCRIPT}
        ${HOST_DATASET_DIR}
        ${ROOTFS_DIR}/root
        ${BUCKYBALL_MODEL_DATASET})
    file(GLOB_RECURSE MODEL_DATASET_DEPENDS CONFIGURE_DEPENDS ${HOST_DATASET_DIR}/*)
  endif()

  if(NOT BUCKYBALL_KERNEL_INTERACTIVE)
    set(MODEL_INIT ${CMAKE_BINARY_DIR}/overlay-model/init)
    set(MODEL_INIT_TEMPLATE ${KERNEL_DIR}/overlay-model/init.in)
    if(MODEL_EXECUTION STREQUAL "host-worker")
      set(MODEL_INIT_TEMPLATE ${KERNEL_DIR}/overlay-worker/init.in)
    endif()
    configure_file(
      ${MODEL_INIT_TEMPLATE}
      ${MODEL_INIT}
      @ONLY
    )
  endif()

  file(GLOB_RECURSE MODEL_RUNTIME_FILES CONFIGURE_DEPENDS ${MODEL_OUTPUT_DIR}/*)
  list(APPEND MODEL_RUNTIME_FILES ${MODEL_RUN_CONFIG})
  set(INSTALL_RUNTIME_COMMAND
      python3 ${INSTALL_MODEL_SCRIPT}
      ${MODEL_OUTPUT_DIR}
      ${MODEL_RUN_BINARY}
      ${ROOTFS_DIR}/root
      ${MODEL_RUN_CONFIG}
      "--dataset=${BUCKYBALL_MODEL_DATASET}"
      "--guest-memory-mib=${BUCKYBALL_GUEST_MEMORY_MIB}"
      "--model-storage=${BUCKYBALL_MODEL_STORAGE}" ${WORKER_CPU_ARGS})
else()
  set(INSTALL_DATASET_COMMAND bash -c "true")
  set(MODEL_DATASET_DEPENDS "")

  if(BUCKYBALL_KERNEL_CHIP)
    set(INSTALL_RUNTIME_COMMAND
        bash ${INSTALL_WORKLOADS_SCRIPT}
        ${WORKLOADS_OUTPUT_DIR}
        ${ROOTFS_DIR}/root
        ${PK_ALLOWLIST})
  else()
    set(INSTALL_RUNTIME_COMMAND
        bash ${INSTALL_WORKLOADS_SCRIPT}
        ${WORKLOADS_OUTPUT_DIR}
        ${ROOTFS_DIR}/root)
  endif()
endif()

set(OVERLAY_COPY_COMMAND
    bash -c "cp -r ${SHARED_OVERLAY_DIR}/. ${ROOTFS_DIR}/")
set(ROOTFS_EXTRA_DEPENDS ${SHARED_OVERLAY_DIR}/init)
if(BUCKYBALL_KERNEL_CHIP AND NOT BUCKYBALL_KERNEL_MODEL AND NOT BUCKYBALL_KERNEL_INTERACTIVE)
  set(OVERLAY_COPY_COMMAND
      bash -c "cp -r ${SHARED_OVERLAY_DIR}/. ${ROOTFS_DIR}/ && cp -r ${BUCKYBALL_CHIP_KERNEL_DIR}/overlay/. ${ROOTFS_DIR}/")
  list(APPEND ROOTFS_EXTRA_DEPENDS
       ${CHIP_INIT}
       ${EXTRACT_TOML_TESTS_SCRIPT}
       ${PK_ALLOWLIST}
       ${BUCKYBALL_PK_WORKLOAD_TOML})
elseif(BUCKYBALL_KERNEL_CHIP AND NOT BUCKYBALL_KERNEL_MODEL)
  list(APPEND ROOTFS_EXTRA_DEPENDS
       ${EXTRACT_TOML_TESTS_SCRIPT}
       ${PK_ALLOWLIST}
       ${BUCKYBALL_PK_WORKLOAD_TOML})
endif()
if(BUCKYBALL_KERNEL_MODEL AND NOT BUCKYBALL_KERNEL_INTERACTIVE)
  set(OVERLAY_COPY_COMMAND
      bash -c "cp -r ${SHARED_OVERLAY_DIR}/. ${ROOTFS_DIR}/ && cp ${MODEL_INIT} ${ROOTFS_DIR}/init")
  list(APPEND ROOTFS_EXTRA_DEPENDS ${MODEL_RUNTIME_FILES} ${MODEL_INIT}
       ${MODEL_INIT_TEMPLATE} ${INSTALL_DATASET_SCRIPT}
       ${MODEL_DATASET_DEPENDS})
elseif(BUCKYBALL_KERNEL_MODEL)
  list(APPEND ROOTFS_EXTRA_DEPENDS ${MODEL_RUNTIME_FILES})
endif()

set(BUSYBOX_APPLETS
    sh ash ls cat echo mount umount mkdir rm cp mv ps top free sleep poweroff reboot)
set(BUSYBOX_LINK_COMMANDS)
foreach(APPLET IN LISTS BUSYBOX_APPLETS)
  list(APPEND BUSYBOX_LINK_COMMANDS
       COMMAND ${CMAKE_COMMAND} -E create_symlink busybox ${ROOTFS_DIR}/bin/${APPLET})
endforeach()

add_custom_command(
    OUTPUT  ${ROOTFS_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ROOTFS_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROOTFS_DIR}/bin
                                               ${ROOTFS_DIR}/sbin
                                               ${ROOTFS_DIR}/etc
                                               ${ROOTFS_DIR}/proc
                                               ${ROOTFS_DIR}/sys
                                               ${ROOTFS_DIR}/dev
                                               ${ROOTFS_DIR}/tmp
                                               ${ROOTFS_DIR}/root ${ROOTFS_DIR}/model
    # Install busybox and create symlinks
    COMMAND ${CMAKE_COMMAND} -E copy ${BUSYBOX_BIN} ${ROOTFS_DIR}/bin/busybox
    ${BUSYBOX_LINK_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E create_symlink ../bin/busybox ${ROOTFS_DIR}/sbin/init
    # Shared overlay, then optional per-chip OS overlay (init, etc.)
    COMMAND ${OVERLAY_COPY_COMMAND}
    COMMAND chmod +x ${ROOTFS_DIR}/init
    # Copy workload binaries or a single model runtime.
    ${WORKER_INSTALL_COMMAND}
    COMMAND ${INSTALL_RUNTIME_COMMAND}
    COMMAND ${INSTALL_DATASET_COMMAND}
    ${BUDGET_ROOTFS_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E touch ${ROOTFS_STAMP}
    DEPENDS ${BUSYBOX_BIN} ${WORKER_DEPENDS} ${MEMORY_BUDGET_SCRIPT} ${PMEM_SCRIPT}
            ${INITRAMFS_DEVNODES}
            ${INSTALL_WORKLOADS_SCRIPT}
            ${INSTALL_MODEL_SCRIPT}
            ${INSTALL_DATASET_SCRIPT}
            ${ROOTFS_EXTRA_DEPENDS}
    COMMENT "Creating initramfs source tree"
    VERBATIM
)

add_custom_target(rootfs-build DEPENDS ${ROOTFS_STAMP})

# ── linux kernel with initramfs ───────────────────────────────────────────────
set(LINUX_BUILD_DIR ${CMAKE_BINARY_DIR}/linux)
set(LINUX_IMAGE_PATH ${LINUX_BUILD_DIR}/arch/riscv/boot/Image)
file(READ ${CONFIG_DIR}/linux-config KERNEL_CONFIG)
string(REPLACE "mem=512M" "mem=${BUCKYBALL_GUEST_MEMORY_MIB}M" KERNEL_CONFIG "${KERNEL_CONFIG}")
if(MODEL_EXECUTION STREQUAL "host-worker")
  string(REPLACE "loglevel=8 ignore_loglevel" "loglevel=0" KERNEL_CONFIG "${KERNEL_CONFIG}")
endif()
if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
  string(APPEND KERNEL_CONFIG "\nCONFIG_LIBNVDIMM=y\nCONFIG_OF_PMEM=y\nCONFIG_BLK_DEV_PMEM=y\nCONFIG_FS_DAX=y\nCONFIG_FS_DAX_LIMITED=y\nCONFIG_EXT2_FS=y\n")
endif()
file(WRITE ${CMAKE_BINARY_DIR}/linux-config "${KERNEL_CONFIG}")

add_custom_command(
    OUTPUT  ${LINUX_IMAGE_PATH}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LINUX_BUILD_DIR}
    COMMAND make -C ${LINUX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${LINUX_BUILD_DIR}
            defconfig
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/linux-config
            ${LINUX_BUILD_DIR}/.config
    # Set initramfs source to rootfs directory plus device-node spec.
    COMMAND bash -c "echo 'CONFIG_INITRAMFS_SOURCE=\"${ROOTFS_DIR} ${INITRAMFS_DEVNODES}\"' >> ${LINUX_BUILD_DIR}/.config"
    COMMAND make -C ${LINUX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${LINUX_BUILD_DIR}
            olddefconfig
    COMMAND make -C ${LINUX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${LINUX_BUILD_DIR}
            -j${NPROC}
            Image
    ${BUDGET_IMAGE_COMMAND}
    DEPENDS ${CMAKE_BINARY_DIR}/linux-config ${ROOTFS_STAMP} ${INITRAMFS_DEVNODES}
            ${LINUX_DIR}/drivers/nvdimm/Kconfig
            ${LINUX_DIR}/drivers/nvdimm/pmem.c
            ${LINUX_DIR}/fs/ext2/file.c
    COMMENT "Building Linux kernel with initramfs"
    VERBATIM
)

add_custom_target(linux-build DEPENDS ${LINUX_IMAGE_PATH})

# ── device tree ──────────────────────────────────────────────────────────────
# The BootROM passes no DTB: OpenSBI takes this one, generated from the chip PB, from its payload.
# The workload build also configures this project without a chip; only fw_payload needs one.
set(BUCKYBALL_TIMEBASE_HZ 5000000 CACHE STRING "SoC clock that advances CLINT mtime (P2E user_clk)")
if(BUCKYBALL_KERNEL_CHIP)
  find_program(PYTHON3 python3 REQUIRED)
  find_program(DTC dtc REQUIRED)
  set(REPO_ROOT ${KERNEL_DIR}/../../../..)
  set(CHIP_PB  ${REPO_ROOT}/examples/chips/${BUCKYBALL_KERNEL_CHIP}/configs/generated/chip.pb)
  set(CHIP_DTS ${CMAKE_BINARY_DIR}/${BUCKYBALL_KERNEL_CHIP}.dts)
  set(CHIP_DTB ${CMAKE_BINARY_DIR}/${BUCKYBALL_KERNEL_CHIP}.dtb)
  add_custom_command(
      OUTPUT  ${CHIP_DTB}
      COMMAND ${PYTHON3} ${KERNEL_DIR}/scripts/chip-dts.py
              --repo ${REPO_ROOT}
              --chip-pb ${CHIP_PB}
              --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB}
              --timebase-hz ${BUCKYBALL_TIMEBASE_HZ}
              --output ${CHIP_DTS}
      COMMAND ${DTC} -I dts -O dtb -o ${CHIP_DTB} ${CHIP_DTS}
      DEPENDS ${CHIP_PB} ${KERNEL_DIR}/scripts/chip-dts.py
      COMMENT "Generating the ${BUCKYBALL_KERNEL_CHIP} device tree"
      VERBATIM
  )
else()
  set(CHIP_DTB ${CMAKE_BINARY_DIR}/no-chip.dtb)
  add_custom_command(
      OUTPUT  ${CHIP_DTB}
      COMMAND ${CMAKE_COMMAND} -E echo "fw_payload needs BUCKYBALL_KERNEL_CHIP to generate its device tree"
      COMMAND ${CMAKE_COMMAND} -E false
      VERBATIM
  )
endif()

# ── opensbi fw_payload ───────────────────────────────────────────────────────
set(OPENSBI_BUILD_DIR ${CMAKE_BINARY_DIR}/opensbi)
set(OPENSBI_FW_PATH ${OPENSBI_BUILD_DIR}/platform/buckyball/firmware/fw_payload.bin)
set(OPENSBI_PLATFORM_CONFIG ${CMAKE_BINARY_DIR}/opensbi-platform.config)
file(GENERATE OUTPUT ${OPENSBI_PLATFORM_CONFIG} CONTENT
  "tile_tasks=${BUCKYBALL_TILE_TASKS}\nvisible=${BUCKYBALL_VISIBLE_HART_COUNT}\ntotal=${BUCKYBALL_TOTAL_HART_COUNT}\nhidden_base=${BUCKYBALL_HIDDEN_HART_BASE}\nguest_bytes=${BUCKYBALL_GUEST_MEMORY_BYTES}\nfdt_addr=${BUCKYBALL_FDT_ADDR}\nfdt_size=${BUCKYBALL_FDT_SIZE}\npmem_base=${BUCKYBALL_PMEM_BASE}\npmem_size=${BUCKYBALL_PMEM_SIZE}\n")
set(OPENSBI_PLATFORM_SOURCES
    ${PLATFORM_DIR}/buckyball/Kconfig
    ${PLATFORM_DIR}/buckyball/objects.mk
    ${PLATFORM_DIR}/buckyball/platform.c
    ${PLATFORM_DIR}/buckyball/task-worker.S)

add_custom_command(
    OUTPUT  ${OPENSBI_FW_PATH}
    COMMAND make -B -C ${OPENSBI_DIR}
            CROSS_COMPILE=${CROSS_COMPILE}
            PLATFORM_DIR=${PLATFORM_DIR}
            PLATFORM=buckyball
            O=${OPENSBI_BUILD_DIR}
            FW_PAYLOAD_PATH=${LINUX_IMAGE_PATH}
            FW_FDT_PATH=${CHIP_DTB}
            BUCKYBALL_VISIBLE_HART_COUNT=${BUCKYBALL_VISIBLE_HART_COUNT}
            BUCKYBALL_TOTAL_HART_COUNT=${BUCKYBALL_TOTAL_HART_COUNT}
            BUCKYBALL_HIDDEN_HART_BASE=${BUCKYBALL_HIDDEN_HART_BASE}
            BUCKYBALL_TILE_TASKS=${BUCKYBALL_TILE_TASKS}
            BUCKYBALL_GUEST_MEMORY_BYTES=${BUCKYBALL_GUEST_MEMORY_BYTES}
            BUCKYBALL_FDT_ADDR=${BUCKYBALL_FDT_ADDR}
            BUCKYBALL_FDT_SIZE=${BUCKYBALL_FDT_SIZE}
            BUCKYBALL_PMEM_BASE=${BUCKYBALL_PMEM_BASE}
            BUCKYBALL_PMEM_SIZE=${BUCKYBALL_PMEM_SIZE}
    DEPENDS ${LINUX_IMAGE_PATH} ${CHIP_DTB} ${OPENSBI_PLATFORM_SOURCES} ${OPENSBI_PLATFORM_CONFIG}
    COMMENT "Building OpenSBI fw_payload with Linux"
    VERBATIM
)

add_custom_target(opensbi-build DEPENDS ${OPENSBI_FW_PATH})

# ── output copy ───────────────────────────────────────────────────────────────
add_custom_command(
    OUTPUT  ${OPENSBI_FW}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
    ${BUDGET_IMAGE_COMMAND}
    COMMAND python3 ${MEMORY_BUDGET_SCRIPT} --payload ${OPENSBI_FW_PATH}
      --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${OPENSBI_FW_PATH} ${OPENSBI_FW}
    DEPENDS ${OPENSBI_FW_PATH}
    COMMENT "Copying OpenSBI fw_payload to output"
    VERBATIM
)

set(STORAGE_OUTPUT)
if(BUCKYBALL_MODEL_STORAGE STREQUAL "pmem")
  set(PMEM_IMAGE ${OUTPUT_DIR}/${FW_PAYLOAD_NAME}.model.ext2)
  set(PMEM_RESOURCE_FILES ${MODEL_OUTPUT_DIR}/layout.json)
  string(JSON PMEM_RESOURCE_COUNT LENGTH "${MODEL_LAYOUT}" resources)
  math(EXPR PMEM_LAST_RESOURCE "${PMEM_RESOURCE_COUNT} - 1")
  foreach(INDEX RANGE ${PMEM_LAST_RESOURCE})
    string(JSON RESOURCE_KEY MEMBER "${MODEL_LAYOUT}" resources ${INDEX})
    string(JSON RESOURCE_FILE GET "${MODEL_LAYOUT}" resources ${RESOURCE_KEY})
    list(APPEND PMEM_RESOURCE_FILES ${MODEL_OUTPUT_DIR}/${RESOURCE_FILE})
  endforeach()
  list(REMOVE_DUPLICATES PMEM_RESOURCE_FILES)
  set(STORAGE_OUTPUT ${OUTPUT_DIR}/${FW_PAYLOAD_NAME}.load.json)
  add_custom_command(OUTPUT ${PMEM_IMAGE}
    BYPRODUCTS ${OUTPUT_DIR}/${FW_PAYLOAD_NAME}.model.files.json
    COMMAND python3 ${PMEM_SCRIPT} image --source ${MODEL_OUTPUT_DIR}
      --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB} --image ${PMEM_IMAGE}
      --staging ${CMAKE_BINARY_DIR}/pmem-root
    DEPENDS ${PMEM_SCRIPT} ${PMEM_RESOURCE_FILES} VERBATIM)
  add_custom_command(OUTPUT ${STORAGE_OUTPUT}
    COMMAND python3 ${PMEM_SCRIPT} manifest --source ${MODEL_OUTPUT_DIR}
      --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB} --image ${PMEM_IMAGE}
      --boot ${OPENSBI_FW} --out ${STORAGE_OUTPUT}
    DEPENDS ${OPENSBI_FW} ${PMEM_IMAGE} ${PMEM_SCRIPT} VERBATIM)
endif()

# ── top-level target ──────────────────────────────────────────────────────────
add_custom_target(kernel-build
    DEPENDS ${OPENSBI_FW} ${STORAGE_OUTPUT}
    COMMENT "OpenSBI fw_payload ready at ${OPENSBI_FW}"
)
