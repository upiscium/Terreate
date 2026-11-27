#include <terreate/asset/mesh_data.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace terreate::asset {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "MeshData stores IEEE 754 binary32 floats");

class MeshDataErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.asset.mesh_data"; }
  std::string message(int value) const override {
    switch (static_cast<MeshDataError>(value)) {
    case MeshDataError::missing_position:
      return "the mesh has no usable position stream";
    case MeshDataError::count_mismatch:
      return "an attribute stream's count differs from the position count";
    case MeshDataError::format_not_allowed:
      return "the format is not allowed for the attribute semantic";
    case MeshDataError::index_out_of_range:
      return "an index is not below the vertex count";
    case MeshDataError::invalid_index_count:
      return "the element count is not valid for the topology";
    case MeshDataError::duplicate_attribute:
      return "two attributes have the same semantic, set and name";
    case MeshDataError::bounds_not_enclosing:
      return "the bounds do not enclose every position";
    case MeshDataError::data_size_mismatch:
      return "the data size does not match the element count and format";
    case MeshDataError::invalid_attribute_key:
      return "the set index or name is not valid for the semantic";
    case MeshDataError::non_finite_position:
      return "a position is NaN or infinite";
    case MeshDataError::attribute_not_found:
      return "the mesh has no such attribute";
    }
    return "unknown MeshData error";
  }
};

const char *semanticName(AttributeSemantic semantic) noexcept {
  switch (semantic) {
  case AttributeSemantic::position:
    return "position";
  case AttributeSemantic::normal:
    return "normal";
  case AttributeSemantic::tangent:
    return "tangent";
  case AttributeSemantic::texcoord:
    return "texcoord";
  case AttributeSemantic::color:
    return "color";
  case AttributeSemantic::custom:
    return "custom";
  }
  return "unknown";
}

std::string describe(const AttributeKey &key) {
  std::string text = semanticName(key.semantic);
  if (key.semantic == AttributeSemantic::custom)
    text += " '" + key.name + "'";
  text += " set " + std::to_string(key.set);
  return text;
}

// Error construction allocates; on exhaustion a code-only Error is returned,
// which does not allocate.
Error makeError(MeshDataError error, const char *operation, const std::string &detail) {
  try {
    return Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return Error{make_error_code(error)};
  }
}

bool formatAllowed(const AttributeKey &key, AttributeFormat format) noexcept {
  switch (key.semantic) {
  case AttributeSemantic::position:
  case AttributeSemantic::normal:
    return format == AttributeFormat::float32x3;
  case AttributeSemantic::tangent:
    return format == AttributeFormat::float32x4;
  case AttributeSemantic::texcoord:
    return format == AttributeFormat::float32x2;
  case AttributeSemantic::color:
    return format == AttributeFormat::float32x3 || format == AttributeFormat::float32x4;
  case AttributeSemantic::custom:
    return componentCount(format) != 0;
  }
  return false;
}

bool keyValid(const AttributeKey &key) noexcept {
  switch (key.semantic) {
  case AttributeSemantic::position:
  case AttributeSemantic::normal:
  case AttributeSemantic::tangent:
    return key.set == 0 && key.name.empty();
  case AttributeSemantic::texcoord:
  case AttributeSemantic::color:
    return key.name.empty();
  case AttributeSemantic::custom:
    return !key.name.empty();
  }
  return false;
}

// A position that cannot be read is treated as NaN: it is never enclosed and never finite.
constexpr std::array<float, 4> kNotANumber{
    std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};

bool elementCountValid(PrimitiveTopology topology, std::size_t count) noexcept {
  switch (topology) {
  case PrimitiveTopology::triangle_list:
    return count % 3 == 0;
  case PrimitiveTopology::triangle_strip:
    return count == 0 || count >= 3;
  case PrimitiveTopology::line_list:
    return count % 2 == 0;
  case PrimitiveTopology::point_list:
    return true;
  }
  return false;
}

template <typename Index>
std::optional<std::size_t> firstOutOfRange(const std::vector<Index> &indices,
                                           std::size_t vertexCount) noexcept {
  for (std::size_t i = 0; i < indices.size(); ++i)
    if (static_cast<std::size_t>(indices[i]) >= vertexCount)
      return i;
  return std::nullopt;
}

} // namespace

auto mesh_data_error_category() noexcept -> const std::error_category & {
  static const MeshDataErrorCategory category;
  return category;
}

auto make_error_code(MeshDataError error) noexcept -> std::error_code {
  return {static_cast<int>(error), mesh_data_error_category()};
}

auto AttributeStream::get(std::size_t vertex) const noexcept
    -> std::optional<std::array<float, 4>> {
  const std::size_t size = elementSize();
  if (size == 0 || vertex >= count || vertex >= data.size() / size)
    return std::nullopt;
  std::array<float, 4> value{0.0F, 0.0F, 0.0F, 0.0F};
  std::memcpy(value.data(), data.data() + vertex * size, size);
  return value;
}

