{
  description = "Reproducible Terreate C++23/GCC/Vulkan development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        vulkanHeaders = pkgs.vulkan-headers;
        vulkanLoader = pkgs.vulkan-loader;
        vulkanValidationLayers = pkgs.vulkan-validation-layers;
        vulkanTools = pkgs.vulkan-tools;
        vulkanRuntime = pkgs.mesa;
      in {
        devShells.default = pkgs.mkShell {
          # GCC/g++ is the project compiler.  The generic compiler and
          # clangd/clang-tidy/clang-format remain externally managed and are
          # intentionally not duplicated by this project shell.
          CC = "gcc";
          CXX = "g++";
          VULKAN_HEADERS_INCLUDE = "${vulkanHeaders}/include";

          packages = with pkgs; [
            # Build and repository tooling.
            cmake
            ninja
            just
            pkg-config
            python3
            git
            gh
            jq
            gdb
            gnused

            # Vulkan headers, loader, validation, tools, and a software ICD
            # make the API/runtime surface observable in a clean shell.
            vulkanHeaders
            vulkanLoader
            vulkanValidationLayers
            vulkanTools
            vulkanRuntime

            # Project-owned C++ development dependencies.  These are shell
            # inputs only; CMake consumers choose their runtime link targets.
            glfw
            vulkan-memory-allocator
            glm
            spdlog

            # Shader and SPIR-V authoring/inspection tools.
            shader-slang
            shaderc
            glslang
            spirv-tools
            spirv-cross

            # GLFW/Vulkan window-system development headers and libraries.
            wayland
            wayland-protocols
            libxkbcommon
            libX11
            libXcursor
            libXi
            libXinerama
            libXrandr
            libXext
            libXfixes
            libXxf86vm
            libxcb
            xorgproto
          ];

          shellHook = ''
            # Prepend the pinned validation layers while retaining any
            # validation layers supplied by the invoking environment.
            export VK_LAYER_PATH="${vulkanValidationLayers}/share/vulkan/explicit_layer.d''${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
            export TERREATE_VULKAN_LOADER="${vulkanLoader}"
            export TERREATE_VULKAN_VALIDATION_LAYERS="${vulkanValidationLayers}"
            export TERREATE_VULKAN_TOOLS="${vulkanTools}"
            export TERREATE_VULKAN_RUNTIME="${vulkanRuntime}"
          '';
        };
      });
}
