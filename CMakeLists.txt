# bbkernel — RISC-V Linux kernel + rootfs builder for Pegasus FPGA
#
# Submodules (in bb-tests/thirdparty/):
#   linux/    — firesim/linux v6.6 fork
#   busybox/  — busybox 1.36
#
# Outputs (in bb-tests/output/kernel/):
#   pegasus-bin   Linux Image (raw binary, RISC-V, loaded at 0x80200000)
#   pegasus.img   ext4 rootfs image (128 MB, with workload binaries in /root/)
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
set(CONFIG_DIR     ${KERNEL_DIR}/config)
set(OVERLAY_DIR    ${KERNEL_DIR}/overlay)
# Output goes to bb-tests/output/kernel/ (consistent with other bb-tests outputs)
set(OUTPUT_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../output/kernel)

set(CROSS_COMPILE  riscv64-unknown-linux-gnu-)
set(ARCH           riscv)
set(NPROC          4)

set(KERNEL_IMAGE  ${OUTPUT_DIR}/pegasus-bin)
set(ROOTFS_IMAGE  ${OUTPUT_DIR}/pegasus.img)
set(BUSYBOX_BIN   ${CMAKE_BINARY_DIR}/busybox/busybox)
set(ROOTFS_DIR    ${CMAKE_BINARY_DIR}/rootfs)

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

# ── linux kernel ──────────────────────────────────────────────────────────────
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
    DEPENDS ${CONFIG_DIR}/linux-config
    COMMENT "Building Linux kernel (RISC-V)"
    VERBATIM
)

add_custom_target(linux-build DEPENDS ${LINUX_IMAGE_PATH})

# ── rootfs assembly ───────────────────────────────────────────────────────────
# WORKLOADS_OUTPUT_DIR: where CTest *-linux binaries land after bbdev workload --build
set(WORKLOADS_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../output/workloads)

add_custom_command(
    OUTPUT  ${ROOTFS_IMAGE}
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
    # Copy all *-linux workload binaries into /root/ (consistent with firemarshal convention)
    COMMAND bash ${KERNEL_DIR}/scripts/install-workloads.sh
            ${WORKLOADS_OUTPUT_DIR} ${ROOTFS_DIR}/root
    # Pack into ext4 image
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
    COMMAND bash -c "dd if=/dev/zero of=${ROOTFS_IMAGE} bs=1M count=128 status=none"
    COMMAND mke2fs -t ext4 -d ${ROOTFS_DIR} -L pegasus-rootfs -F ${ROOTFS_IMAGE}
    DEPENDS ${BUSYBOX_BIN} ${OVERLAY_DIR}/init
    COMMENT "Assembling ext4 rootfs (with workload binaries in /root/)"
    VERBATIM
)

# ── kernel image copy ─────────────────────────────────────────────────────────
add_custom_command(
    OUTPUT  ${KERNEL_IMAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${LINUX_IMAGE_PATH} ${KERNEL_IMAGE}
    DEPENDS ${LINUX_IMAGE_PATH}
    COMMENT "Copying kernel Image to output"
    VERBATIM
)

# ── top-level target ──────────────────────────────────────────────────────────
add_custom_target(kernel-build
    DEPENDS ${KERNEL_IMAGE} ${ROOTFS_IMAGE}
    COMMENT "Pegasus kernel + rootfs ready in ${OUTPUT_DIR}"
)
