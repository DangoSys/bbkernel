# bbkernel — RISC-V Linux kernel + rootfs builder
#
# Submodules (in stack/os/thirdparty/):
#   linux/    — firesim/linux v6.6 fork
#   busybox/  — busybox 1.36
#   opensbi/  — OpenSBI firmware
#
# Output (in examples/designs/<design>/output/sims/device/workloads/kernel/):
#   fw_payload*.bin — OpenSBI + Linux + initramfs (complete boot image)
#
cmake_minimum_required(VERSION 3.26)
project(bbkernel NONE)

set(KERNEL_DIR     ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(REPO_ROOT ${KERNEL_DIR}/../../.. ABSOLUTE)
set(THIRDPARTY_DIR ${KERNEL_DIR}/../thirdparty)
set(LINUX_DIR      ${THIRDPARTY_DIR}/linux)
set(BUSYBOX_DIR    ${THIRDPARTY_DIR}/busybox)
set(OPENSBI_DIR    ${THIRDPARTY_DIR}/opensbi)
set(CONFIG_DIR     ${KERNEL_DIR}/config)
set(SHARED_OVERLAY_DIR ${KERNEL_DIR}/overlay)
set(PLATFORM_DIR   ${KERNEL_DIR}/opensbi-platform)
set(OUTPUT_DIR ${BUCKYBALL_KERNEL_OUTPUT}/workloads/kernel)

set(CROSS_COMPILE  riscv64-unknown-linux-gnu-)
set(ARCH           riscv)
set(NPROC          100)

set(BUCKYBALL_HART_COUNT 64 CACHE STRING "Linux-visible Buckyball hart count")
if(NOT BUCKYBALL_HART_COUNT MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "BUCKYBALL_HART_COUNT must be a positive integer")
endif()
set(BUCKYBALL_GUEST_MEMORY_MIB 512 CACHE STRING "Linux guest memory budget in MiB")
math(EXPR BUCKYBALL_GUEST_MEMORY_BYTES "${BUCKYBALL_GUEST_MEMORY_MIB} * 1048576")
set(BUCKYBALL_FDT_SIZE 262144)
math(EXPR BUCKYBALL_FDT_ADDR "2147483648 + ${BUCKYBALL_GUEST_MEMORY_BYTES} - ${BUCKYBALL_FDT_SIZE}")
set(BUCKYBALL_KERNEL_DESIGN "" CACHE STRING "Design whose Linux workloads the rootfs carries")
set(BUCKYBALL_KERNEL_OUTPUT "" CACHE PATH
  "Output root of the simulation the kernel is built for: output/sims/<selection> of the design")
set(BUCKYBALL_KERNEL_WORKLOADS "" CACHE PATH
  "Directory whose CMakeLists.txt sets BUCKYBALL_KERNEL_PROGRAMS: hart, program pairs installed into the rootfs")
set(BUCKYBALL_MODEL_BASE "" CACHE STRING "Physical address of the model region of the DDR")
option(BUCKYBALL_KERNEL_INTERACTIVE
  "Keep shared /init shell; do not auto-run design workloads" OFF)

# OUTPUT_DIR belongs to one device config, so the image name is fixed.
set(FW_PAYLOAD_NAME fw_payload-linux)
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
set(WORKLOADS_OUTPUT_DIR ${BUCKYBALL_KERNEL_OUTPUT}/workloads)
set(INSTALL_WORKLOADS_SCRIPT ${KERNEL_DIR}/scripts/install-workloads.sh)
set(USER_ALLOWLIST ${CMAKE_BINARY_DIR}/user-workloads.allowlist)
set(USER_BINDINGS ${CMAKE_BINARY_DIR}/user-workloads.tsv)
set(DESIGN_INIT "")

if(BUCKYBALL_KERNEL_DESIGN)
  # Linux regression mode: the programs the entry lists, each
  # pinned to its hart by init-workloads.
  set(DESIGN_INIT ${CMAKE_BINARY_DIR}/init-workloads)
  configure_file(${SHARED_OVERLAY_DIR}/init-workloads ${DESIGN_INIT} @ONLY)
  add_subdirectory("${BUCKYBALL_KERNEL_WORKLOADS}" kernel-workloads)
  set(ALLOWLIST "")
  set(BINDINGS "")
  while(BUCKYBALL_KERNEL_PROGRAMS)
    list(POP_FRONT BUCKYBALL_KERNEL_PROGRAMS HART PROGRAM)
    string(APPEND ALLOWLIST "${PROGRAM}\n")
    string(APPEND BINDINGS "${HART}\t${PROGRAM}\n")
  endwhile()
  file(CONFIGURE OUTPUT ${USER_ALLOWLIST} CONTENT "${ALLOWLIST}")
  file(CONFIGURE OUTPUT ${USER_BINDINGS} CONTENT "${BINDINGS}")
endif()

if(BUCKYBALL_KERNEL_DESIGN)
  set(INSTALL_RUNTIME_COMMAND
      bash ${INSTALL_WORKLOADS_SCRIPT}
      ${WORKLOADS_OUTPUT_DIR}
      ${ROOTFS_DIR}/root
      ${USER_ALLOWLIST})
else()
  set(INSTALL_RUNTIME_COMMAND
      bash ${INSTALL_WORKLOADS_SCRIPT}
      ${WORKLOADS_OUTPUT_DIR}
      ${ROOTFS_DIR}/root)
endif()

set(OVERLAY_COPY_COMMAND
    bash -c "cp -r ${SHARED_OVERLAY_DIR}/. ${ROOTFS_DIR}/")
set(ROOTFS_EXTRA_DEPENDS ${SHARED_OVERLAY_DIR}/init)
if(BUCKYBALL_KERNEL_DESIGN AND NOT BUCKYBALL_KERNEL_INTERACTIVE)
  set(OVERLAY_COPY_COMMAND
      bash -c "cp -r ${SHARED_OVERLAY_DIR}/. ${ROOTFS_DIR}/ && cp ${DESIGN_INIT} ${ROOTFS_DIR}/init && cp ${USER_BINDINGS} ${ROOTFS_DIR}/root/workload-harts.tsv")
  list(APPEND ROOTFS_EXTRA_DEPENDS
       ${DESIGN_INIT}
       ${USER_ALLOWLIST}
       ${USER_BINDINGS}
       ${BUCKYBALL_KERNEL_WORKLOADS}/CMakeLists.txt)
elseif(BUCKYBALL_KERNEL_DESIGN)
  list(APPEND ROOTFS_EXTRA_DEPENDS
       ${USER_ALLOWLIST}
       ${USER_BINDINGS}
       ${BUCKYBALL_KERNEL_WORKLOADS}/CMakeLists.txt)
endif()
# ── dynamic runtime and the model tools ──────────────────────────────────────
# buddy-cli loads the model's runner with dlopen, so the rootfs carries the
# dynamic loader and the libraries the tools and runners link. All come from
# the toolchain that built them. A model itself is not in the rootfs: it is
# in the model region of the DDR, and model-run starts it.
execute_process(COMMAND ${CROSS_COMPILE}g++ -print-file-name=libstdc++.so.6
                OUTPUT_VARIABLE LIBSTDCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CROSS_COMPILE}g++ -print-file-name=libgcc_s.so.1
                OUTPUT_VARIABLE LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CROSS_COMPILE}g++ -print-file-name=libc.so.6
                OUTPUT_VARIABLE LIBC OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(LIBC_REAL ${LIBC} REALPATH)
