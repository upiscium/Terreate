#include <terreate/graphics/vertex_layout.hpp>

#include <vulkan/vulkan_format_traits.hpp>

#include <algorithm>
#include <new>
#include <string>

namespace terreate::graphics {
namespace {

class VertexLayoutErrorCategory final : public std::error_category {
public:
  [[nodiscard]] const char *name() const noexcept override {
    return "terreate.graphics.vertex_layout";
  }
  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<VertexLayoutError>(value)) {
    case VertexLayoutError::no_attributes:
      return "a vertex layout needs at least one attribute";
    case VertexLayoutError::unsupported_format:
      return "a vertex attribute format must be a single-plane uncompressed format";
    case VertexLayoutError::format_size_mismatch:
      return "the block size of a vertex attribute format differs from the size of its member";
    case VertexLayoutError::attributes_overlap:
      return "two vertex attributes overlap";
    case VertexLayoutError::duplicate_location:
      return "two vertex attributes use the same shader location";
    case VertexLayoutError::duplicate_binding:
      return "two combined vertex layouts use the same binding";
    }
    return "unknown vertex layout error";
  }
};

auto fail(VertexLayoutError error, const char *operation, const char *detail) -> terreate::Error {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}

// A vertex attribute is one uncompressed, single-plane texel: its size is the format's block
// size. Undefined, compressed and multi-plane formats have no such size.
bool usableFormat(vk::Format format) noexcept {
  if (format == vk::Format::eUndefined)
    return false;
  if (vk::planeCount(format) != 1)
    return false;
  const char *compression = vk::compressionScheme(format);
  return compression != nullptr &&
         *compression == '\0'; // "" is the traits' answer for uncompressed formats
}

} // namespace

const std::error_category &vertex_layout_error_category() noexcept {
  static const VertexLayoutErrorCategory category;
  return category;
}
std::error_code make_error_code(VertexLayoutError error) noexcept {
  return {static_cast<int>(error), vertex_layout_error_category()};
}

auto makeVertexLayout(std::size_t stride, VertexBindingSpec binding,
                      std::span<const VertexAttributeData> attributes)
    -> terreate::Result<VertexLayout> {
  if (attributes.empty())
    return std::unexpected(
        fail(VertexLayoutError::no_attributes, "vertexLayout", "declare at least one attribute"));
  for (const auto &attribute : attributes) {
    if (!usableFormat(attribute.format))
      return std::unexpected(fail(VertexLayoutError::unsupported_format, "vertexLayout",
                                  "the format is undefined, compressed or multi-plane"));
    if (vk::blockSize(attribute.format) != attribute.member_size)
      return std::unexpected(fail(VertexLayoutError::format_size_mismatch, "vertexLayout",
                                  "the format's block size differs from the member's size"));
  }
  for (std::size_t i = 0; i < attributes.size(); ++i) {
    for (std::size_t j = i + 1; j < attributes.size(); ++j) {
      if (attributes[i].location == attributes[j].location)
        return std::unexpected(fail(VertexLayoutError::duplicate_location, "vertexLayout",
                                    "two attributes use the same location"));
      const std::uint64_t first_end =
          std::uint64_t{attributes[i].offset} + attributes[i].member_size;
      const std::uint64_t second_end =
          std::uint64_t{attributes[j].offset} + attributes[j].member_size;
      if (attributes[i].offset < second_end && attributes[j].offset < first_end)
        return std::unexpected(fail(VertexLayoutError::attributes_overlap, "vertexLayout",
                                    "two attributes cover the same bytes"));
    }
  }
  try {
    VertexLayout layout;
    layout.bindings_.push_back(vk::VertexInputBindingDescription{
        binding.binding, static_cast<std::uint32_t>(stride), binding.rate});
    layout.attributes_.reserve(attributes.size());
    for (const auto &attribute : attributes)
      layout.attributes_.push_back(vk::VertexInputAttributeDescription{
          attribute.location, binding.binding, attribute.format, attribute.offset});
    return layout;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{std::make_error_code(std::errc::not_enough_memory)});
  }
}

auto combine(const VertexLayout &first, const VertexLayout &second)
    -> terreate::Result<VertexLayout> {
  for (const auto &a : first.bindings_)
    for (const auto &b : second.bindings_)
      if (a.binding == b.binding)
        return std::unexpected(fail(VertexLayoutError::duplicate_binding, "combine",
                                    "a binding number occurs in both layouts"));
  for (const auto &a : first.attributes_)
    for (const auto &b : second.attributes_)
      if (a.location == b.location)
        return std::unexpected(fail(VertexLayoutError::duplicate_location, "combine",
                                    "a shader location occurs in both layouts"));
  try {
    VertexLayout layout = first;
    layout.bindings_.insert(layout.bindings_.end(), second.bindings_.begin(),
                            second.bindings_.end());
    layout.attributes_.insert(layout.attributes_.end(), second.attributes_.begin(),
                              second.attributes_.end());
    return layout;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{std::make_error_code(std::errc::not_enough_memory)});
  }
}

} // namespace terreate::graphics
