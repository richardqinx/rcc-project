# SPDX-License-Identifier: GPL-3.0-or-later

include(CMakePackageConfigHelpers)

set(cfrontPackageDir "${CFRONT_BINARY_DIR}/lib/cmake/CFront")
set(cfrontInstallPackageDir "${CMAKE_INSTALL_LIBDIR}/cmake/CFront")
set(CFRONT_PACKAGE_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(CFRONT_STANDALONE_BUILD)
  set(CFRONT_PACKAGE_RCC_CMAKE_DIR "${RCC_DIR}")
else()
  set(CFRONT_PACKAGE_RCC_CMAKE_DIR "${RCC_BINARY_DIR}/lib/cmake/RCC")
endif()
configure_package_config_file(CFrontConfig.cmake.in
  "${cfrontPackageDir}/CFrontConfig.cmake"
  INSTALL_DESTINATION "${cfrontPackageDir}"
  NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
export(EXPORT CFrontTargets FILE "${cfrontPackageDir}/CFrontTargets.cmake")

set(CFRONT_PACKAGE_MODULE_DIR "\${CMAKE_CURRENT_LIST_DIR}")
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(CFRONT_PACKAGE_RCC_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/RCC")
else()
  set(CFRONT_PACKAGE_RCC_CMAKE_DIR "\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}/cmake/RCC")
endif()
configure_package_config_file(CFrontConfig.cmake.in CFrontConfig.install.cmake
  INSTALL_DESTINATION "${cfrontInstallPackageDir}"
  NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
configure_file(CFrontConfigVersion.cmake.in
  "${cfrontPackageDir}/CFrontConfigVersion.cmake" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CFrontConfig.install.cmake"
  DESTINATION "${cfrontInstallPackageDir}" RENAME CFrontConfig.cmake)
install(FILES "${cfrontPackageDir}/CFrontConfigVersion.cmake" AddCFront.cmake
  DESTINATION "${cfrontInstallPackageDir}")
install(EXPORT CFrontTargets FILE CFrontTargets.cmake
  DESTINATION "${cfrontInstallPackageDir}")
