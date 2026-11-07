# Explicit precompiled SPIR-V and ShaderModule (#260)

This module is the minimal device-independent SPIR-V loading path and the
minimal Device-borrowing Vulkan ShaderModule RAII path. It does not own a
compiler, reflection service, Pipeline, DescriptorSet, RenderGraph, Queue,
frame scheduler, resource-use tracker or upload workflow.

## CPU-owned SPIR-V binary

SpirvBinary::fromWords and SpirvBinary::fromBytes snapshot caller input and
own storage independently. Word parsing accepts SPIR-V version 1.0–1.6 and
validates only the structural five-word header: magic 0x07230203, compatible
version field, nonzero id bound, zero schema and whole 32-bit words.
fromBytes decodes little-endian byte order explicitly into aligned words,
and toBytes serializes an independent little-endian snapshot.
Read-only observers include words(), wordCount(), byteSize(), versionWord().
Moving away from the binary invalidates the source.

Header acceptance is NOT proof of valid instructions, entry points, stage,
capabilities, environment, or pipeline compatibility. A later optional
validator or native Vulkan driver may reject them. No Shader Compiler or
Reflection backend is linked into Terreate::Graphics.

## Explicit Vulkan ShaderModule

createShaderModule(Device, SpirvBinary) requires a borrowed live Device and
a valid CPU binary. It invokes one synchronous vkCreateShaderModule with the
exact words and byte count, copying no source references into the owner.
Return type is terreate::Result<ShaderModule>. Native failures retain the
driver's original VkResult error; semantic validation and allocation failures
have their own diagnostic error categories.

ShaderModule is move-only RAII, never default-constructible. Each live value
owns exactly one native VkShaderModule. Borrowed observers include
nativeHandle(), nativeShaderModule(), nativeDevice(), correlatedWith(Device),
and configuration() with requested/effective code byte count and version.
Moved-from modules return null native handles/configuration. Shader stage and
entry-point are deliberately NOT inferred: they must be supplied explicitly
when later configuring a Pipeline in #267.

Device and its parent Instance must outlive ShaderModule. The application
is responsible for waiting for any GPU references before destroying or
move-replacing an owner. No automatic submit, wait, caching or scheduling.

## Tests and packaging

graphics.spirv_binary is CPU-only: byte/word snapshots, little-endian
round-trip, header negative cases, move-only-state observability and structured
errors. This test does not require a Vulkan Device or compiler.

graphics.shader_module uses the real headless Vulkan loader/Device with
a fixed Compute Shader SPIR-V word fixture, generated offline with
glslangValidator -V -S comp and validated using spirv-val --target-env
vulkan1.3. It observes native VkShaderModuleCreateInfo, native failure
propagation, move-only ownership and exactly-once creation/destruction.
When Validation Layer is available, a real Debug Utils WARNING|VALIDATION
callback probe verifies that warning/error categories reach a borrowed
application sink. Only the synthetic probe is subtracted; actual warnings
or errors cause a failed GPU test. Missing layer is a separately reported
validation-only SKIP; missing GPU/driver/Device is BLOCKED/nonzero and
never masquerades as a GPU PASS.

Installed Terreate::Graphics external consumer smoke includes both new
public headers, links CPU-only fromWords/fromBytes/toBytes and verifies
the ShaderModule factory is publicly reachable without runtime compiler
or reflection dependencies. Core-only install remains Vulkan-free.

## Extensions

Optional Shader Compiler adapters (#261) and reflection (#262), Descriptor
(#266), GraphicsPipeline (#267), and Dynamic Rendering/draw (#265) remain
separately planned work. This utility does not manage resource-use provenance,
GPU completion, queue selection, or device-lost recovery.
