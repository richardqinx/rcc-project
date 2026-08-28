// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_IR_PASSINSTRUMENTATION_H
#define RCC_IR_PASSINSTRUMENTATION_H

#include "rcc/Support/Error.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {

enum class InstrumentationPhase : std::uint8_t {
  Before,
  After,
};

template <typename IRUnitT, typename PassResultT> class PassInstrumentationCallbacks final {
public:
  using BeforeCallback = std::function<void(std::string_view, const IRUnitT &)>;
  using AfterCallback = std::function<void(std::string_view, const IRUnitT &, const PassResultT &)>;
  using FailureCallback = std::function<void(std::string_view, const IRUnitT &, const Error &)>;
  using BeforeAnalysisCallback = std::function<void(std::string_view, const IRUnitT &)>;
  using AfterAnalysisCallback =
      std::function<void(std::string_view, const IRUnitT &, bool, std::uint64_t)>;
  using AnalysisFailureCallback =
      std::function<void(std::string_view, const IRUnitT &, const Error &)>;

  void addBeforeCallback(BeforeCallback Callback) {
    addCallback(Before, std::move(Callback));
  }

  void addAfterCallback(AfterCallback Callback) {
    addCallback(After, std::move(Callback));
  }

  void addFailureCallback(FailureCallback Callback) {
    addCallback(Failure, std::move(Callback));
  }

  void addBeforeAnalysisCallback(BeforeAnalysisCallback Callback) {
    addCallback(BeforeAnalysis, std::move(Callback));
  }

  void addAfterAnalysisCallback(AfterAnalysisCallback Callback) {
    addCallback(AfterAnalysis, std::move(Callback));
  }

  void addAnalysisFailureCallback(AnalysisFailureCallback Callback) {
    addCallback(AnalysisFailure, std::move(Callback));
  }

  void runBefore(std::string_view Name, const IRUnitT &Unit) const {
    for (const BeforeCallback &Callback : snapshot(Before))
      Callback(Name, Unit);
  }

  void runAfter(std::string_view Name, const IRUnitT &Unit, const PassResultT &Result) const {
    for (const AfterCallback &Callback : snapshot(After))
      Callback(Name, Unit, Result);
  }

  void runFailure(std::string_view Name, const IRUnitT &Unit, const Error &FailureValue) const {
    for (const FailureCallback &Callback : snapshot(Failure))
      Callback(Name, Unit, FailureValue);
  }

  void runBeforeAnalysis(std::string_view Name, const IRUnitT &Unit) const {
    for (const BeforeAnalysisCallback &Callback : snapshot(BeforeAnalysis))
      Callback(Name, Unit);
  }

  void runAfterAnalysis(std::string_view Name, const IRUnitT &Unit, bool CacheHit,
                        std::uint64_t Nanoseconds) const {
    for (const AfterAnalysisCallback &Callback : snapshot(AfterAnalysis))
      Callback(Name, Unit, CacheHit, Nanoseconds);
  }

  void runAnalysisFailure(std::string_view Name, const IRUnitT &Unit,
                          const Error &FailureValue) const {
    for (const AnalysisFailureCallback &Callback : snapshot(AnalysisFailure))
      Callback(Name, Unit, FailureValue);
  }

  [[nodiscard]] bool empty() const {
    std::scoped_lock Lock(Mutex);
    return Before.empty() && After.empty() && Failure.empty() && BeforeAnalysis.empty() &&
           AfterAnalysis.empty() && AnalysisFailure.empty();
  }

private:
  template <typename CallbackT>
  void addCallback(std::vector<CallbackT> &Callbacks, CallbackT Callback) {
    if (!Callback)
      return;
    std::scoped_lock Lock(Mutex);
    Callbacks.push_back(std::move(Callback));
  }

  template <typename CallbackT>
  [[nodiscard]] std::vector<CallbackT> snapshot(const std::vector<CallbackT> &Callbacks) const {
    std::scoped_lock Lock(Mutex);
    return Callbacks;
  }

  mutable std::mutex Mutex;
  std::vector<BeforeCallback> Before;
  std::vector<AfterCallback> After;
  std::vector<FailureCallback> Failure;
  std::vector<BeforeAnalysisCallback> BeforeAnalysis;
  std::vector<AfterAnalysisCallback> AfterAnalysis;
  std::vector<AnalysisFailureCallback> AnalysisFailure;
};

} // namespace rcc

#endif // RCC_IR_PASSINSTRUMENTATION_H
