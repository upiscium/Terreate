#include <terreate/graphics/retirement.hpp>

#include <exception>
#include <new>
#include <string>
#include <vector>

namespace terreate::graphics {
namespace {
class RetirementErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.retirement"; }
  std::string message(int code) const override {
    switch (static_cast<RetirementError>(code)) {
    case RetirementError::invalid_resource:
      return "invalid or moved-from resource";
    case RetirementError::missing_completion:
      return "nonempty explicit completion set required";
    case RetirementError::invalid_completion:
      return "invalid borrowed submission completion";
    case RetirementError::device_mismatch:
      return "all declared submissions must belong to one logical Device";
    case RetirementError::pending_entries:
      return "retirement queue still owns pending resources";
    case RetirementError::device_loss_not_observed:
      return "native device loss must be observed before explicit reclaim";
    case RetirementError::device_lost:
      return "retirement queue's logical Device is lost";
    case RetirementError::allocation_failed:
      return "retirement metadata allocation failed";
    }
    return "unknown retirement error";
  }
};
} // namespace

const std::error_category &retirement_error_category() noexcept {
  static const RetirementErrorCategory category;
  return category;
}
std::error_code make_error_code(RetirementError error) noexcept {
  return {static_cast<int>(error), retirement_error_category()};
}
terreate::Result<void> RetirementQueue::fail(RetirementError error) {
  return std::unexpected(terreate::Error{make_error_code(error), "retirement",
                                         "operation rejected without changing ownership"});
}
RetirementQueue::~RetirementQueue() noexcept {
  if (!entries_.empty())
    std::terminate();
}
terreate::Result<void> RetirementQueue::requireEmpty() const {
  if (!entries_.empty())
    return fail(RetirementError::pending_entries);
  return {};
}
terreate::Result<CollectReport> RetirementQueue::collect() {
  if (lost_) {
    auto error = fail(RetirementError::device_lost);
    return std::unexpected(error.error());
  }
  std::vector<bool> ready;
  try {
    ready.resize(entries_.size(), false);
  } catch (const std::bad_alloc &) {
    auto error = fail(RetirementError::allocation_failed);
    return std::unexpected(error.error());
  }
  // Two phases: a failed native poll preserves every owner and its conditions.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    bool all = true;
    for (const auto &receipt : entries_[i].receipts) {
      const auto observation = receipt.poll();
      if (!observation) {
        if (observation.error().code() == vk::make_error_code(vk::Result::eErrorDeviceLost))
          lost_ = true;
        return std::unexpected(observation.error());
      }
      if (observation->state != SubmissionCompletionState::completed)
        all = false;
    }
    ready[i] = all;
  }
  std::size_t collected = 0;
  // Different entries are independent. For dependent owners, callers must
  // retire one explicit RAII aggregate with ordered child/parent members.
  for (std::size_t i = entries_.size(); i > 0; --i) {
    if (ready[i - 1]) {
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i - 1));
      ++collected;
    }
  }
  if (entries_.empty())
    device_ = nullptr;
  return CollectReport{collected, entries_.size()};
}
terreate::Result<std::size_t> RetirementQueue::reclaimAfterDeviceLoss(const Device &device) {
  if (!lost_)
    return std::unexpected(
        terreate::Error{make_error_code(RetirementError::device_loss_not_observed),
                        "reclaimAfterDeviceLoss", "no native device loss observed"});
  if (&device != device_ || !device.valid())
    return std::unexpected(terreate::Error{make_error_code(RetirementError::device_mismatch),
                                           "reclaimAfterDeviceLoss",
                                           "must specify the original live lost Device"});
  const auto count = entries_.size();
  // Device loss does NOT mean normally completed GPU work. This is a distinct
  // Vulkan-lost-device exception, invoked explicitly under caller's teardown
  // synchronization, with all parent resources still alive.
  while (!entries_.empty())
    entries_.pop_back();
  device_ = nullptr;
  return count;
}
} // namespace terreate::graphics
