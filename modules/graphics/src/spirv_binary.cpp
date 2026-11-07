#include <terreate/graphics/spirv_binary.hpp>

#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
constexpr std::uint32_t spirv_magic = 0x07230203U;
constexpr std::size_t header_words = 5;

class SpirvBinaryErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.spirv_binary"; }
  std::string message(int value) const override {
    switch (static_cast<SpirvBinaryError>(value)) {
    case SpirvBinaryError::invalid_size:
      return "invalid SPIR-V binary length";
    case SpirvBinaryError::invalid_magic:
      return "invalid SPIR-V magic word";
    case SpirvBinaryError::invalid_version:
      return "unsupported SPIR-V version header";
    case SpirvBinaryError::invalid_header:
      return "invalid SPIR-V header";
    case SpirvBinaryError::allocation_failed:
      return "SPIR-V storage allocation failed";
    }
    return "unknown SPIR-V binary error";
  }
};

auto fail(SpirvBinaryError error, const char *detail, const char *operation) {
  return std::unexpected(terreate::Error{make_error_code(error), operation, detail});
}

[[nodiscard]] terreate::Result<void> validateHeader(std::span<const std::uint32_t> words) {
  if (words.size() < header_words ||
      words.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
    return fail(SpirvBinaryError::invalid_size, "SPIR-V requires five or more 32-bit words",
                "SpirvBinary");
  if (words[0] != spirv_magic)
    return fail(SpirvBinaryError::invalid_magic, "SPIR-V magic must be 0x07230203", "SpirvBinary");
  const std::uint32_t version = words[1];
  const std::uint32_t major = (version >> 16U) & 0xffU;
  const std::uint32_t minor = (version >> 8U) & 0xffU;
  if (major != 1U || minor > 6U || (version & 0xff0000ffU) != 0)
    return fail(SpirvBinaryError::invalid_version, "SPIR-V 1.0 to 1.6 header required",
                "SpirvBinary");
  if (words[3] == 0U || words[4] != 0U)
    return fail(SpirvBinaryError::invalid_header, "nonzero id bound and zero schema required",
                "SpirvBinary");
  return {};
}
} // namespace

const std::error_category &spirv_binary_error_category() noexcept {
  static const SpirvBinaryErrorCategory category;
  return category;
}
std::error_code make_error_code(SpirvBinaryError error) noexcept {
  return {static_cast<int>(error), spirv_binary_error_category()};
}

SpirvBinary::SpirvBinary(SpirvBinary &&other) noexcept : words_(std::move(other.words_)) {
  // The standard promises only a valid unspecified vector state after move.
  // The public binary contract deliberately makes moved-from values invalid.
  other.words_.clear();
}
SpirvBinary &SpirvBinary::operator=(SpirvBinary &&other) noexcept {
  if (this != &other) {
    words_ = std::move(other.words_);
    other.words_.clear();
  }
  return *this;
}

auto SpirvBinary::fromWords(std::span<const std::uint32_t> words) -> terreate::Result<SpirvBinary> {
  if (auto result = validateHeader(words); !result)
    return std::unexpected(result.error());
  try {
    return SpirvBinary{std::vector<std::uint32_t>{words.begin(), words.end()}};
  } catch (const std::bad_alloc &) {
    return fail(SpirvBinaryError::allocation_failed, "cannot copy SPIR-V words",
                "SpirvBinary::fromWords");
  } catch (const std::length_error &) {
    return fail(SpirvBinaryError::allocation_failed, "SPIR-V word count exceeds vector capacity",
                "SpirvBinary::fromWords");
  }
}

auto SpirvBinary::fromBytes(std::span<const std::byte> bytes) -> terreate::Result<SpirvBinary> {
  if (bytes.size() < header_words * sizeof(std::uint32_t) ||
      bytes.size() % sizeof(std::uint32_t) != 0)
    return fail(SpirvBinaryError::invalid_size, "SPIR-V byte count must be >=20 and divisible by 4",
                "SpirvBinary::fromBytes");
  const auto readWord = [&bytes](std::size_t index) -> std::uint32_t {
    const auto offset = index * sizeof(std::uint32_t);
    std::uint32_t word = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
      word |= std::to_integer<std::uint32_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return word;
  };
  std::uint32_t header[header_words]{};
  for (std::size_t i = 0; i < header_words; ++i)
    header[i] = readWord(i);
  if (auto result = validateHeader(header); !result)
    return std::unexpected(result.error());
  try {
    std::vector<std::uint32_t> words;
    words.reserve(bytes.size() / sizeof(std::uint32_t));
    for (std::size_t i = 0; i < bytes.size() / sizeof(std::uint32_t); ++i)
      words.push_back(readWord(i));
    return SpirvBinary{std::move(words)};
  } catch (const std::bad_alloc &) {
    return fail(SpirvBinaryError::allocation_failed, "cannot copy SPIR-V bytes",
                "SpirvBinary::fromBytes");
  } catch (const std::length_error &) {
    return fail(SpirvBinaryError::allocation_failed, "SPIR-V word count exceeds vector capacity",
                "SpirvBinary::fromBytes");
  }
}

bool SpirvBinary::valid() const noexcept { return words_.size() >= header_words; }

auto SpirvBinary::toBytes() const -> terreate::Result<std::vector<std::byte>> {
  if (!valid())
    return fail(SpirvBinaryError::invalid_header, "SPIR-V value is moved-from",
                "SpirvBinary::toBytes");
  try {
    std::vector<std::byte> bytes(byteSize());
    for (std::size_t i = 0; i < words_.size(); ++i) {
      const auto word = words_[i];
      for (unsigned byte = 0; byte < 4; ++byte) {
        bytes[i * sizeof(std::uint32_t) + byte] =
            static_cast<std::byte>((word >> (byte * 8U)) & 0xffU);
      }
    }
    return bytes;
  } catch (const std::bad_alloc &) {
    return fail(SpirvBinaryError::allocation_failed, "cannot serialize SPIR-V bytes",
                "SpirvBinary::toBytes");
  } catch (const std::length_error &) {
    return fail(SpirvBinaryError::allocation_failed, "SPIR-V byte count exceeds vector capacity",
                "SpirvBinary::toBytes");
  }
}

} // namespace terreate::graphics
