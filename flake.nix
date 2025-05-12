{
  description = "Wayland Compositor + Vulkan Renderer dev shell";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };

        wlroots       = pkgs.wlroots_0_18;
        coreTools     = with pkgs; [
          clang_19 cmake ninja pkg-config
          wayland wayland-protocols libxkbcommon xwayland
          libffi libdrm mesa
          vulkan-loader vulkan-headers vulkan-tools
          vulkan-validation-layers glslang
          wlroots
        ];

        vscode        = pkgs.vscode;
        vscExtensions = with pkgs.vscode-extensions; [
          ms-vscode.cpptools
          ms-vscode.cmake-tools
          nixos.nix
        ];

        buildInputs   = coreTools ++ [vscode] ++ vscExtensions;

        pkgConfigPath  = pkgs.lib.makeSearchPath "lib/pkgconfig" coreTools;
        wlrootsInclude = "${wlroots}/include";
        wlrootsLib     = "${wlroots}/lib";
      in {
        devShell = pkgs.mkShell {
          name        = "nixtile";
          buildInputs = buildInputs;

          shellHook = ''
export CC=clang
export CXX=clang++
export PKG_CONFIG_PATH=${pkgConfigPath}
export VK_ICD_FILENAMES=${pkgs.vulkan-loader}/etc/vulkan/icd.d
export VK_LAYER_PATH=${pkgs.vulkan-validation-layers}/etc/vulkan/explicit_layer.d

# Eksponer wlroots for CMake
export WLROOTS_INCLUDE_DIR=${wlrootsInclude}
export WLROOTS_LIB_DIR=${wlrootsLib}

# Generer compile_commands.json
echo "🛠 Generating compile_commands.json..."
mkdir -p build
cd build
cmake .. \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWLROOTS_INCLUDE_DIR=$WLROOTS_INCLUDE_DIR \
  -DWLROOTS_LIB_DIR=$WLROOTS_LIB_DIR
cd ..
ln -sf build/compile_commands.json .

# Opprett VSCode workspace slik at src åpnes
cat > nixtile.code-workspace <<'EOF'
{
  "folders": [
    { "path": "src" }
  ],
  "settings": {
    // Relative path fra src til build
    "C_Cpp.default.compileCommands": "../build/compile_commands.json",
    "C_Cpp.default.intelliSenseMode": "linux-clang-x64",
    "C_Cpp.default.cppStandard": "c++20",
    "cmake.cmakePath": "cmake"
  }
}
EOF

# Alias slik at 'code' åpner workspace
alias code="code nixtile.code-workspace"="code nixtile.code-workspace"

echo "✅ Dev shell ready: compile_commands.json laget, VSCode workspace klar (åpner src)."
          '';
        };
      }
    );
}