get_filename_component(GLIBC_LIB ${LIBC_REAL} DIRECTORY)
set(RUNTIME_LIBS ${LIBSTDCXX} ${LIBGCC}
    ${GLIBC_LIB}/libc.so.6 ${GLIBC_LIB}/libm.so.6 ${GLIBC_LIB}/libdl.so.2
    ${GLIBC_LIB}/libpthread.so.0 ${GLIBC_LIB}/ld-linux-riscv64-lp64d.so.1)
set(RUNTIME_LIB_COMMANDS COMMAND ${CMAKE_COMMAND} -E make_directory ${ROOTFS_DIR}/lib)
foreach(LIB IN LISTS RUNTIME_LIBS)
  list(APPEND RUNTIME_LIB_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy ${LIB} ${ROOTFS_DIR}/lib/)
endforeach()

# The model tools are built by the model build (models/CMakeLists.txt) for the
# device they run on; the rootfs takes them from there.
set(BUDDY_CLI ${CMAKE_BINARY_DIR}/buddy-cli-rv/buddy-cli)
set(MODEL_RUN ${CMAKE_BINARY_DIR}/model-run)
add_custom_command(
    OUTPUT  ${MODEL_RUN}
    COMMAND ${CROSS_COMPILE}g++ -std=c++20 -O2 -I${REPO_ROOT}/stack/runtime/services
            -o ${MODEL_RUN} ${KERNEL_DIR}/tools/model-run.cpp
            -Wl,--dynamic-linker=/lib/ld-linux-riscv64-lp64d.so.1
    COMMAND patchelf --set-rpath /lib ${MODEL_RUN}
    DEPENDS ${KERNEL_DIR}/tools/model-run.cpp ${REPO_ROOT}/stack/runtime/services/resources.h
    VERBATIM
)

