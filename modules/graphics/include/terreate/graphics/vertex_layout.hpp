#ifndef TERREATE_GRAPHICS_VERTEX_LAYOUT_HPP
#define TERREATE_GRAPHICS_VERTEX_LAYOUT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <type_traits>
#include <vector>

#include <terreate/core/result.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>

namespace terreate::graphics {

enum class VertexLayoutError : std::uint8_t {
  /// A layout needs at least one attribute.
  no_attributes = 1,
  /// The format is undefined, compressed or multi-plane: a vertex attribute is a single
  /// uncompressed texel-sized value.
  unsupported_format,
  /// The block size of the format differs from the size of the member it reads.
  format_size_mismatch,
  /// Two attributes of one layout cover some of the same bytes.
  attributes_overlap,
  /// Two attributes use the same shader location.
  duplicate_location,
  /// Two layouts combined use the same binding number.
  duplicate_binding,
};

[[nodiscard]] const std::error_category &vertex_layout_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(VertexLayoutError error) noexcept;

/// Where one member of a vertex type is read by the pipeline: the shader location, the format
/// the application states, the byte offset of the member (measured from the type) and the
/// member's size.
struct VertexAttributeData {
  std::uint32_t location = 0;
  vk::Format format = vk::Format::eUndefined;
  std::uint32_t offset = 0;
  std::uint32_t member_size = 0;
};

/// A `VertexAttributeData` that belongs to the vertex type `Vertex`, so that a layout cannot mix
/// members of different types. Made by `vertexAttribute`.
template <class Vertex> struct VertexAttribute {
  VertexAttributeData data{};
};

namespace detail {
template <class Member> struct MemberPointerTraits;
template <class Class, class Type> struct MemberPointerTraits<Type Class::*> {
  using ClassType = Class;
  using MemberType = Type;
};
} // namespace detail

/// Declare that the member `Member` of the application's vertex type (for example
/// `vertexAttribute<&Vertex::position>(0, vk::Format::eR32G32B32Sfloat)`) is read at shader
/// location `location` as `format`. The format is always stated: it is never deduced from the
/// member's type, because the same four bytes can be unorm, snorm, uint or sint. The byte offset
/// is measured from the member of a default-constructed object of the type (no macro, no
/// `offsetof`), so the type must be standard layout, trivially copyable and default
/// constructible. Nothing is validated here; `vertexLayout` validates the whole set.
template <auto Member>
[[nodiscard]] auto vertexAttribute(std::uint32_t location, vk::Format format)
    -> VertexAttribute<typename detail::MemberPointerTraits<decltype(Member)>::ClassType> {
  using Traits = detail::MemberPointerTraits<decltype(Member)>;
  using Vertex = typename Traits::ClassType;
  static_assert(std::is_standard_layout_v<Vertex> && std::is_trivially_copyable_v<Vertex> &&
                    std::is_default_constructible_v<Vertex>,
                "a vertex type must be standard layout, trivially copyable and default "
                "constructible");
  static_assert(!std::is_member_function_pointer_v<decltype(Member)>,
                "a vertex attribute is a data member");
  const Vertex probe{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *base = reinterpret_cast<const std::byte *>(&probe);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *member = reinterpret_cast<const std::byte *>(&(probe.*Member));
  return {{location, format, static_cast<std::uint32_t>(member - base),
           static_cast<std::uint32_t>(sizeof(typename Traits::MemberType))}};
}

/// The binding number and the input rate of one vertex buffer binding.
struct VertexBindingSpec {
  std::uint32_t binding = 0;
  vk::VertexInputRate rate = vk::VertexInputRate::eVertex;
};

/// A validated CPU-only description of how the vertex buffers feed a pipeline: the native
/// `vk::VertexInputBindingDescription` and `vk::VertexInputAttributeDescription` values that
/// `GraphicsPipelineDescription` takes, derived from the application's vertex struct. It holds no
/// Device or Vulkan object and calls no Vulkan function.
class VertexLayout final {
public:
  [[nodiscard]] std::span<const vk::VertexInputBindingDescription> bindings() const noexcept {
    return bindings_;
  }
  [[nodiscard]] std::span<const vk::VertexInputAttributeDescription> attributes() const noexcept {
    return attributes_;
  }
  /// Replace `vertex_bindings` and `vertex_attributes` of `description` with this layout.
  void applyTo(GraphicsPipelineDescription &description) const {
    description.vertex_bindings = bindings_;
    description.vertex_attributes = attributes_;
  }

private:
  friend auto makeVertexLayout(std::size_t stride, VertexBindingSpec binding,
                               std::span<const VertexAttributeData> attributes)
      -> terreate::Result<VertexLayout>;
  friend auto combine(const VertexLayout &first, const VertexLayout &second)
      -> terreate::Result<VertexLayout>;
  std::vector<vk::VertexInputBindingDescription> bindings_{};
  std::vector<vk::VertexInputAttributeDescription> attributes_{};
};

/// The non-template core of `vertexLayout`: validates `attributes` against `stride` and builds
/// the layout. Prefer `vertexLayout`, which supplies the stride and the offsets from the type.
[[nodiscard]] auto makeVertexLayout(std::size_t stride, VertexBindingSpec binding,
                                    std::span<const VertexAttributeData> attributes)
    -> terreate::Result<VertexLayout>;

/// The layout of one vertex buffer binding whose elements are `Vertex`: the stride is
/// `sizeof(Vertex)`, and the attributes are the ones declared with `vertexAttribute`. Validates
/// before anything else: at least one attribute, every format single-plane and uncompressed with
/// a block size equal to its member's size, no overlapping attributes, no duplicate location.
template <class Vertex, class... Attributes>
[[nodiscard]] auto vertexLayout(VertexBindingSpec binding, VertexAttribute<Vertex> first,
                                Attributes... rest) -> terreate::Result<VertexLayout>
  requires(std::is_same_v<Attributes, VertexAttribute<Vertex>> && ...)
{
  const std::array<VertexAttributeData, 1 + sizeof...(Attributes)> data{first.data, rest.data...};
  return makeVertexLayout(sizeof(Vertex), binding, data);
}

/// Binding 0, per-vertex rate.
template <class Vertex, class... Attributes>
[[nodiscard]] auto vertexLayout(VertexAttribute<Vertex> first, Attributes... rest)
    -> terreate::Result<VertexLayout>
  requires(std::is_same_v<Attributes, VertexAttribute<Vertex>> && ...)
{
  return vertexLayout(VertexBindingSpec{}, first, rest...);
}

/// Join two layouts of different bindings (for example per-vertex data and per-instance data):
/// the bindings and attributes of `first`, then those of `second`. Fails when a binding number
/// or a shader location occurs in both.
[[nodiscard]] auto combine(const VertexLayout &first, const VertexLayout &second)
    -> terreate::Result<VertexLayout>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::VertexLayoutError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_VERTEX_LAYOUT_HPP
