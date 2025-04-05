{
  description = "NixTile Dev Flake";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "nixtile";
          version = "0.0.1";
          src = ./.;

          nativeBuildInputs = with pkgs; [ cmake pkg-config ninja ];
          buildInputs = with pkgs; [
            wayland
            wayland-protocols
            libdrm
            libinput
            mesa
            libxkbcommon
            udev

            vulkan-loader
            vulkan-headers
            glslang
            shaderc
            spirv-tools

            ffmpeg
          ];

          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Debug" ];
          installPhase = ''
            mkdir -p $out/bin
            cp nixtile $out/bin/
          '';
        };

        devShell = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            git

            wayland
            wayland-protocols
            libdrm
            libinput
            mesa
            libxkbcommon
            udev

            vulkan-loader
            vulkan-headers
            glslang
            shaderc
            spirv-tools

            ffmpeg
          ];

          shellHook = ''
            export VK_ICD_FILENAMES=${pkgs.vulkan-loader}/etc/vulkan/icd.d/nvidia_icd.json
            export VK_LAYER_PATH=${pkgs.vulkan-loader}/etc/vulkan/explicit_layer.d
            echo "[NixTile] Dev shell is ready for action"
          '';
        };
      });
}

