{
  description = "Reproducible Terreate C++23/GCC/Vulkan development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    # cgltf (single-header glTF parser, MIT) is not in nixpkgs. It is pinned by
    # flake.lock as a non-flake source tree and is the backend of the optional
    # AdapterAssetGltfCgltf component (#278).
    cgltf = {
      url = "github:jkuhlmann/cgltf";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, cgltf }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        vulkanHeaders = pkgs.vulkan-headers;
        vulkanLoader = pkgs.vulkan-loader;
        vulkanValidationLayers = pkgs.vulkan-validation-layers;
        vulkanTools = pkgs.vulkan-tools;
        vulkanRuntime = pkgs.mesa;
        # stb_image (single-header, public domain / MIT) is the backend of the
        # optional AdapterAssetImageStb component (#277). It is a build-time
        # input of that one static library only, never of the CMake package.
        stbHeaders = pkgs.stb;
      in {
        devShells.default = pkgs.mkShell {
          # GCC/g++ is the project compiler.  The generic compiler and
          # clangd/clang-tidy/clang-format remain externally managed and are
          # intentionally not duplicated by this project shell.
          CC = "gcc";
          CXX = "g++";
          VULKAN_HEADERS_INCLUDE = "${vulkanHeaders}/include";
          # The directory that contains stb_image.h (nixpkgs installs it in
          # include/stb); read by the AdapterAssetImageStb configure step.
          STB_INCLUDE = "${stbHeaders}/include/stb";
          # The directory that contains cgltf.h (the root of the pinned flake
          # input); read by the AdapterAssetGltfCgltf configure step.
          CGLTF_INCLUDE = "${cgltf}";

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

            # Documentation generation (TERREATE_BUILD_DOCS): Doxygen for the API
            # reference, Sphinx (Furo, MyST, breathe) for the pages; shell tools only,
            # never a dependency of the CMake package.
            # Compiler cache: the CMake-driven tests compile the same sources several
            # times, so a cache makes the gate and ordinary rebuilds much faster.
            ccache

            doxygen
            (python3.withPackages (ps: [ ps.sphinx ps.furo ps.myst-parser ps.breathe ]))

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
            stbHeaders

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

            # CMake reads CMAKE_CXX_COMPILER_LAUNCHER from the environment, so the
            # preset and every configure started by a test use the cache. The
            # invoking environment may override each value, and an empty
            # CMAKE_CXX_COMPILER_LAUNCHER turns the cache off.
            export CMAKE_CXX_COMPILER_LAUNCHER="''${CMAKE_CXX_COMPILER_LAUNCHER-ccache}"
            export CCACHE_MAXSIZE="''${CCACHE_MAXSIZE-5G}"
            # Hash the compiler wrapper by content, not by size and time (every file in the
            # Nix store has the same time), so a new compiler never reuses old objects.
            export CCACHE_COMPILERCHECK="''${CCACHE_COMPILERCHECK-content}"
          '';
        };
      });
}
