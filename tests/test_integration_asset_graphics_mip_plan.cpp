// #467: the CPU side of the explicit mip generation choice of the image upload. The default
// (`none`) is unchanged, `cpu_box` makes the plan describe and own the GENERATED levels, the
// options reach asset::generateMipChain, and the errors of Asset and of the Integration are
// passed through unchanged. No Vulkan Instance or device is created; no OS- or linker-specific
// mechanism is used.
#include <terreate/integration/asset_graphics/image_upload.hpp>
#include <terreate/integration/asset_graphics/image_upload_convenience.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace {
namespace asset = terreate::asset;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics.mip_plan";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

// The generated image of a plan, or an empty image when the plan has none (the checks then fail).
const asset::ImageData &generatedOf(const ag::ImageUploadPlan &plan) {
  static const asset::ImageData none;
  return plan.generated ? *plan.generated : none;
}

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

static_assert(static_cast<int>(ag::MipGeneration::none) == 0 &&
              static_cast<int>(ag::MipGeneration::cpu_box) == 1);

asset::ImageData makeImage(asset::PixelFormat format, asset::ColorInterpretation interpretation,
                           asset::ImageExtent extent, std::uint32_t layers = 1,
                           std::uint32_t mips = 1) {
  asset::ImageData image;
  image.format = format;
  image.interpretation = interpretation;
  image.extent = extent;
  image.array_layers = layers;
  image.mip_levels = mips;
  auto layout = terreate::unwrap(asset::packedLayout(format, extent, mips, layers));
  image.data.resize(layout.byte_size);
  for (std::size_t i = 0; i < image.data.size(); ++i)
    image.data[i] = static_cast<std::byte>((i * 53U + 5U) & 0xFFU);
  image.subresources = std::move(layout.subresources);
  return image;
}

void defaultsAreUnchanged() {
  check(ag::ImageUploadDescription{}.mip_generation == ag::MipGeneration::none,
        "the explicit description defaults to none");
  check(ag::ImageUploadConvenienceDescription{}.mip_generation == ag::MipGeneration::none,
        "the convenience description defaults to none");
  check(ag::ImageUploadDescription{}.mip_options.levels == 0 &&
            ag::ImageUploadDescription{}.mip_options.renormalize_normals,
        "the options default to the full chain with renormalization");

  // A single-level image stays single-level, and a chain the application made is uploaded as is.
  const auto base =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {5, 3, 1});
  const auto plan = ag::planImageUpload(base);
  if (check(bool(plan), "default plan")) {
    check(plan->mip_generation == ag::MipGeneration::none && !plan->generated &&
              plan->image.mip_levels == 1 && plan->subresources.size() == 1,
          "none generates nothing and describes the levels present");
  }
  const auto chain =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {5, 3, 1}, 1, 3);
  const auto own = ag::planImageUpload(chain);
  check(own && own->image.mip_levels == 3 && !own->generated && own->subresources.size() == 3,
        "a hand-made chain is planned as it is");
}

void planDescribesGeneratedLevels() {
  const auto base =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {5, 3, 1}, 2);
  ag::ImageUploadDescription description;
  description.mip_generation = ag::MipGeneration::cpu_box;
  const auto plan = ag::planImageUpload(base, description);
  const auto expected = asset::generateMipChain(base);
  if (!check(bool(plan) && bool(expected), "cpu_box plan"))
    return;
  check(plan->mip_generation == ag::MipGeneration::cpu_box, "the plan records the choice");
  check(plan->generated.has_value() && generatedOf(*plan).data == expected->data &&
            generatedOf(*plan).subresources == expected->subresources &&
            asset::validate(*plan->generated).has_value(),
        "the plan owns the chain generateMipChain makes");
  check(plan->format == vk::Format::eR8G8B8A8Srgb && plan->image.mip_levels == 3 &&
            plan->image.array_layers == 2 && plan->image.extent.width == 5 &&
            plan->image.extent.height == 3,
        "the image description has the generated levels");
  if (!check(plan->subresources.size() == 6, "one entry per generated (mip, layer)"))
    return;
  const std::uint32_t widths[3] = {5, 2, 1};
  const std::uint32_t heights[3] = {3, 1, 1};
  for (std::size_t i = 0; i < 6; ++i) {
    const auto &entry = plan->subresources[i];
    const std::uint32_t mip = static_cast<std::uint32_t>(i % 3);
    const auto *found = expected->find(mip, static_cast<std::uint32_t>(i / 3));
    check(entry.mip == mip && entry.layer == i / 3 && found != nullptr &&
              entry.data_offset == found->offset && entry.row_pitch == found->row_pitch &&
              entry.byte_count == static_cast<std::size_t>(widths[mip]) * 4U * heights[mip] &&
              entry.region.imageExtent.width == widths[mip] &&
              entry.region.imageExtent.height == heights[mip] &&
              entry.region.imageSubresource.mipLevel == mip &&
              entry.region.bufferRowLength == widths[mip],
          "subresource " + std::to_string(i) + " describes the generated level");
  }

  // The lower-level equivalent gives the same plan.
  const auto manual = ag::planImageUpload(*expected);
  check(manual && manual->image.mip_levels == plan->image.mip_levels &&
            manual->subresources.size() == plan->subresources.size() && !manual->generated,
        "planning the generated image by hand gives the same levels");

  // The source is only read: the plan does not keep a reference to it.
  auto copy = base;
  const auto detached = ag::planImageUpload(copy, description);
  copy.data.clear();
  check(detached && detached->generated && generatedOf(*detached).data == expected->data,
        "the plan stays valid after the source is gone");
}

