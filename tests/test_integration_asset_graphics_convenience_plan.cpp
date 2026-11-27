// #463: the CPU side of the beginner mesh upload convenience. The default
// shader-location mapping, its documented rule, and the plan that reports which
// mapping was used. No Vulkan Instance or device is created; no OS- or
// linker-specific mechanism is used.
#include <terreate/integration/asset_graphics/mesh_upload.hpp>
#include <terreate/integration/asset_graphics/mesh_upload_convenience.hpp>

#include <terreate/asset/primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace asset = terreate::asset;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics.convenience_plan";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

static_assert(!std::is_copy_constructible_v<ag::MeshUploadSubmission>);
static_assert(std::is_move_constructible_v<ag::MeshUploadSubmission>);
static_assert(!std::is_copy_constructible_v<ag::MeshUploadResult>);

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

const asset::AttributeKey position{asset::AttributeSemantic::position, 0, {}};
const asset::AttributeKey normal{asset::AttributeSemantic::normal, 0, {}};
const asset::AttributeKey tangent{asset::AttributeSemantic::tangent, 0, {}};
const asset::AttributeKey texcoord{asset::AttributeSemantic::texcoord, 0, {}};
const asset::AttributeKey texcoord1{asset::AttributeSemantic::texcoord, 1, {}};
const asset::AttributeKey color{asset::AttributeSemantic::color, 0, {}};
const asset::AttributeKey custom{asset::AttributeSemantic::custom, 0, "weights"};

asset::AttributeStream zeroed(const asset::AttributeKey &key, asset::AttributeFormat format,
                              std::size_t count) {
  asset::AttributeStream stream;
  stream.key = key;
  stream.format = format;
  stream.count = count;
  stream.data.assign(count * stream.elementSize(), std::byte{0});
  return stream;
}

bool locationsAre(const std::vector<ag::MeshAttributeBinding> &bindings,
                  const std::vector<std::pair<asset::AttributeKey, std::uint32_t>> &expected) {
  if (bindings.size() != expected.size())
    return false;
  for (std::size_t i = 0; i < expected.size(); ++i)
    if (!(bindings[i].key == expected[i].first) || bindings[i].location != expected[i].second)
      return false;
  return true;
}

void defaultMapping() {
  const auto box = terreate::unwrap(asset::box({})); // position, normal, texcoord 0
  check(locationsAre(ag::defaultMeshAttributeBindings(box),
                     {{position, 0}, {normal, 1}, {texcoord, 2}}),
        "a box maps position 0, normal 1, texcoord 2");

  const auto positions_only = terreate::unwrap(asset::box({.normals = false, .uvs = false}));
  check(locationsAre(ag::defaultMeshAttributeBindings(positions_only), {{position, 0}}),
        "only the attributes the mesh has are mapped");

  // Every default attribute, plus attributes that are never in the default.
  auto rich = terreate::unwrap(asset::box({.tangents = true}));
  const auto count = rich.vertexCount();
  rich.attributes.push_back(zeroed(color, asset::AttributeFormat::float32x4, count));
  rich.attributes.push_back(zeroed(texcoord1, asset::AttributeFormat::float32x2, count));
  rich.attributes.push_back(zeroed(custom, asset::AttributeFormat::float32x4, count));
  check(bool(asset::validate(rich)), "the rich mesh is valid");
  check(locationsAre(ag::defaultMeshAttributeBindings(rich),
                     {{position, 0}, {normal, 1}, {texcoord, 2}, {color, 3}, {tangent, 4}}),
        "position 0, normal 1, texcoord 2, color 3, tangent 4; no texcoord set 1, no custom");

  // The result is an ordinary value: it can be edited and used with the
  // three-step planMeshUpload.
  auto edited = ag::defaultMeshAttributeBindings(rich);
  edited[1].location = 7;
  ag::MeshUploadDescription description;
  description.attributes = edited;
  const auto plan = ag::planMeshUpload(rich, description);
  check(plan && !plan->used_default_attributes && plan->attributes[1].location == 7,
        "an edited default is the application's own list");
}

void planReportsTheMapping() {
  const auto box = terreate::unwrap(asset::box({}));

  const auto defaulted = ag::planMeshUploadWithDefaults(box, {});
  if (!check(bool(defaulted), "an empty list is the default mapping"))
    return;
  check(defaulted->used_default_attributes, "the plan reports that the default was used");
  check(defaulted->attributes.size() == 3 && defaulted->attributes[0].location == 0 &&
            defaulted->attributes[1].location == 1 && defaulted->attributes[2].location == 2 &&
            defaulted->unmapped.empty(),
        "the resolved locations are in the plan");

  // The default plan equals the explicit three-step plan made from the helper.
  ag::MeshUploadDescription explicit_description;
  explicit_description.attributes = ag::defaultMeshAttributeBindings(box);
  const auto explicit_plan = ag::planMeshUpload(box, explicit_description);
  check(explicit_plan && !explicit_plan->used_default_attributes &&
            explicit_plan->vertex_bytes == defaulted->vertex_bytes &&
            explicit_plan->index_bytes == defaulted->index_bytes &&
            explicit_plan->binding == defaulted->binding &&
            explicit_plan->attributes == defaulted->attributes,
        "the default plan has the bytes and structs of the explicit plan");

  // A non-empty list is used exactly as given.
  ag::MeshUploadDescription given;
  given.attributes = {{position, 5}};
  const auto given_plan = ag::planMeshUploadWithDefaults(box, given);
  check(given_plan && !given_plan->used_default_attributes && given_plan->attributes.size() == 1 &&
            given_plan->attributes[0].location == 5 && given_plan->unmapped.size() == 2,
        "a non-empty list is used exactly as given and reports unmapped attributes");

  // The three-step planMeshUpload still rejects an empty list.
  check(hasError(ag::planMeshUpload(box, {}), ag::MeshUploadError::no_attributes),
        "planMeshUpload still rejects an empty list");
}

void errors() {
  const auto box = terreate::unwrap(asset::box({}));

  // A mesh without a position is not valid Asset data: the plan returns the
  // MeshDataError unchanged; the helper just lists what exists.
  auto headless = box;
  std::erase_if(headless.attributes,
                [](const asset::AttributeStream &stream) { return stream.key == position; });
  check(locationsAre(ag::defaultMeshAttributeBindings(headless), {{normal, 1}, {texcoord, 2}}),
        "the helper lists only what a mesh without position has");
  const auto refused = ag::planMeshUploadWithDefaults(headless, {});
  check(!refused &&
            refused.error().code() == make_error_code(asset::MeshDataError::missing_position),
        "a mesh without position is the asset error, unchanged");

  // Other plan errors pass through the convenience plan unchanged.
  ag::MeshUploadDescription duplicate;
  duplicate.attributes = {{position, 0}, {normal, 0}};
  check(hasError(ag::planMeshUploadWithDefaults(box, duplicate),
                 ag::MeshUploadError::duplicate_location),
        "a given list with a duplicate location is refused as in planMeshUpload");

  // The timeout error has a message.
  check(!ag::mesh_upload_error_category()
             .message(static_cast<int>(ag::MeshUploadError::wait_timeout))
             .empty(),
        "wait_timeout has a message");
}

} // namespace

int main() {
  defaultMapping();
  planReportsTheMapping();
  errors();
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("integration.asset_graphics.convenience_plan: plan=PASS");
  return EXIT_SUCCESS;
}
