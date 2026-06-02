let
  pkgs = import <nixpkgs> { };

  # Pebble's `pebble-tool` is a Python app that we install into a project-local
  # venv (see shellHook). These are the system Python deps it pulls in.
  pythonEnv = pkgs.python3.withPackages (ps: with ps; [
    pip
    setuptools
    virtualenv
    wheel
    # Common deps used by pebble-tool & the SDK build scripts; installing them
    # at the system level avoids slow pip builds inside the venv.
    pyserial
    pypng
    pyyaml
    requests
    websocket-client
    pillow
    gevent
    pygments
    sh
  ]);
in
pkgs.mkShell {
  name = "pebble-dev";

  buildInputs = with pkgs; [
    # Python stack for pebble-tool / waf build
    pythonEnv

    # ARM cross toolchain. The SDK ships its own (which we patchelf), but
    # having one on PATH is useful for poking at .elf files with objdump/gdb.
    gcc-arm-embedded

    # JS side: appinfo, Rocky.js, PebbleKit JS
    nodejs_20

    # Emulator (qemu-pebble) runtime libs
    SDL2
    SDL2.dev
    libffi
    pixman
    glib
    libpulseaudio
    sndio
    alsa-lib
    systemd            # libudev for qemu-pebble
    bzip2

    # USB / serial for installing onto a real watch
    libusb1

    # Image & font tooling used by the resource pipeline
    freetype
    libpng
    pkg-config

    # Needed to patchelf the SDK's prebuilt binaries
    patchelf

    # General
    git
    wget
    curl
    unzip
    which
    file
  ];

  # Runtime libs the SDK's prebuilt binaries need on NixOS. The arm-none-eabi
  # toolchain we sidestep via symlinks to Nix's gcc-arm-embedded (see shellHook),
  # but qemu-pebble has no equivalent and must be patchelf'd against this set.
  NIX_LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [
    stdenv.cc.cc.lib
    zlib
    ncurses5
    libiconvReal       # GNU libiconv — Linux glibc-iconv only ships headers
    # qemu-pebble deps:
    glib
    pixman
    SDL2
    systemd            # libudev
    libpulseaudio
    sndio
    alsa-lib
    bzip2
  ]);
  NIX_LD = "${pkgs.stdenv.cc.libc}/lib/ld-linux-x86-64.so.2";

  shellHook = ''
    # Pebble tooling state lives inside the project so different projects can
    # pin different SDK versions.
    export PEBBLE_HOME="$PWD/.pebble"
    mkdir -p "$PEBBLE_HOME"

    # Project-local venv for pebble-tool. First run: `pip install pebble-tool`.
    if [ ! -d "$PEBBLE_HOME/venv" ]; then
      echo "[shell.nix] Creating Pebble venv at $PEBBLE_HOME/venv"
      python -m venv "$PEBBLE_HOME/venv"
    fi
    # shellcheck disable=SC1091
    source "$PEBBLE_HOME/venv/bin/activate"
    export PATH="$PEBBLE_HOME/venv/bin:$PATH"
    export LD_LIBRARY_PATH="$NIX_LD_LIBRARY_PATH:$LD_LIBRARY_PATH"

    # The SDK's waf picks up host CC/CXX/AR/LD if set, then tries -mthumb
    # against host gcc and explodes. Unset so it falls back to its own
    # arm-none-eabi toolchain.
    unset CC CXX AR AS LD RANLIB
    unset CC_FOR_TARGET CXX_FOR_TARGET AR_FOR_TARGET AS_FOR_TARGET LD_FOR_TARGET RANLIB_FOR_TARGET

    # Fix up SDK binaries for NixOS. Two distinct problems:
    #
    # 1) The SDK's bundled arm-none-eabi-gcc (14.2) was built against
    #    libisl.so.15 and libmpfr.so.4 — sonames that nixpkgs no longer
    #    carries. Instead of patchelf'ing them, we replace the SDK's
    #    arm-none-eabi/bin/ dir with symlinks into Nix's gcc-arm-embedded.
    #    (The Nix toolchain self-resolves its own libexec/cc1 via $ORIGIN.)
    #
    # 2) qemu-pebble lives in toolchain/bin and IS still a stranger binary;
    #    patchelf its interpreter + rpath so it can run on NixOS.
    #
    # Both fixes are idempotent and gated on a sentinel per SDK install.
    NIX_ARM_BIN=$(dirname $(readlink -f $(which arm-none-eabi-gcc)))
    if [ -d "$HOME/.pebble-sdk/SDKs" ]; then
      for sdk_dir in "$HOME"/.pebble-sdk/SDKs/[0-9]*; do
        [ -d "$sdk_dir/toolchain" ] || continue
        sentinel="$sdk_dir/.nix-patched"
        if [ ! -f "$sentinel" ]; then
          echo "[shell.nix] Fixing SDK binaries in $sdk_dir/toolchain ..."
          # (1) swap arm-none-eabi/bin/* for symlinks to Nix's toolchain
          arm_bin="$sdk_dir/toolchain/arm-none-eabi/bin"
          if [ -d "$arm_bin" ] && [ ! -L "$arm_bin/arm-none-eabi-gcc" ]; then
            mv "$arm_bin" "''${arm_bin}.orig"
            mkdir -p "$arm_bin"
            for f in "$NIX_ARM_BIN"/arm-none-eabi-*; do
              ln -sf "$f" "$arm_bin/$(basename "$f")"
            done
          fi
          # (2) patchelf any remaining x86-64 ELFs (qemu-pebble etc.)
          while IFS= read -r f; do
            if file "$f" 2>/dev/null | grep -q "ELF 64-bit LSB executable, x86-64"; then
              interp=$(patchelf --print-interpreter "$f" 2>/dev/null || true)
              if [ -n "$interp" ] && [ "$interp" != "$NIX_LD" ]; then
                patchelf --set-interpreter "$NIX_LD" --set-rpath "$NIX_LD_LIBRARY_PATH" "$f" || true
              fi
            fi
          done < <(find "$sdk_dir/toolchain" -type f -executable -not -path "*/.orig/*")
          touch "$sentinel"
          echo "[shell.nix] Done."
        fi
      done
    fi

    echo "Pebble dev shell ready."
    echo "  arm-none-eabi-gcc (Nix):    $(arm-none-eabi-gcc --version | head -1)"
    if command -v pebble >/dev/null 2>&1; then
      echo "  pebble-tool:               $(pebble --version 2>&1 | head -1)"
    else
      echo "  pebble-tool: not installed yet. Run: pip install pebble-tool"
    fi
    echo
  '';
}
