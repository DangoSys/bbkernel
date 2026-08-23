# bbkernel — RISC-V Linux kernel + rootfs builder
#
# Submodules (in bb-tests/thirdparty/):
#   linux/    — firesim/linux v6.6 fork
#   busybox/  — busybox 1.36
#   opensbi/  — OpenSBI firmware
#
# Outputs (in bb-tests/output/kernel/):
#   fw_payload*.bin — OpenSBI + Linux + initramfs (complete boot image)
#   fw_payload*.hex — Verilog hex format for P2E DDR loading
#
# Usage:
#   cmake -B <build_dir> -S .
#   cmake --build <build_dir> --target kernel-build
#
cmake_minimum_required(VERSION 3.16)
project(bbkernel NONE)

set(KERNEL_DIR     ${CMAKE_CURRENT_SOURCE_DIR})
set(THIRDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty)
set(LINUX_DIR      ${THIRDPARTY_DIR}/linux)
set(BUSYBOX_DIR    ${THIRDPARTY_DIR}/busybox)
set(OPENSBI_DIR    ${THIRDPARTY_DIR}/opensbi)
set(CONFIG_DIR     ${KERNEL_DIR}/config)
set(SHARED_OVERLAY_DIR ${KERNEL_DIR}/overlay)
set(PLATFORM_DIR   ${KERNEL_DIR}/opensbi-platform)
# Output goes to bb-tests/output/kernel/ (consistent with other bb-tests outputs)
set(OUTPUT_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/kernel)

set(CROSS_COMPILE  riscv64-unknown-linux-gnu-)
set(ARCH           riscv)
set(NPROC          4)

set(BUCKYBALL_VISIBLE_HART_COUNT 64 CACHE STRING "Linux-visible Buckyball hart count")
set(BUCKYBALL_TOTAL_HART_COUNT ${BUCKYBALL_VISIBLE_HART_COUNT} CACHE STRING "Total hardware Buckyball hart count")
set(BUCKYBALL_KERNEL_MODEL "" CACHE STRING "Single ModelTest model copied into initramfs /root")
set(BUCKYBALL_KERNEL_CHIP "" CACHE STRING
  "Chip name; pk overlay when alone, ModelTest path under archs/buckyball/<chip> when with model")
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
set(WORKLOADS_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/workloads)
set(MODEL_OUTPUT_ROOT
    ${WORKLOADS_OUTPUT_DIR}/src/ModelTest/e2e/models/archs/buckyball/${BUCKYBALL_KERNEL_CHIP})
