# SPDX-License-Identifier: GPL-3.0-or-later

foreach(rccPolicy CMP0141 CMP0144 CMP0147 CMP0156 CMP0182 CMP0164)
  if(POLICY ${rccPolicy})
    cmake_policy(SET ${rccPolicy} NEW)
  endif()
endforeach()

if(POLICY CMP0156)
  if(POLICY CMP0179)
    cmake_policy(SET CMP0179 NEW)
  endif()
endif()
