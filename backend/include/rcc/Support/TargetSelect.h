// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_SUPPORT_TARGETSELECT_H
#define RCC_SUPPORT_TARGETSELECT_H

namespace rcc {

#define RCC_TARGET(TargetName) void initialize##TargetName##Target();
#include "rcc/Config/Targets.def"

inline void initializeAllTargets() {
#define RCC_TARGET(TargetName) initialize##TargetName##Target();
#include "rcc/Config/Targets.def"
}

} // namespace rcc

#endif // RCC_SUPPORT_TARGETSELECT_H
