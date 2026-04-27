{
  description = "CHIP-8 core for RVVM bare-metal — riscv64 freestanding, zig-cc toolchain";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            zig            # cc + linker + cross targets, the whole toolchain
            qemu           # quick smoke-test against qemu virt before RVVM
            llvmPackages.bintools  # llvm-objdump / llvm-objcopy / llvm-readelf
          ];

          shellHook = ''
            echo "chip-8 core: zig $(zig version)"
            echo "target: riscv64-freestanding-none, attached as RVVM mtd-physmap firmware"
          '';
        };
      });
}
