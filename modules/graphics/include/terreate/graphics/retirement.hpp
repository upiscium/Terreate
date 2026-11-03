#ifndef TERREATE_GRAPHICS_RETIREMENT_HPP
#define TERREATE_GRAPHICS_RETIREMENT_HPP

#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <terreate/graphics/submission.hpp>
#include <type_traits>
#include <utility>
#include <vector>

namespace terreate::graphics {

enum class RetirementError : std::uint8_t {
  invalid_resource = 1,
  missing_completion,
  invalid_completion,
  device_mismatch,
  pending_entries,
  device_loss_not_observed,
  device_lost,
  allocation_failed,
};

[[nodiscard]] const std::error_category &retirement_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(RetirementError error) noexcept;

struct CollectReport {
  std::size_t collected = 0;
  std::size_t pending = 0;
};

/// Caller-owned, explicit retirement queue. No inferred resource-use coverage,
/// scheduling, background work, synchronization, automatic waits or waitIdle.
/// Each entry holds one exclusive owner plus the explicit AND of all uses.
/// Only successful native SubmissionReceipts are accepted, never arbitrary
/// caller-reported Completion observations.
///
/// All receipts in this queue must borrow the same live Device wrapper at its
/// original address, including across multiple Queues of that Device. No cross-
/// device owner/receipt inference; all resource handles must belong to that
/// Device, and parents/synchronization objects outlive every queue operation.
///
/// Destruction of a nonempty queue is a fail-fast contract violation. Explicitly
/// collect all entries, or use reclaimAfterDeviceLoss after the queue itself
/// observes VK_ERROR_DEVICE_LOST. A pending destructor does not wait or free.
class RetirementQueue final {
public:
  RetirementQueue() = default;
  RetirementQueue(const RetirementQueue &) = delete;
  RetirementQueue &operator=(const RetirementQueue &) = delete;
  RetirementQueue(RetirementQueue &&) = delete;
  RetirementQueue &operator=(RetirementQueue &&) = delete;
  ~RetirementQueue() noexcept;

  /// All potentially throwing allocations occur before the noexcept move.
  /// On allocation or validation failure the caller still owns the resource.
  /// Only noncopyable owners are accepted: a noexcept rvalue construction
  /// alone could invoke a copy constructor and duplicate native ownership.
  /// Resource may be a move-only RAII aggregate: declare Image before ImageView
  /// within the aggregate so its destructor destroys View before Image.
  template <class Resource>
    requires(!std::is_copy_constructible_v<Resource> && !std::is_copy_assignable_v<Resource> &&
             std::is_nothrow_move_constructible_v<Resource> &&
             std::is_nothrow_destructible_v<Resource> &&
             requires(const Resource &r) {
               { r.valid() } -> std::convertible_to<bool>;
             })
  [[nodiscard]] terreate::Result<void> retire(Resource &owner,
                                              std::span<const SubmissionReceipt> uses) {
    if (!owner.valid())
      return fail(RetirementError::invalid_resource);
    if (lost_)
      return fail(RetirementError::device_lost);
    if (uses.empty())
      return fail(RetirementError::missing_completion);

    const Device *device = nullptr;
    for (const auto &receipt : uses) {
      const auto &description = receipt.completion();
      if (!description.device || !description.device->valid() || !description.queue.valid() ||
          !description.queue.correlatedWith(*description.device) ||
          !description.requested.valid() || !description.effective.valid())
        return fail(RetirementError::invalid_completion);
      if (!device)
        device = description.device;
      if (description.device != device || (device_ && description.device != device_))
        return fail(RetirementError::device_mismatch);
    }

    try {
      std::vector<SubmissionReceipt> receipts(uses.begin(), uses.end());
      entries_.reserve(entries_.size() + 1);
      // new allocates BEFORE calling Owned's noexcept move constructor.
      auto holder = std::unique_ptr<Owner>(new Owned<Resource>(std::move(owner)));
      entries_.push_back(Entry{std::move(holder), std::move(receipts)});
    } catch (const std::bad_alloc &) {
      return fail(RetirementError::allocation_failed);
    }
    device_ = device;
    return {};
  }

  /// One explicit nonblocking observation per declared receipt. Only every
  /// term COMPLETED may destroy its owner; any native error releases NOTHING.
  /// A native DEVICE_LOST error latches an explicit recovery capability.
  [[nodiscard]] terreate::Result<CollectReport> collect();

  /// Destructive, caller-invoked teardown on a LOST logical Device only.
  /// Legal only after collect() has observed native VK_ERROR_DEVICE_LOST on
  /// this queue's single Device. Not an ordinary completion/success path.
  /// Caller guarantees all owned resources are on the lost Device, parent
  /// objects are still valid, and dependency groups destroy child before parent.
  /// Destroys all retained RAII owners, without polling/waiting or waitIdle.
  [[nodiscard]] terreate::Result<std::size_t> reclaimAfterDeviceLoss(const Device &device);

  [[nodiscard]] std::size_t pendingCount() const noexcept { return entries_.size(); }
  [[nodiscard]] bool deviceLossObserved() const noexcept { return lost_; }
  [[nodiscard]] terreate::Result<void> requireEmpty() const;

private:
  struct Owner {
    virtual ~Owner() = default;
  };
  template <class Resource> struct Owned final : Owner {
    explicit Owned(Resource &&r) noexcept : value(std::move(r)) {}
    Resource value;
  };
  struct Entry {
    std::unique_ptr<Owner> owner;
    std::vector<SubmissionReceipt> receipts;
  };

  [[nodiscard]] static terreate::Result<void> fail(RetirementError);
  std::vector<Entry> entries_;
  const Device *device_ = nullptr;
  bool lost_ = false;
};
} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::RetirementError> : true_type {};
} // namespace std
#endif // TERREATE_GRAPHICS_RETIREMENT_HPP
