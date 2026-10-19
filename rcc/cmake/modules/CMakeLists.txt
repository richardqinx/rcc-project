# SPDX-License-Identifier: GPL-3.0-or-later

include(CMakePackageConfigHelpers)

get_directory_property(RCC_DEFINITIONS DIRECTORY "${RCC_SOURCE_DIR}" COMPILE_DEFINITIONS)
set(RCC_PACKAGE_MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(RCC_PACKAGE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(rccPackageDir "${RCC_BINARY_DIR}/lib/cmake/RCC")
set(rccInstallPackageDir "${CMAKE_INSTALL_LIBDIR}/cmake/RCC")
set(RCC_PACKAGE_MODULE_DIR "${RCC_CMAKE_DIR}")
set(RCC_PACKAGE_INCLUDE_DIRS "${RCC_INCLUDE_DIRS}")
set(RCC_PACKAGE_LIBRARY_DIR "${RCC_LIBRARY_DIR}")
set(RCC_PACKAGE_BUILD_LIBRARY_DIR "${RCC_LIBRARY_DIR}")
set(RCC_PACKAGE_INSTALL_PREFIX "")
configure_package_config_file(RCCConfig.cmake.in "${rccPackageDir}/RCCConfig.cmake"
  INSTALL_DESTINATION "${rccPackageDir}"
  NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
export(EXPORT RCCTargets FILE "${rccPackageDir}/RCCTargets.cmake")

set(RCC_PACKAGE_MODULE_DIR "\${CMAKE_CURRENT_LIST_DIR}")
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
  set(RCC_PACKAGE_INCLUDE_DIRS "${CMAKE_INSTALL_INCLUDEDIR}")
else()
  set(RCC_PACKAGE_INCLUDE_DIRS "\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(RCC_PACKAGE_LIBRARY_DIR "${CMAKE_INSTALL_LIBDIR}")
else()
  set(RCC_PACKAGE_LIBRARY_DIR "\${PACKAGE_PREFIX_DIR}/${CMAKE_INSTALL_LIBDIR}")
endif()
set(RCC_PACKAGE_BUILD_LIBRARY_DIR "")
set(RCC_PACKAGE_INSTALL_PREFIX "\${PACKAGE_PREFIX_DIR}")
configure_package_config_file(RCCConfig.cmake.in RCCConfig.install.cmake
  INSTALL_DESTINATION "${rccInstallPackageDir}"
  NO_SET_AND_CHECK_MACRO NO_CHECK_REQUIRED_COMPONENTS_MACRO)
configure_file(RCCConfigVersion.cmake.in
  "${rccPackageDir}/RCCConfigVersion.cmake" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RCCConfig.install.cmake"
  DESTINATION "${rccInstallPackageDir}" RENAME RCCConfig.cmake)
install(FILES
  "${rccPackageDir}/RCCConfigVersion.cmake"
  AddRCC.cmake HandleRCCOptions.cmake
  DESTINATION "${rccInstallPackageDir}")
install(EXPORT RCCTargets FILE RCCTargets.cmake
  DESTINATION "${rccInstallPackageDir}")
