// #463 (images): the CPU side of the beginner image upload convenience. The documented
// default (the convenience description asks for the sampled layout, the explicit
// description keeps none), the plan that records it, and the errors passed through.
// No Vulkan Instance or device is created; no OS- or linker-specific mechanism is used.
#include <terreate/integration/asset_graphics/image_upload.hpp>
#include <terreate/integration/asset_graphics/image_upload_convenience.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace {
namespace asset = terreate::asset;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics.image_convenience_plan";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

static_assert(!std::is_copy_constructible_v<ag::ImageUploadSubmission>);
static_assert(std::is_move_constructible_v<ag::ImageUploadSubmission>);
static_assert(!std::is_copy_constructible_v<ag::ImageUploadResult>);

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

asset::ImageData
makeImage(asset::PixelFormat format = asset::PixelFormat::rgba8,
          asset::ColorInterpretation interpretation = asset::ColorInterpretation::srgb) {
  asset::ImageData image;
  image.format = format;
  image.interpretation = interpretation;
  image.extent = {4, 2, 1};
  const auto layout = terreate::unwrap(asset::packedLayout(format, image.extent, 1, 1));
  image.data.assign(layout.byte_size, std::byte{7});
  image.subresources = layout.subresources;
  return image;
}

void run() {
  const auto data = makeImage();

  // The explicit description keeps `none`; the convenience description asks for the
  // sampled layout.
  check(ag::ImageUploadDescription{}.layout_transition == ag::LayoutTransition::none,
        "the explicit description defaults to no transition");
  const ag::ImageUploadConvenienceDescription defaults;
  check(defaults.layout_transition == ag::LayoutTransition::shader_read_only,
        "the convenience description defaults to shader_read_only");
  check(defaults.consumer_stages == vk::PipelineStageFlagBits2::eFragmentShader,
        "the default consumer is the fragment shader");

  const auto plan = ag::planImageUploadWithDefaults(data, {});
  if (check(bool(plan), "the default plan")) {
    check(plan->layout_transition == ag::LayoutTransition::shader_read_only,
          "the plan records the transition flag");
    check(plan->final_layout == vk::ImageLayout::eShaderReadOnlyOptimal,
          "the plan states the sampled layout as final");
    check(plan->copy_layout == vk::ImageLayout::eTransferDstOptimal,
          "the copy layout is unchanged");
    check(plan->transition_barriers.size() == 1 &&
              plan->transition_barriers.front().new_layout ==
                  vk::ImageLayout::eShaderReadOnlyOptimal &&
              plan->transition_barriers.front().dst_stage ==
                  vk::PipelineStageFlagBits2::eFragmentShader,
          "the plan states one barrier to ShaderReadOnlyOptimal for the fragment shader");
    check(plan->format == vk::Format::eR8G8B8A8Srgb, "the srgb rgba8 format");
  }

  // The same as the explicit steps with the transition named.
  ag::ImageUploadDescription explicit_description;
  explicit_description.layout_transition = ag::LayoutTransition::shader_read_only;
  const auto explicit_plan = ag::planImageUpload(data, explicit_description);
  if (plan && check(bool(explicit_plan), "the explicit plan")) {
    check(explicit_plan->final_layout == plan->final_layout &&
              explicit_plan->subresources.size() == plan->subresources.size() &&
              explicit_plan->transition_barriers.size() == plan->transition_barriers.size() &&
              explicit_plan->image.usage == plan->image.usage &&
              explicit_plan->format == plan->format,
          "the convenience plan equals the explicit plan with the transition named");
  }

  // The application can say none, and name other consumer stages and usage.
  ag::ImageUploadConvenienceDescription none;
  none.layout_transition = ag::LayoutTransition::none;
  const auto none_plan = ag::planImageUploadWithDefaults(data, none);
  if (check(bool(none_plan), "the plan with none")) {
    check(none_plan->layout_transition == ag::LayoutTransition::none &&
              none_plan->transition_barriers.empty() &&
              none_plan->final_layout == vk::ImageLayout::eTransferDstOptimal,
          "none states no transition and keeps the copy layout");
  }
  ag::ImageUploadConvenienceDescription other;
  other.extra_usage = vk::ImageUsageFlagBits::eTransferSrc;
  other.consumer_stages = vk::PipelineStageFlagBits2::eComputeShader;
  const auto converted = ag::toImageUploadDescription(other);
  check(converted.extra_usage == other.extra_usage &&
            converted.consumer_stages == other.consumer_stages &&
            converted.layout_transition == ag::LayoutTransition::shader_read_only &&
            converted.mip_generation == ag::MipGeneration::none,
        "toImageUploadDescription carries every choice");
  const auto other_plan = ag::planImageUploadWithDefaults(data, other);
  if (check(bool(other_plan), "the plan with other usage and stages")) {
    check((other_plan->image.usage & vk::ImageUsageFlagBits::eTransferSrc) ==
              vk::ImageUsageFlagBits::eTransferSrc,
          "the extra usage reaches the image");
    check(other_plan->transition_barriers.size() == 1 &&
              other_plan->transition_barriers.front().dst_stage ==
                  vk::PipelineStageFlagBits2::eComputeShader,
          "the consumer stages reach the barrier");
  }

  // Errors are those of the explicit steps, unchanged.
  ag::ImageUploadConvenienceDescription no_stages;
  no_stages.consumer_stages = {};
  check(hasError(ag::planImageUploadWithDefaults(data, no_stages),
                 ag::ImageUploadError::unsupported_option),
        "empty consumer stages with the default transition are unsupported_option");
  no_stages.layout_transition = ag::LayoutTransition::none;
  check(bool(ag::planImageUploadWithDefaults(data, no_stages)),
        "empty consumer stages are ignored with none");
  check(hasError(ag::planImageUploadWithDefaults(makeImage(asset::PixelFormat::rgb8), {}),
                 ag::ImageUploadError::unsupported_format),
        "rgb8 is unsupported_format");
  auto zero = data;
  zero.extent = {0, 2, 1};
  check(hasError(ag::planImageUploadWithDefaults(zero, {}), asset::ImageDataError::zero_extent),
        "an invalid ImageData is the Asset error, unchanged");

  check(ag::make_error_code(ag::ImageUploadError::wait_timeout).category() ==
                ag::image_upload_error_category() &&
            !ag::make_error_code(ag::ImageUploadError::wait_timeout).message().empty(),
        "wait_timeout has a message in the image upload category");
}

} // namespace

int main() {
  run();
  if (failures != 0)
    return EXIT_FAILURE;
  std::printf("%s: plan=PASS\n", test_name);
  return EXIT_SUCCESS;
}
