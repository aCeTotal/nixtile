{
  description = "nixtile - tiling wayland compositor based on wlroots";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        nixtilePackage = pkgs.callPackage ./nix/nixtile/package.nix { };
      in {
        packages = {
          default = nixtilePackage;
          nixtile = nixtilePackage;
        };
        
        apps = {
          default = flake-utils.lib.mkApp {
            drv = nixtilePackage;
          };
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            pkgs.gcc
            pkgs.pkg-config
            pkgs.wayland
            pkgs.wlroots
            pkgs.libinput
            pkgs.xorg.libxcb
            pkgs.xorg.xcbutil
            pkgs.xorg.xcbutilwm
            pkgs.xorg.xcbutilkeysyms
            pkgs.xorg.xcbutilerrors
            pkgs.xorg.xcbutilimage
            pkgs.xorg.xcbutilrenderutil
            pkgs.xorg.xcbutilcursor
            pkgs.xorg.xorgproto
            pkgs.xorg.xkeyboardconfig
            pkgs.xorg.libX11
            pkgs.xorg.libXcursor
            pkgs.xorg.libXrandr
            pkgs.xorg.libXi
            pkgs.libxkbcommon
            pkgs.mesa
            pkgs.libGL
            pkgs.libglvnd
            pkgs.wayland-protocols
            pkgs.scdoc
            pkgs.gnumake
            pkgs.git
            pkgs.wayland-scanner
            pkgs.pixman
            pkgs.cage
          ];
          shellHook = ''
            # Ingen ekstra PKG_CONFIG_PATH nødvendig for wlroots
            echo "nixtile devShell ready!"
          '';
        };
      }
    );
}
