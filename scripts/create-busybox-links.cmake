if(NOT DEFINED ROOTFS_DIR)
  message(FATAL_ERROR "ROOTFS_DIR is required")
endif()

set(applets
  sh ash ls cat echo mount umount mkdir rm cp mv ps top free sleep poweroff)

foreach(app IN LISTS applets)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink busybox ${ROOTFS_DIR}/bin/${app}
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to create busybox applet link: ${app}")
  endif()
endforeach()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E create_symlink ../bin/busybox ${ROOTFS_DIR}/sbin/init
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Failed to create /sbin/init link")
endif()