auto AttributeStream::toFloats() const -> terreate::Result<std::vector<float>> {
  if (!sizeConsistent())
    return std::unexpected(
        makeError(MeshDataError::data_size_mismatch, "AttributeStream::toFloats", describe(key)));
  std::vector<float> values(count * components());
  if (!values.empty())
    std::memcpy(values.data(), data.data(), data.size());
  return values;
}

auto makeAttributeStream(AttributeKey key, AttributeFormat format, std::span<const float> values)
    -> terreate::Result<AttributeStream> {
  const std::size_t per = componentCount(format);
  if (values.size() % per != 0)
    return std::unexpected(makeError(MeshDataError::data_size_mismatch, "makeAttributeStream",
                                     describe(key) + ": value count is not a multiple of the "
                                                     "component count"));
  AttributeStream stream;
  stream.key = std::move(key);
  stream.format = format;
  stream.count = values.size() / per;
  stream.data.resize(values.size_bytes());
  if (!values.empty())
    std::memcpy(stream.data.data(), values.data(), values.size_bytes());
  return stream;
}

auto MeshData::find(const AttributeKey &key) const noexcept -> const AttributeStream * {
  for (const auto &stream : attributes)
    if (stream.key == key)
      return &stream;
  return nullptr;
}

auto MeshData::find(const AttributeKey &key) noexcept -> AttributeStream * {
  return const_cast<AttributeStream *>(std::as_const(*this).find(key));
}

auto MeshData::positions() const noexcept -> const AttributeStream * {
  return find(AttributeKey{AttributeSemantic::position, 0, {}});
}

auto MeshData::vertexCount() const noexcept -> std::size_t {
  const AttributeStream *position = positions();
  return position == nullptr ? 0 : position->count;
}

auto MeshData::indexType() const noexcept -> IndexType {
  if (std::holds_alternative<std::vector<std::uint16_t>>(indices))
    return IndexType::uint16;
  if (std::holds_alternative<std::vector<std::uint32_t>>(indices))
    return IndexType::uint32;
  return IndexType::none;
}

auto MeshData::indexCount() const noexcept -> std::size_t {
  if (const auto *narrow = std::get_if<std::vector<std::uint16_t>>(&indices))
    return narrow->size();
  if (const auto *wide = std::get_if<std::vector<std::uint32_t>>(&indices))
    return wide->size();
  return 0;
}

auto MeshData::index(std::size_t position) const noexcept -> std::optional<std::uint32_t> {
  if (const auto *narrow = std::get_if<std::vector<std::uint16_t>>(&indices))
    return position < narrow->size() ? std::optional<std::uint32_t>{(*narrow)[position]}
                                     : std::nullopt;
  if (const auto *wide = std::get_if<std::vector<std::uint32_t>>(&indices))
    return position < wide->size() ? std::optional<std::uint32_t>{(*wide)[position]} : std::nullopt;
  return std::nullopt;
}

auto validate(const MeshData &mesh) -> terreate::Result<void> {
  constexpr const char *op = "asset::validate";
  for (std::size_t i = 0; i < mesh.attributes.size(); ++i) {
    const AttributeStream &stream = mesh.attributes[i];
    if (!keyValid(stream.key))
      return std::unexpected(
          makeError(MeshDataError::invalid_attribute_key, op, describe(stream.key)));
    for (std::size_t j = 0; j < i; ++j)
      if (mesh.attributes[j].key == stream.key)
        return std::unexpected(
            makeError(MeshDataError::duplicate_attribute, op, describe(stream.key)));
    if (!formatAllowed(stream.key, stream.format))
      return std::unexpected(
          makeError(MeshDataError::format_not_allowed, op, describe(stream.key)));
    if (!stream.sizeConsistent())
      return std::unexpected(
          makeError(MeshDataError::data_size_mismatch, op, describe(stream.key)));
  }

  const AttributeStream *position = mesh.positions();
  if (position == nullptr)
    return std::unexpected(makeError(MeshDataError::missing_position, op, "no position stream"));
  const std::size_t vertices = position->count;
  for (const auto &stream : mesh.attributes)
    if (stream.count != vertices)
      return std::unexpected(makeError(MeshDataError::count_mismatch, op,
                                       describe(stream.key) + " has " +
                                           std::to_string(stream.count) + " elements, positions " +
                                           std::to_string(vertices)));

  const std::size_t elements = mesh.indexType() == IndexType::none ? vertices : mesh.indexCount();
  if (!elementCountValid(mesh.topology, elements))
    return std::unexpected(
        makeError(MeshDataError::invalid_index_count, op, std::to_string(elements) + " elements"));

  std::optional<std::size_t> bad;
  if (const auto *narrow = std::get_if<std::vector<std::uint16_t>>(&mesh.indices))
    bad = firstOutOfRange(*narrow, vertices);
  else if (const auto *wide = std::get_if<std::vector<std::uint32_t>>(&mesh.indices))
    bad = firstOutOfRange(*wide, vertices);
  if (bad)
    return std::unexpected(makeError(MeshDataError::index_out_of_range, op,
                                     "index " + std::to_string(*bad) + " is " +
                                         std::to_string(mesh.index(*bad).value_or(0)) +
                                         ", vertex count " + std::to_string(vertices)));

  if (mesh.bounds) {
    for (std::size_t v = 0; v < vertices; ++v) {
      const auto p = position->get(v).value_or(kNotANumber);
      for (std::size_t c = 0; c < 3; ++c)
        // Written so that a NaN position is not enclosed.
        if (!(p[c] >= mesh.bounds->min[c] && p[c] <= mesh.bounds->max[c]))
          return std::unexpected(
              makeError(MeshDataError::bounds_not_enclosing, op, "position " + std::to_string(v)));
    }
  }
  return {};
}