void optionsReachTheFilter() {
  const auto base =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::linear, {8, 8, 1});
  ag::ImageUploadDescription description;
  description.mip_generation = ag::MipGeneration::cpu_box;
  description.mip_options.levels = 2;
  auto plan = ag::planImageUpload(base, description);
  check(plan && plan->image.mip_levels == 2 && plan->subresources.size() == 2 && plan->generated &&
            generatedOf(*plan).mip_levels == 2,
        "an option for fewer levels");

  auto normals =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::normal_map, {4, 4, 1});
  description.mip_options = {};
  const auto renormalized = ag::planImageUpload(normals, description);
  description.mip_options.renormalize_normals = false;
  const auto plain = ag::planImageUpload(normals, description);
  const auto expected_plain = asset::generateMipChain(normals, {.renormalize_normals = false});
  const auto expected_renormalized = asset::generateMipChain(normals);
  check(renormalized && plain && expected_plain && expected_renormalized &&
            generatedOf(*renormalized).data == expected_renormalized->data &&
            generatedOf(*plain).data == expected_plain->data &&
            generatedOf(*plain).data != generatedOf(*renormalized).data,
        "renormalize_normals reaches the filter");

  // Float and 16-bit formats work too.
  for (const auto format :
       {asset::PixelFormat::r16, asset::PixelFormat::rg16, asset::PixelFormat::rgba16,
        asset::PixelFormat::r32f, asset::PixelFormat::rg32f, asset::PixelFormat::rgba32f,
        asset::PixelFormat::r8, asset::PixelFormat::rg8}) {
    ag::ImageUploadDescription named;
    named.mip_generation = ag::MipGeneration::cpu_box;
    asset::ImageData image = makeImage(format, asset::ColorInterpretation::linear, {6, 5, 1});
    if (asset::isFloat(format))
      for (std::size_t i = 0; i < image.data.size(); i += 4) {
        const float value = static_cast<float>(i % 17) * 0.25F;
        std::memcpy(image.data.data() + i, &value, sizeof value);
      }
    const auto result = ag::planImageUpload(image, named);
    check(result && result->image.mip_levels == 3 && result->generated &&
              asset::validate(*result->generated).has_value(),
          "cpu_box plan of format " + std::to_string(static_cast<int>(format)));
  }
}

void errorsPassThrough() {
  ag::ImageUploadDescription description;
  description.mip_generation = ag::MipGeneration::cpu_box;

  const auto chain =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {4, 4, 1}, 1, 3);
  check(hasError(ag::planImageUpload(chain, description), asset::MipChainError::already_has_mips),
        "an image with mips is refused with the MipChainError, unchanged");
  const auto base =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {4, 4, 1});
  description.mip_options.levels = 4;
  check(hasError(ag::planImageUpload(base, description), asset::MipChainError::invalid_level_count),
        "too many levels is the MipChainError, unchanged");
  description.mip_options = {};

  auto broken = base;
  broken.data.pop_back();
  check(
      hasError(ag::planImageUpload(broken, description), asset::ImageDataError::data_out_of_bounds),
      "an invalid image is the ImageDataError, unchanged");

  const auto rgb =
      makeImage(asset::PixelFormat::rgb8, asset::ColorInterpretation::linear, {4, 4, 1});
  check(hasError(ag::planImageUpload(rgb, description), ag::ImageUploadError::unsupported_format),
        "rgb8 is unsupported_format, as without generation");
  const auto volume =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::linear, {2, 2, 2});
  check(hasError(ag::planImageUpload(volume, description), ag::ImageUploadError::unsupported_image),
        "a 3D image is unsupported_image, as without generation");

  // An undefined choice is refused, not treated as a request.
  ag::ImageUploadDescription forged;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  forged.mip_generation = static_cast<ag::MipGeneration>(2);
  check(hasError(ag::planImageUpload(base, forged), ag::ImageUploadError::unsupported_option),
        "an undefined mip_generation is unsupported_option");
}

void convenienceCarriesTheChoice() {
  const auto base =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {5, 3, 1});
  const auto plain = ag::planImageUploadWithDefaults(base, {});
  check(plain && plain->mip_generation == ag::MipGeneration::none && !plain->generated &&
            plain->image.mip_levels == 1,
        "the convenience generates nothing by default");

  ag::ImageUploadConvenienceDescription description;
  description.mip_generation = ag::MipGeneration::cpu_box;
  description.mip_options.levels = 2;
  const auto converted = ag::toImageUploadDescription(description);
  check(converted.mip_generation == ag::MipGeneration::cpu_box && converted.mip_options.levels == 2,
        "the conversion carries the choice and the options");
  const auto plan = ag::planImageUploadWithDefaults(base, description);
  const auto explicit_plan = ag::planImageUpload(base, converted);
  check(plan && explicit_plan && plan->image.mip_levels == 2 && plan->generated &&
            generatedOf(*plan).data == generatedOf(*explicit_plan).data &&
            plan->final_layout == vk::ImageLayout::eShaderReadOnlyOptimal,
        "the convenience plan equals the explicit plan of the converted description");
  const auto chain =
      makeImage(asset::PixelFormat::rgba8, asset::ColorInterpretation::srgb, {4, 4, 1}, 1, 3);
  check(hasError(ag::planImageUploadWithDefaults(chain, description),
                 asset::MipChainError::already_has_mips),
        "the convenience passes the MipChainError through");
}

} // namespace

int main() {
  defaultsAreUnchanged();
  planDescribesGeneratedLevels();
  optionsReachTheFilter();
  errorsPassThrough();
  convenienceCarriesTheChoice();
  if (failures != 0)
    return EXIT_FAILURE;
  std::printf("%s: plan=PASS\n", test_name);
  return EXIT_SUCCESS;
}
