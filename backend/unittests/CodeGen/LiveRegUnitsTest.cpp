// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveRegUnits.h"

#include "LiveTestSupport.h"

namespace {

using namespace rcc::test::liveness;

void testLiveRegUnits(TestContext &Context) {
  const rcc::RegisterUnitID Unit = rcc::RegisterUnitID::fromValue(9);
  rcc::LiveRegUnits Present;
  rcc::LiveRegUnits Removed;
  Present.add(Unit, rcc::LaneMask::fromRaw(0x3));
  Removed.add(Unit, rcc::LaneMask::fromRaw(0x2));
  Present.subtract(Removed);
  Context.expect(Present.lanes(Unit) == rcc::LaneMask::fromRaw(0x1) &&
                     Present.contains(Unit, rcc::LaneMask::fromRaw(0x1)) &&
                     !Present.intersects(Unit, rcc::LaneMask::fromRaw(0x2)),
                 "LiveRegUnits must preserve partial union and subtraction");
  Present.unite(Removed);
  Context.expect(Present.contains(Unit, rcc::LaneMask::fromRaw(0x3)),
                 "LiveRegUnits union must restore all selected lanes");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();
  testLiveRegUnits(Context);
  return Context.result();
}
