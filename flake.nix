{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
  };
  outputs =
    { self, nixpkgs }:
    {
      packages =
        nixpkgs.lib.genAttrs
          [
            "x86_64-linux"
            "aarch64-linux"
            "x86_64-darwin"
            "aarch64-darwin"
          ]
          (
            system:
            let
              pkgs = nixpkgs.legacyPackages.${system};
              llvm = pkgs.llvmPackages_19;
              kernel =
                variant: defconfig:
                {
                  fileMmap ? false,
                }:
                pkgs.stdenvNoCC.mkDerivation {
                  pname = if fileMmap then "linux-${variant}-file-mmap" else "linux-${variant}";
                  version = "7.1.5-wasm";
                  src = ./.;

                  nativeBuildInputs = with pkgs; [
                    perl
                    bc
                    bison
                    flex

                    pkg-config
                    ncurses

                    dtc
                    llvm.clang-unwrapped
                    llvm.lld
                    llvm.libllvm

                    wabt
                  ];

                  HOSTCC = "${llvm.clang}/bin/clang";
                  KBUILD_BUILD_TIMESTAMP = "1970-01-01 00:00:00 UTC";

                  enableParallelBuilding = true;
                  configurePhase =
                    if fileMmap then
                      ''
                        make HOSTCC=$HOSTCC -j$NIX_BUILD_CORES ${defconfig}
                        scripts/config --enable WASM_FILE_MMAP --enable WASM_FILE_MMAP_STREAM
                        make HOSTCC=$HOSTCC -j$NIX_BUILD_CORES olddefconfig
                        grep -qx 'CONFIG_WASM_FILE_MMAP=y' .config
                        grep -qx 'CONFIG_WASM_FILE_MMAP_STREAM=y' .config
                      ''
                    else
                      "make HOSTCC=$HOSTCC -j$NIX_BUILD_CORES ${defconfig}";
                  buildPhase = "make HOSTCC=$HOSTCC -j$NIX_BUILD_CORES vmlinux.wasm";
                  installPhase =
                    if fileMmap then
                      ''
                        mkdir -p $out
                        install -m 0644 vmlinux.wasm $out/vmlinux.wasm
                        install -m 0644 .config $out/kernel.config
                        install -m 0644 COPYING $out/COPYING
                        cp -R LICENSES $out/LICENSES
                        cat > $out/profile.txt <<'EOF'
                        This profile enables private file mmap copies.
                        The base configuration is ${defconfig}.
                        CONFIG_WASM_FILE_MMAP=y and CONFIG_WASM_FILE_MMAP_STREAM=y are enabled.
                        Readable regular files are copied into independent private mappings.
                        Later file writes are not reflected in these copies.
                        Shared, executable, and fixed mappings remain unsupported by this copy ABI.
                        Execution requires a matching host and user-module execution ABI.
                        EOF
                      ''
                    else
                      "cp vmlinux.wasm $out";
                };
            in
            rec {
              wasm32 = kernel "wasm32" "wasm32_defconfig" { };
              wasm64 = kernel "wasm64" "wasm64_defconfig" { };
              wasm32-file-mmap = kernel "wasm32" "wasm32_defconfig" { fileMmap = true; };
              wasm64-file-mmap = kernel "wasm64" "wasm64_defconfig" { fileMmap = true; };
              default = wasm32;
            }
          );
    };
}
