#include <terreate/graphics/specialization.hpp>

#include <limits>
#include <string>

namespace terreate::graphics {
namespace {
class SpecializationErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.specialization"; }
  std::string message(int value) const override {
    switch (static_cast<SpecializationError>(value)) {
    case SpecializationError::entry_out_of_range:
      return "specialization map entry outside the data";
    case SpecializationError::duplicate_constant_id:
      return "duplicate specialization constant ID";
    case SpecializationError::empty_entry:
      return "specialization map entry with zero size";
    case SpecializationError::data_without_entries:
      return "specialization data without map entries";
    case SpecializationError::too_large:
      return "specialization entry count exceeds uint32_t";
    }
    return "unknown specialization error";
  }
};

terreate::Result<void> fail(SpecializationError error, const char *detail) {
  return std::unexpected(
      terreate::Error{make_error_code(error), "validateSpecializationDescription", detail});
}
} // namespace

const std::error_category &specialization_error_category() noexcept {
  static const SpecializationErrorCategory category;
  return category;
}
std::error_code make_error_code(SpecializationError error) noexcept {
  return {static_cast<int>(error), specialization_error_category()};
}

auto validateSpecializationDescription(const SpecializationDescription &description)
    -> terreate::Result<void> {
  const auto &entries = description.map_entries;
  const auto data_size = description.data.size();
  if (entries.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(SpecializationError::too_large, "mapEntryCount must fit uint32_t");
  if (entries.empty() && data_size != 0)
    return fail(SpecializationError::data_without_entries, "data requires map entries");
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto &entry = entries[index];
    if (entry.size == 0)
      return fail(SpecializationError::empty_entry, "a zero size matches no constant");
    // 00773 and 00774, written so that no sum can overflow.
    if (entry.offset >= data_size || entry.size > data_size - entry.offset)
      return fail(SpecializationError::entry_out_of_range, "entry must lie inside the data");
    for (std::size_t other = 0; other < index; ++other)
      if (entries[other].constantID == entry.constantID)
        return fail(SpecializationError::duplicate_constant_id, "constant IDs must be unique");
  }
  return {};
}

} // namespace terreate::graphics
