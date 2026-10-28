{
  description = "Killzone 3 PC port: reverse-engineering toolchain and builds";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      # Only these unfree packages are allowed: RPCS3 (licence includes unfree parts) and
      # steam-run (runs the user's Proton for tests). Everything else must be free.
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfreePredicate = pkg: builtins.elem (nixpkgs.lib.getName pkg) [
          "rpcs3" "steam-run" "steam" "steam-unwrapped" "steam-original"
        ];
      };

      python = pkgs.python3.withPackages (ps: with ps; [
        capstone   # PPU disassembly in scripts
        numpy      # fast binary scans (branch xrefs, pattern search)
        pillow     # screenshots and texture dumps
        pyelftools # ELF / PRX parsing after decryption
        lz4
        zstandard
        # ps3recomp tool pipeline (third_party/ps3recomp/requirements.txt)
        construct
        pycryptodome
        cryptography  # SELF decryption
        tabulate
        tqdm
      ]);

      # Tools for the analysis stage: no GPU, no network needed.
      reTools = with pkgs; [
        python
        bubblewrap      # tools/sandbox.sh
        tinyxxd binutils file hexyl
        ghidra          # PPU (PowerPC 64 BE) static analysis
        radare2         # quick disassembly / patch checks
        duckdb          # SQL over TSV/JSON analysis data
        (rizin.withPlugins (ps: [ ps.rz-ghidra ]))  # `pdg @ addr`: Ghidra decompiler per function, no project DB
        ffmpeg          # Bink video + ATRAC3 audio decode (asset previews)
        imagemagick
        p7zip
        git
        ruff            # tools/lint.sh: Python lint (changed files, pre-commit)
        shellcheck      # tools/lint.sh: shell scripts
        llvmPackages_19.clang-tools  # tools/lint.sh: clang-format for our C/C++ in port/ (not patches, generated code or third_party)
      ];

      # Runtime / graphics stage: reference emulator, GPU capture, headless display.
      runtimeTools = with pkgs; [
        rpcs3           # reference runtime, SELF decryption, behaviour oracle
        renderdoc       # frame capture of RPCS3's Vulkan output
        vulkan-tools    # vulkaninfo
        vulkan-validation-layers
        xvfb xvfb-run xdotool
        mesa-demos
      ];

      # Host-side build tools (recompiler, asset converters run on Linux).
      buildTools = with pkgs; [
        cmake ninja pkg-config
        clang_19 lld_19 llvmPackages_19.llvm
        shaderc spirv-tools vulkan-headers
        steam-run       # runs the user's Proton for tests (tools/proton-test.sh)
      ];

      # Windows x64 target: the shipped port is a Windows exe, tested only under Proton.
      win = pkgs.pkgsCross.mingwW64;

      # mkShell sets $out to <repo>/outputs/out and the cc wrapper adds an unquoted "-rpath $out/lib" to
      # NIX_LDFLAGS. The repo path contains a space, so every host compile-and-link in a dev shell failed
      # ("cannot find 3/outputs/out/lib"). Nothing is installed to $out, so drop that entry in every shell.
      fixLdflags = ''
        if [ -n "''${NIX_LDFLAGS-}" ] && [ -n "''${out-}" ]; then
          export NIX_LDFLAGS="''${NIX_LDFLAGS//"-rpath $out/lib"/}"
        fi
      '';
      # macOS build hosts (Apple silicon; nixpkgs drops x86_64-darwin in 26.11): the same `nix develop` shells for building, without the Linux-only
      # pieces (bubblewrap, steam-run/Proton, Xvfb, RPCS3, Ghidra). tools/sandbox.sh runs the command directly on
      # macOS. Running the Windows exe needs a Windows PC (or a Windows compatibility layer, untested).
      darwinShells = dsys:
        let
          dp = import nixpkgs { system = dsys; };
          dpython = dp.python3.withPackages (ps: with ps; [
            capstone numpy pillow pyelftools lz4 zstandard construct pycryptodome cryptography tabulate tqdm ]);
          dwin = dp.pkgsCross.mingwW64;
          dbase = [ dpython dp.git dp.tinyxxd dp.binutils dp.p7zip dp.ruff dp.shellcheck dp.llvmPackages_19.clang-tools ];
        in {
          re = dp.mkShell { packages = dbase; shellHook = fixLdflags; };
          ci = dp.mkShell { packages = dbase ++ [ dp.gnumake dp.cmake dp.ninja ]; shellHook = fixLdflags; };
          default = dp.mkShell { packages = dbase ++ [ dp.cmake dp.ninja dp.pkg-config ]; shellHook = fixLdflags; };
          win = dwin.mkShell {
            nativeBuildInputs = [ dp.cmake dp.ninja dp.pkg-config dp.shaderc dp.python3 dp.ccache ];
            buildInputs = [ dwin.SDL2 dwin.sdl3 ];   # sdl3: native pads (patch 4010, net 24); SDL2 is sdl2-compat over SDL3
            shellHook = fixLdflags;
          };
        };
    in {
      devShells = nixpkgs.lib.genAttrs [ "aarch64-darwin" ] darwinShells // {
      ${system} = {
        # `nix develop` : everything
        default = pkgs.mkShell {
          packages = reTools ++ runtimeTools ++ buildTools;
          shellHook = fixLdflags + ''
            export KZ3_ROOT="$PWD"
            export KZ3_GAME="$PWD/Game"
            export KZ3_WORK="$PWD/work"
            # NixOS exposes the NVIDIA Vulkan ICD here; no nixGL needed.
            export VK_ICD_FILENAMES=/run/opengl-driver/share/vulkan/icd.d/nvidia_icd.json
          '';
        };
        # `nix develop .#ci` : what the public repo's CI needs (lint, patch apply, host tests): no Ghidra/RPCS3/GPU tools.
        ci = pkgs.mkShell {
          packages = [ python pkgs.ruff pkgs.shellcheck pkgs.llvmPackages_19.clang-tools pkgs.git pkgs.gnumake pkgs.cmake pkgs.ninja pkgs.zlib ];   # zlib: test_host_composed links the overlay (-lz)
          shellHook = fixLdflags;
        };
        # `nix develop .#re` : lightweight shell for extraction / analysis
        re = pkgs.mkShell { packages = reTools; shellHook = fixLdflags; };
        # `nix develop .#win` : cross-compile the port to Windows x64 (MinGW-w64).
        # Vulkan is loaded at runtime (volk) from vulkan-1.dll, which Proton/the NVIDIA
        # driver provide, so no Windows Vulkan SDK is needed.
        win = win.mkShell {
          # ccache: tools/build-kz3.sh uses it when present (work/ccache per checkout; 2026-10-09).
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.shaderc pkgs.bubblewrap pkgs.python3 pkgs.ccache ];
          buildInputs = [ win.SDL2 win.sdl3 ];   # sdl3: native pads (patch 4010, net 24); SDL2 is sdl2-compat over SDL3
          shellHook = fixLdflags;
        };
      };
      };
    };
}
