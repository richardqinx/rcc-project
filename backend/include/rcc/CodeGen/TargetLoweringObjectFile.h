// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETLOWERINGOBJECTFILE_H
#define RCC_CODEGEN_TARGETLOWERINGOBJECTFILE_H

#include "rcc/MC/MCSection.h"
#include "rcc/Support/Error.h"

namespace rcc {

struct GlobalObjectDesc;
class MachineConstantPoolEntry;
class MachineFunction;
class MachineJumpTableEntry;
class MachineMCContext;

class TargetLoweringObjectFile {
public:
  virtual ~TargetLoweringObjectFile() = default;

  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  textSection(const MachineFunction &Function) const = 0;
  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  globalSection(const GlobalObjectDesc &Object, MachineMCContext &Context) const = 0;
  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  constantPoolSection(const MachineFunction &Function,
                      const MachineConstantPoolEntry &Entry) const = 0;
  [[nodiscard]] virtual Expected<mc::MCSectionDesc>
  jumpTableSection(const MachineFunction &Function, const MachineJumpTableEntry &Entry) const = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETLOWERINGOBJECTFILE_H
