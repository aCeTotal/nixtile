{
  lib,
  fetchFromGitHub,
  installShellFiles,
  libX11,
  libinput,
  libxcb,
  libxkbcommon,
  pixman,
  pkg-config,
  stdenv,
  testers,
  wayland,
  wayland-protocols,
  wayland-scanner,
  wlroots,
  writeText,
  xcbutilwm,
  xwayland,
  # Boolean flags
  enableXWayland ? true,
  withCustomConfigH ? (configH != null),
  # Configurable options
  configH ?
    if conf != null then
      lib.warn ''
        conf parameter is deprecated;
        use configH instead
      '' conf
    else
      null,
  # Deprecated options
  # Remove them before next version of either Nixpkgs or nixtile itself
  conf ? null,
}:

assert withCustomConfigH -> (configH != null);
stdenv.mkDerivation (finalAttrs: {
  pname = "nixtile";
  version = "0.1-dev";

  src = fetchFromGitHub {
    owner = "aCeTotal";
    repo = "nixtile";
    rev = "f8a4943c78dc2438e7aee966a95a2c5b376b9d4a";
    sha256 = "sha256-1z3kqbnpx1n0z7z0cjiqgy00lp7v3vgq58fr8x9f24jr1qrv7qp7";
  };

  nativeBuildInputs = [
    installShellFiles
    pkg-config
    wayland-scanner
  ];

  buildInputs =
    [
      libinput
      libxcb
      libxkbcommon
      pixman
      wayland
      wayland-protocols
      wlroots
    ]
    ++ lib.optionals enableXWayland [
      libX11
      xcbutilwm
      xwayland
    ];

  outputs = [
    "out"
    "man"
  ];

  postPatch =
    let
      configFile =
        if lib.isDerivation configH || builtins.isPath configH then
          configH
        else
          writeText "config.h" configH;
    in
    lib.optionalString withCustomConfigH "cp ${configFile} config.h";
    
  postInstall = ''
    mkdir -p $out/share/wayland-sessions
    mkdir -p $out/share/applications
    cp -f nixtile.desktop $out/share/wayland-sessions/
    cp -f nixtile.desktop $out/share/applications/
  '';

  makeFlags =
    [
      "PKG_CONFIG=${stdenv.cc.targetPrefix}pkg-config"
      "WAYLAND_SCANNER=wayland-scanner"
      "PREFIX=$(out)"
      "MANDIR=$(man)/share/man"
      "DATADIR=$(out)/share"
    ]
    ++ lib.optionals enableXWayland [
      ''XWAYLAND="-DXWAYLAND"''
      ''XLIBS="xcb xcb-icccm"''
    ];

  strictDeps = true;

  __structuredAttrs = true;

  passthru = {
    tests.version = testers.testVersion {
      package = finalAttrs.finalPackage;
      # `nixtile -v` emits its version string to stderr and returns 1
      command = "nixtile -v 2>&1; return 0";
    };
  };

  meta = {
    homepage = "https://github.com/totalvoid/nixtile";
    description = "Dynamic tiling Wayland compositor based on wlroots";
    longDescription = ''
      nixtile is a compact, hackable compositor for Wayland based on wlroots. It is
      intended to fill the same space in the Wayland world that dwm does in X11,
      primarily in terms of philosophy, and secondarily in terms of
      functionality. Like dwm, nixtile is:

      - Easy to understand, hack on, and extend with patches
      - One C source file (or a very small number) configurable via config.h
      - Tied to as few external dependencies as possible
    '';
    license = lib.licenses.gpl3Only;
    maintainers = [ ];
    inherit (wayland.meta) platforms;
    mainProgram = "nixtile";
  };
})