set(BUDDY_SOURCE_DIR ${REPO_ROOT}/stack/thirdparty/buddy-mlir)
# buddy-cli for the RISC-V Linux that runs a model: the program that loads a
# model's runner. It links no MLIR, but buddy-mlir's build needs one for the
# target. The interpreter is the rootfs's own; libraries are not searched for
# in the toolchain's store.
set(RISCV_BUDDY ${CMAKE_BINARY_DIR}/buddy-cli-rv)
add_custom_command(
  OUTPUT ${RISCV_BUDDY}/buddy-cli
  COMMAND ${CMAKE_COMMAND} -G Ninja -S ${BUDDY_SOURCE_DIR}/llvm/llvm -B ${RISCV_BUDDY}/mlir
          -DLLVM_ENABLE_PROJECTS=mlir -DLLVM_TARGETS_TO_BUILD=RISCV
          -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
          -DLLVM_INCLUDE_BENCHMARKS=OFF -DMLIR_INCLUDE_TESTS=OFF
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64 -DCMAKE_CROSSCOMPILING=ON
          -DLLVM_HOST_TRIPLE=riscv64-unknown-linux-gnu
          -DLLVM_DEFAULT_TARGET_TRIPLE=riscv64-unknown-linux-gnu
          -DCMAKE_C_COMPILER=riscv64-unknown-linux-gnu-gcc
          -DCMAKE_CXX_COMPILER=riscv64-unknown-linux-gnu-g++
          -DCMAKE_BUILD_TYPE=Release -DLLVM_NATIVE_TOOL_DIR=${BUDDY_SOURCE_DIR}/llvm/build/bin
          -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_TERMINFO=OFF
          -DLLVM_ENABLE_LIBXML2=OFF
  COMMAND ${CMAKE_COMMAND} -G Ninja -S ${BUDDY_SOURCE_DIR} -B ${RISCV_BUDDY}/buddy
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64 -DCMAKE_BUILD_TYPE=Release
          -DCMAKE_C_COMPILER=riscv64-unknown-linux-gnu-gcc
          -DCMAKE_CXX_COMPILER=riscv64-unknown-linux-gnu-g++
          -DMLIR_DIR=${RISCV_BUDDY}/mlir/lib/cmake/mlir -DLLVM_DIR=${RISCV_BUDDY}/mlir/lib/cmake/llvm
          -DBUDDY_ENABLE_TESTS=OFF -DFLATC_EXECUTABLE=${REPO_ROOT}/result/bin/flatc
          -DBUDDY_FLATBUFFERS_INCLUDE_HINTS=${REPO_ROOT}/result/include
          "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--dynamic-linker=/lib/ld-linux-riscv64-lp64d.so.1"
  COMMAND ${CMAKE_COMMAND} --build ${RISCV_BUDDY}/buddy --target buddy-cli
  COMMAND ${CMAKE_COMMAND} -E copy ${RISCV_BUDDY}/buddy/bin/buddy-cli ${RISCV_BUDDY}/buddy-cli
  COMMAND patchelf --set-rpath /lib ${RISCV_BUDDY}/buddy-cli
  COMMENT "Building the RISC-V buddy-cli"
  VERBATIM
)
add_custom_target(riscv-buddy-cli DEPENDS ${RISCV_BUDDY}/buddy-cli)


set(BUSYBOX_APPLETS
    sh ash ls cat echo mount umount mkdir rm cp mv ps top free sleep poweroff reboot)
set(BUSYBOX_LINK_COMMANDS)
foreach(APPLET IN LISTS BUSYBOX_APPLETS)
  list(APPEND BUSYBOX_LINK_COMMANDS
       COMMAND ${CMAKE_COMMAND} -E create_symlink busybox ${ROOTFS_DIR}/bin/${APPLET})
