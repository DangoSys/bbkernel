# bbkernel — RISC-V Linux kernel + rootfs builder
#
# Submodules (in bb-tests/thirdparty/):
#   linux/    — firesim/linux v6.6 fork
#   busybox/  — busybox 1.36
#   opensbi/  — OpenSBI firmware
#
# Outputs (in bb-tests/output/kernel/):
#   fw_payload.bin — OpenSBI + Linux + initramfs (complete boot image)
#   fw_payload.hex — Verilog hex format for P2E DDR loading
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
set(OVERLAY_DIR    ${KERNEL_DIR}/overlay)
set(PLATFORM_DIR   ${KERNEL_DIR}/opensbi-platform)
# Output goes to bb-tests/output/kernel/ (consistent with other bb-tests outputs)
set(OUTPUT_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/kernel)

set(CROSS_COMPILE  riscv64-unknown-linux-gnu-)
set(ARCH           riscv)
set(NPROC          4)

set(OPENSBI_FW    ${OUTPUT_DIR}/fw_payload.bin)
set(BUSYBOX_BIN   ${CMAKE_BINARY_DIR}/busybox/busybox)
set(ROOTFS_DIR    ${CMAKE_BINARY_DIR}/rootfs)
set(ROOTFS_CPIO   ${CMAKE_BINARY_DIR}/rootfs.cpio)

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

# ── rootfs cpio ──────────────────────────────────────────────────────────────
set(WORKLOADS_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/workloads)

add_custom_command(
    OUTPUT  ${ROOTFS_CPIO}
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
    COMMAND bash -c "cd ${ROOTFS_DIR}/bin && for app in sh ash ls cat echo mount umount mkdir rm cp mv ps top free; do ln -sf busybox $$app 2>/dev/null; done"
    COMMAND bash -c "ln -sf ../bin/busybox ${ROOTFS_DIR}/sbin/init 2>/dev/null; true"
    # Copy overlay (init script, etc/)
    COMMAND bash -c "cp -r ${OVERLAY_DIR}/. ${ROOTFS_DIR}/"
    COMMAND chmod +x ${ROOTFS_DIR}/init
    # Copy workload binaries
    COMMAND bash ${KERNEL_DIR}/scripts/install-workloads.sh
            ${WORKLOADS_OUTPUT_DIR} ${ROOTFS_DIR}/root
    # Pack into cpio
    COMMAND bash -c "cd ${ROOTFS_DIR} && find . | cpio -o -H newc > ${ROOTFS_CPIO}"
    DEPENDS ${BUSYBOX_BIN} ${OVERLAY_DIR}/init
    COMMENT "Creating initramfs cpio"
    VERBATIM
)

add_custom_target(rootfs-cpio DEPENDS ${ROOTFS_CPIO})

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
    # Set initramfs source to our cpio
    COMMAND bash -c "echo 'CONFIG_INITRAMFS_SOURCE=\"${ROOTFS_CPIO}\"' >> ${LINUX_BUILD_DIR}/.config"
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
    DEPENDS ${CONFIG_DIR}/linux-config ${ROOTFS_CPIO}
    COMMENT "Building Linux kernel with initramfs"
    VERBATIM
)

add_custom_target(linux-build DEPENDS ${LINUX_IMAGE_PATH})

# ── opensbi fw_payload ───────────────────────────────────────────────────────
set(OPENSBI_BUILD_DIR ${CMAKE_BINARY_DIR}/opensbi)
set(OPENSBI_FW_PATH ${OPENSBI_BUILD_DIR}/platform/buckyball/firmware/fw_payload.bin)

add_custom_command(
    OUTPUT  ${OPENSBI_FW_PATH}
    COMMAND make -C ${OPENSBI_DIR}
            CROSS_COMPILE=${CROSS_COMPILE}
            PLATFORM_DIR=${PLATFORM_DIR}
            PLATFORM=buckyball
            O=${OPENSBI_BUILD_DIR}
            FW_PAYLOAD_PATH=${LINUX_IMAGE_PATH}
    DEPENDS ${LINUX_IMAGE_PATH}
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