set(INSTALL_WORKLOADS_SCRIPT ${KERNEL_DIR}/scripts/install-workloads.sh)
set(INSTALL_MODEL_SCRIPT ${KERNEL_DIR}/scripts/install-model.sh)
set(INSTALL_DATASET_SCRIPT ${KERNEL_DIR}/scripts/install-dataset.sh)
set(MODELTEST_DATASETS_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ModelTest/e2e/datasets)
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
if(BUCKYBALL_KERNEL_MODEL)
  if(BUCKYBALL_KERNEL_MODEL STREQUAL "bert")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/Bert)
    set(MODEL_RUN_BINARY buddy-buckyball-bert-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "deepseekr1")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/DeepSeekR1)
    set(MODEL_RUN_BINARY buddy-buckyball-deepseekr1-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "gemma4")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/Gemma4)
    set(MODEL_RUN_BINARY buddy-buckyball-gemma4-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "lenet")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/LeNet)
    set(MODEL_RUN_BINARY buddy-buckyball-lenet-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "llama2")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/llama2)
    set(MODEL_RUN_BINARY buddy-buckyball-llama2-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "mobilenet")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/MobileNetV3)
    set(MODEL_RUN_BINARY buddy-buckyball-mobilenetv3-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "qwen3")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/Qwen3)
    set(MODEL_RUN_BINARY buddy-buckyball-qwen3-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "resnet")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/ResNet18)
    set(MODEL_RUN_BINARY buddy-buckyball-resnet-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "stablediffusion")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/StableDiffusion)
    set(MODEL_RUN_BINARY buddy-buckyball-stablediffusion-run)
  elseif(BUCKYBALL_KERNEL_MODEL STREQUAL "yolo")
    set(MODEL_OUTPUT_DIR ${MODEL_OUTPUT_ROOT}/YOLO26)
    set(MODEL_RUN_BINARY buddy-buckyball-yolo26-run)
  else()
    message(FATAL_ERROR
      "Unknown BUCKYBALL_KERNEL_MODEL='${BUCKYBALL_KERNEL_MODEL}'. "
      "Valid models: bert, deepseekr1, gemma4, lenet, llama2, mobilenet, "
      "qwen3, resnet, stablediffusion, yolo")
  endif()

  if(NOT EXISTS ${MODEL_OUTPUT_DIR})
    message(FATAL_ERROR
      "Model output directory not found: ${MODEL_OUTPUT_DIR}. "
      "Run `bbdev workload --build '--chip ${BUCKYBALL_KERNEL_CHIP} --model ${BUCKYBALL_KERNEL_MODEL}'` first.")
  endif()
  if(NOT EXISTS ${MODEL_OUTPUT_DIR}/${MODEL_RUN_BINARY})
    message(FATAL_ERROR
      "Model run binary not found: ${MODEL_OUTPUT_DIR}/${MODEL_RUN_BINARY}. "
      "Run `bbdev workload --build '--chip ${BUCKYBALL_KERNEL_CHIP} --model ${BUCKYBALL_KERNEL_MODEL}'` first.")
  endif()

  set(INSTALL_DATASET_COMMAND bash -c "true")
  set(MODEL_DATASET_DEPENDS "")
  if(NOT BUCKYBALL_MODEL_DATASET STREQUAL "")
    if(BUCKYBALL_MODEL_DATASET MATCHES "^/" OR BUCKYBALL_MODEL_DATASET MATCHES "\\.\\.")
      message(FATAL_ERROR
        "BUCKYBALL_MODEL_DATASET must be relative without '..': ${BUCKYBALL_MODEL_DATASET}")
    endif()
    string(REGEX REPLACE "^datasets/" "" _DATASET_LEAF "${BUCKYBALL_MODEL_DATASET}")
    if(_DATASET_LEAF STREQUAL "" OR _DATASET_LEAF STREQUAL "${BUCKYBALL_MODEL_DATASET}")
      message(FATAL_ERROR
        "BUCKYBALL_MODEL_DATASET must look like datasets/<name>: ${BUCKYBALL_MODEL_DATASET}")
    endif()
    set(HOST_DATASET_DIR ${MODELTEST_DATASETS_DIR}/${_DATASET_LEAF})
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
    configure_file(
      ${KERNEL_DIR}/overlay-model/init.in
      ${MODEL_INIT}
      @ONLY
    )
  endif()

  file(GLOB_RECURSE MODEL_RUNTIME_FILES CONFIGURE_DEPENDS ${MODEL_OUTPUT_DIR}/*)
  set(INSTALL_RUNTIME_COMMAND
      bash ${INSTALL_MODEL_SCRIPT}
      ${MODEL_OUTPUT_DIR}
      ${MODEL_RUN_BINARY}
      ${ROOTFS_DIR}/root)
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
       ${KERNEL_DIR}/overlay-model/init.in ${INSTALL_DATASET_SCRIPT}
       ${MODEL_DATASET_DEPENDS})
elseif(BUCKYBALL_KERNEL_MODEL)
  list(APPEND ROOTFS_EXTRA_DEPENDS ${MODEL_RUNTIME_FILES})
endif()

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
                                               ${ROOTFS_DIR}/root
    # Install busybox and create symlinks
    COMMAND ${CMAKE_COMMAND} -E copy ${BUSYBOX_BIN} ${ROOTFS_DIR}/bin/busybox
    COMMAND ${CMAKE_COMMAND}
            -DROOTFS_DIR=${ROOTFS_DIR}
            -P ${KERNEL_DIR}/scripts/create-busybox-links.cmake
    # Shared overlay, then optional per-chip OS overlay (init, etc.)
    COMMAND ${OVERLAY_COPY_COMMAND}
    COMMAND chmod +x ${ROOTFS_DIR}/init
    # Copy workload binaries or a single ModelTest runtime.
    COMMAND ${INSTALL_RUNTIME_COMMAND}
    COMMAND ${INSTALL_DATASET_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E touch ${ROOTFS_STAMP}
    DEPENDS ${BUSYBOX_BIN}
            ${INITRAMFS_DEVNODES}
            ${KERNEL_DIR}/scripts/create-busybox-links.cmake
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

add_custom_command(
    OUTPUT  ${LINUX_IMAGE_PATH}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LINUX_BUILD_DIR}
    COMMAND make -C ${LINUX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${LINUX_BUILD_DIR}
            defconfig
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CONFIG_DIR}/linux-config
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
    DEPENDS ${CONFIG_DIR}/linux-config ${ROOTFS_STAMP} ${INITRAMFS_DEVNODES}
    COMMENT "Building Linux kernel with initramfs"
    VERBATIM
)

add_custom_target(linux-build DEPENDS ${LINUX_IMAGE_PATH})

# ── opensbi fw_payload ───────────────────────────────────────────────────────
set(OPENSBI_BUILD_DIR ${CMAKE_BINARY_DIR}/opensbi)
set(OPENSBI_FW_PATH ${OPENSBI_BUILD_DIR}/platform/buckyball/firmware/fw_payload.bin)
set(OPENSBI_PLATFORM_SOURCES
    ${PLATFORM_DIR}/buckyball/Kconfig
    ${PLATFORM_DIR}/buckyball/objects.mk
    ${PLATFORM_DIR}/buckyball/platform.c)

add_custom_command(
    OUTPUT  ${OPENSBI_FW_PATH}
    COMMAND make -B -C ${OPENSBI_DIR}
            CROSS_COMPILE=${CROSS_COMPILE}
            PLATFORM_DIR=${PLATFORM_DIR}
            PLATFORM=buckyball
            O=${OPENSBI_BUILD_DIR}
            FW_PAYLOAD_PATH=${LINUX_IMAGE_PATH}
            BUCKYBALL_VISIBLE_HART_COUNT=${BUCKYBALL_VISIBLE_HART_COUNT}
            BUCKYBALL_TOTAL_HART_COUNT=${BUCKYBALL_TOTAL_HART_COUNT}
            BUCKYBALL_HIDDEN_HART_BASE=${BUCKYBALL_HIDDEN_HART_BASE}
    DEPENDS ${LINUX_IMAGE_PATH} ${OPENSBI_PLATFORM_SOURCES}
    COMMENT "Building OpenSBI fw_payload with Linux"
    VERBATIM
)

add_custom_target(opensbi-build DEPENDS ${OPENSBI_FW_PATH})

# ── output copy ───────────────────────────────────────────────────────────────
add_custom_command(
    OUTPUT  ${OPENSBI_FW}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${OPENSBI_FW_PATH} ${OPENSBI_FW}
    DEPENDS ${OPENSBI_FW_PATH}
    COMMENT "Copying OpenSBI fw_payload to output"
    VERBATIM
)

# ── top-level target ──────────────────────────────────────────────────────────
add_custom_target(kernel-build
    DEPENDS ${OPENSBI_FW}
    COMMENT "OpenSBI fw_payload ready at ${OPENSBI_FW}"
)