endforeach()

# The workload binaries the rootfs carries are found by name when it is built,
# so CMake cannot track them: the rootfs is rebuilt on every kernel build.
add_custom_target(rootfs-build
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
    ${BUSYBOX_LINK_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E create_symlink ../bin/busybox ${ROOTFS_DIR}/sbin/init
    # Shared overlay, then optional per-design OS overlay (init, etc.)
    COMMAND ${OVERLAY_COPY_COMMAND}
    COMMAND chmod +x ${ROOTFS_DIR}/init
    ${RUNTIME_LIB_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E copy ${MODEL_RUN} ${ROOTFS_DIR}/bin/model-run
    COMMAND ${CMAKE_COMMAND} -E copy ${BUDDY_CLI} ${ROOTFS_DIR}/bin/buddy-cli
    # Copy workload binaries.
    COMMAND ${INSTALL_RUNTIME_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E touch ${ROOTFS_STAMP}
    BYPRODUCTS ${ROOTFS_STAMP}
    DEPENDS ${BUSYBOX_BIN} ${MODEL_RUN} ${BUDDY_CLI}
            ${INITRAMFS_DEVNODES}
            ${INSTALL_WORKLOADS_SCRIPT}
            ${ROOTFS_EXTRA_DEPENDS}
    COMMENT "Creating initramfs source tree"
    VERBATIM
)


# ── linux kernel with initramfs ───────────────────────────────────────────────
set(LINUX_BUILD_DIR ${CMAKE_BINARY_DIR}/linux)
set(LINUX_IMAGE_PATH ${LINUX_BUILD_DIR}/arch/riscv/boot/Image)
file(READ ${CONFIG_DIR}/linux-config KERNEL_CONFIG)
if(BUCKYBALL_HART_COUNT GREATER 1)
  string(REGEX REPLACE "CONFIG_NR_CPUS=[0-9]+" "CONFIG_NR_CPUS=${BUCKYBALL_HART_COUNT}" KERNEL_CONFIG "${KERNEL_CONFIG}")
  string(APPEND KERNEL_CONFIG "\nCONFIG_SMP=y\n")
else()
  string(APPEND KERNEL_CONFIG "\n# CONFIG_SMP is not set\n")
endif()
string(REPLACE "mem=512M" "mem=${BUCKYBALL_GUEST_MEMORY_MIB}M" KERNEL_CONFIG "${KERNEL_CONFIG}")
file(WRITE ${CMAKE_BINARY_DIR}/linux-config "${KERNEL_CONFIG}")

add_custom_command(
    OUTPUT  ${LINUX_IMAGE_PATH}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LINUX_BUILD_DIR}
    COMMAND make -C ${LINUX_DIR}
            ARCH=${ARCH}
            CROSS_COMPILE=${CROSS_COMPILE}
            O=${LINUX_BUILD_DIR}
            defconfig
    COMMAND ${LINUX_DIR}/scripts/kconfig/merge_config.sh -m
            -O ${LINUX_BUILD_DIR}
            ${LINUX_BUILD_DIR}/.config
            ${CMAKE_BINARY_DIR}/linux-config
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
    DEPENDS ${CMAKE_BINARY_DIR}/linux-config ${ROOTFS_STAMP} ${INITRAMFS_DEVNODES}
    COMMENT "Building Linux kernel with initramfs"
    VERBATIM
)

add_custom_target(linux-build DEPENDS ${LINUX_IMAGE_PATH})

# ── device tree ──────────────────────────────────────────────────────────────
# The BootROM passes no DTB: OpenSBI takes this one, generated from the design PB, from its payload.
# The workload build also configures this project without a design; only fw_payload needs one.
unset(BUCKYBALL_TIMEBASE_HZ CACHE)
# Match Chipyard's 500 MHz logical clock / 1000 RTC divider, not FPGA wall time.
set(BUCKYBALL_TIMEBASE_HZ 500000)
if(BUCKYBALL_KERNEL_DESIGN)
  find_program(PYTHON3 python3 REQUIRED)
  find_program(DTC dtc REQUIRED)
  set(DESIGN_PB  ${BUCKYBALL_KERNEL_OUTPUT}/generated/design.pb)
  set(DESIGN_DTS ${CMAKE_BINARY_DIR}/${BUCKYBALL_KERNEL_DESIGN}.dts)
  set(DESIGN_DTB ${CMAKE_BINARY_DIR}/${BUCKYBALL_KERNEL_DESIGN}.dtb)
  add_custom_command(
      OUTPUT  ${DESIGN_DTB}
      COMMAND ${PYTHON3} ${KERNEL_DIR}/scripts/design-dts.py
              --repo ${REPO_ROOT}
              --design-pb ${DESIGN_PB}
              --guest-memory-mib ${BUCKYBALL_GUEST_MEMORY_MIB}
              --timebase-hz ${BUCKYBALL_TIMEBASE_HZ}
              --output ${DESIGN_DTS}
      COMMAND ${DTC} -I dts -O dtb -o ${DESIGN_DTB} ${DESIGN_DTS}
      DEPENDS ${DESIGN_PB} ${KERNEL_DIR}/scripts/design-dts.py
      COMMENT "Generating the ${BUCKYBALL_KERNEL_DESIGN} device tree"
      VERBATIM
  )
else()
  set(DESIGN_DTB ${CMAKE_BINARY_DIR}/no-design.dtb)
  add_custom_command(
      OUTPUT  ${DESIGN_DTB}
      COMMAND ${CMAKE_COMMAND} -E echo "fw_payload needs BUCKYBALL_KERNEL_DESIGN to generate its device tree"
      COMMAND ${CMAKE_COMMAND} -E false
      VERBATIM
  )
endif()

# ── opensbi fw_payload ───────────────────────────────────────────────────────
set(OPENSBI_BUILD_DIR ${CMAKE_BINARY_DIR}/opensbi)
set(OPENSBI_FW_PATH ${OPENSBI_BUILD_DIR}/platform/buckyball/firmware/fw_payload.bin)
set(OPENSBI_PLATFORM_CONFIG ${CMAKE_BINARY_DIR}/opensbi-platform.config)
file(GENERATE OUTPUT ${OPENSBI_PLATFORM_CONFIG} CONTENT
  "harts=${BUCKYBALL_HART_COUNT}\nguest_bytes=${BUCKYBALL_GUEST_MEMORY_BYTES}\nfdt_addr=${BUCKYBALL_FDT_ADDR}\nfdt_size=${BUCKYBALL_FDT_SIZE}\n")
set(OPENSBI_PLATFORM_SOURCES
    ${PLATFORM_DIR}/buckyball/Kconfig
    ${PLATFORM_DIR}/buckyball/objects.mk
    ${PLATFORM_DIR}/buckyball/platform.c)

add_custom_command(
    OUTPUT  ${OPENSBI_FW_PATH}
    COMMAND make -B -j${NPROC} -C ${OPENSBI_DIR}
            CROSS_COMPILE=${CROSS_COMPILE}
            PLATFORM_DIR=${PLATFORM_DIR}
            PLATFORM=buckyball
            O=${OPENSBI_BUILD_DIR}
            FW_PAYLOAD_PATH=${LINUX_IMAGE_PATH}
            FW_FDT_PATH=${DESIGN_DTB}
            BUCKYBALL_HART_COUNT=${BUCKYBALL_HART_COUNT}
            BUCKYBALL_GUEST_MEMORY_BYTES=${BUCKYBALL_GUEST_MEMORY_BYTES}
            BUCKYBALL_FDT_ADDR=${BUCKYBALL_FDT_ADDR}
            BUCKYBALL_FDT_SIZE=${BUCKYBALL_FDT_SIZE}
    DEPENDS ${LINUX_IMAGE_PATH} ${DESIGN_DTB} ${OPENSBI_PLATFORM_SOURCES} ${OPENSBI_PLATFORM_CONFIG}
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

# ── DDR image tool ───────────────────────────────────────────────────────────
# A host tool: bbdev runs it to lay the kernel and the model into one DDR image.
add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/image-pack
    COMMAND g++ -std=c++20 -O2 -o ${CMAKE_BINARY_DIR}/image-pack ${KERNEL_DIR}/tools/image-pack.cpp
    DEPENDS ${KERNEL_DIR}/tools/image-pack.cpp
    VERBATIM
)

# ── top-level target ──────────────────────────────────────────────────────────
add_custom_target(kernel-build
    DEPENDS ${OPENSBI_FW} ${CMAKE_BINARY_DIR}/image-pack
    COMMENT "OpenSBI fw_payload ready at ${OPENSBI_FW}"
)