auto computeBounds(const MeshData &mesh) -> terreate::Result<Bounds> {
  constexpr const char *op = "asset::computeBounds";
  const AttributeStream *position = mesh.positions();
  if (position == nullptr || position->count == 0)
    return std::unexpected(
        makeError(MeshDataError::missing_position, op, "no position or an empty position stream"));
  if (position->format != AttributeFormat::float32x3)
    return std::unexpected(makeError(MeshDataError::format_not_allowed, op, "position"));
  if (!position->sizeConsistent())
    return std::unexpected(makeError(MeshDataError::data_size_mismatch, op, "position"));
  Bounds bounds;
  for (std::size_t v = 0; v < position->count; ++v) {
    const auto p = position->get(v).value_or(kNotANumber);
    for (std::size_t c = 0; c < 3; ++c) {
      if (!std::isfinite(p[c]))
        return std::unexpected(
            makeError(MeshDataError::non_finite_position, op, "position " + std::to_string(v)));
      if (v == 0) {
        bounds.min[c] = bounds.max[c] = p[c];
      } else {
        bounds.min[c] = std::min(bounds.min[c], p[c]);
        bounds.max[c] = std::max(bounds.max[c], p[c]);
      }
    }
  }
  return bounds;
}

auto layoutOf(const MeshData &mesh) -> MeshLayout {
  MeshLayout layout;
  layout.topology = mesh.topology;
  layout.indexType = mesh.indexType();
  layout.vertexCount = mesh.vertexCount();
  layout.indexCount = mesh.indexCount();
  layout.interleaved = false;
  for (const auto &stream : mesh.attributes)
    layout.attributes.push_back(AttributeDescription{
        stream.key, stream.format, 0, static_cast<std::uint32_t>(stream.elementSize())});
  return layout;
}

auto interleave(const MeshData &mesh, std::span<const AttributeKey> order)
    -> terreate::Result<InterleavedMesh> {
  constexpr const char *op = "asset::interleave";
  if (auto valid = validate(mesh); !valid)
    return std::unexpected(std::move(valid.error()));

  std::vector<const AttributeStream *> selected;
  if (order.empty()) {
    for (const auto &stream : mesh.attributes)
      selected.push_back(&stream);
  } else {
    for (std::size_t i = 0; i < order.size(); ++i) {
      for (std::size_t j = 0; j < i; ++j)
        if (order[j] == order[i])
          return std::unexpected(
              makeError(MeshDataError::duplicate_attribute, op, describe(order[i])));
      const AttributeStream *stream = mesh.find(order[i]);
      if (stream == nullptr)
        return std::unexpected(
            makeError(MeshDataError::attribute_not_found, op, describe(order[i])));
      selected.push_back(stream);
    }
  }

  InterleavedMesh result;
  result.layout.topology = mesh.topology;
  result.layout.indexType = mesh.indexType();
  result.layout.vertexCount = mesh.vertexCount();
  result.layout.indexCount = mesh.indexCount();
  result.layout.interleaved = true;
  std::uint32_t stride = 0;
  for (const AttributeStream *stream : selected) {
    result.layout.attributes.push_back(
        AttributeDescription{stream->key, stream->format, stride, 0});
    stride += static_cast<std::uint32_t>(stream->elementSize());
  }
  for (auto &attribute : result.layout.attributes)
    attribute.stride = stride;

  result.vertices.resize(result.layout.vertexCount * stride);
  for (std::size_t v = 0; v < result.layout.vertexCount; ++v) {
    std::byte *out = result.vertices.data() + v * stride;
    for (const AttributeStream *stream : selected) {
      const std::size_t size = stream->elementSize();
      std::memcpy(out, stream->data.data() + v * size, size);
      out += size;
    }
  }
  result.indices = mesh.indices;
  return result;
}

} // namespace terreate::asset
