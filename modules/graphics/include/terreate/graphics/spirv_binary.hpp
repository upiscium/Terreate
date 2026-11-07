#ifndef TERREATE_GRAPHICS_SPIRV_BINARY_HPP
#define TERREATE_GRAPHICS_SPIRV_BINARY_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <terreate/core/result.hpp>

namespace terreate::graphics {

enum class SpirvBinaryError : std::uint8_t {
  invalid_size = 1,
  invalid_magic,
  invalid_version,
  invalid_header,
  allocation_failed,
};

[[nodiscard]] const std::error_category &spirv_binary_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SpirvBinaryError error) noexcept;

/// CPU-owned SPIR-V words; no Device, shader compiler, reflection, stage or
/// entry-point inference. Only the five-word binary header is validated.
/// Full instruction/semantic validation is the caller's separate responsibility.
/// Copying owns independent storage; moved-from values are invalid.
/// The input words/bytes are snapshotted: no external memory is retained.
class SpirvBinary final {
public:
  SpirvBinary() = delete;
  SpirvBinary(const SpirvBinary &) = default;
  SpirvBinary &operator=(const SpirvBinary &) = default;
  SpirvBinary(SpirvBinary &&other) noexcept;
  SpirvBinary &operator=(SpirvBinary &&other) noexcept;
  ~SpirvBinary() = default;

  [[nodiscard]] static auto fromWords(std::span<const std::uint32_t> words)
      -> terreate::Result<SpirvBinary>;
  /// Byte input is explicitly little-endian, regardless of host byte order.
  [[nodiscard]] static auto fromBytes(std::span<const std::byte> bytes)
      -> terreate::Result<SpirvBinary>;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] std::span<const std::uint32_t> words() const noexcept { return words_; }
  [[nodiscard]] std::size_t wordCount() const noexcept { return words_.size(); }
  [[nodiscard]] std::size_t byteSize() const noexcept {
    return words_.size() * sizeof(std::uint32_t);
  }
  [[nodiscard]] std::uint32_t versionWord() const noexcept { return valid() ? words_[1] : 0; }
  /// Serialize an independently owned little-endian snapshot. Never expose
  /// native-endian mapped bytes of the word vector as SPIR-V wire bytes.
  [[nodiscard]] auto toBytes() const -> terreate::Result<std::vector<std::byte>>;

private:
  explicit SpirvBinary(std::vector<std::uint32_t> words) noexcept : words_(std::move(words)) {}
  std::vector<std::uint32_t> words_{};
};

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SpirvBinaryError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SPIRV_BINARY_HPP
