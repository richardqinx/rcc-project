# Determine if the compiler has GCC-compatible command-line syntax.

if(NOT DEFINED RCC_COMPILER_IS_GCC_COMPATIBLE)
  if(CMAKE_COMPILER_IS_GNUCXX)
    set(RCC_COMPILER_IS_GCC_COMPATIBLE ON)
  elseif( MSVC )
    set(RCC_COMPILER_IS_GCC_COMPATIBLE OFF)
  elseif( "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" )
    set(RCC_COMPILER_IS_GCC_COMPATIBLE ON)
  elseif( "${CMAKE_CXX_COMPILER_ID}" MATCHES "Intel" )
    set(RCC_COMPILER_IS_GCC_COMPATIBLE ON)
  endif()
endif()
