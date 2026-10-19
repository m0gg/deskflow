# Deskflow fork flake: builds this repo's source via the nixpkgs deskflow
# recipe, with the local changes that drifted from the v1.26.0 release tarball:
#
#   * the /usr/share/X11/xkb substituteInPlace is dropped: this fork no
#     longer references evdev.xml.
#   * the upstream postInstall docs (README.md, doc/user/configuration.md)
#     do not exist in this fork.
#   * tests are individual ctest tests now, not a single `legacytests` binary:
#     upstream's checkPhase runs ./bin/legacytests (which this fork does not
#     build) and passes -DSKIP_BUILD_TESTS=ON, an option this fork renamed to
#     BUILD_TESTS (default ON), so the checkPhase is replaced instead.
#
# Consumed by the nix-msca flake as input `deskflow-src` (nixpkgs follows the
# consumer's nixpkgs there).
{
  description = "deskflow (m0gg fork)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs, ... }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
      ];
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.deskflow.overrideAttrs (finalAttrs: prevAttrs: {
            # src/CMakeLists.txt of this fork is at 1.27.0
            version = "1.27.0-dev";

            src = self;

            postPatch = ''
              substituteInPlace deploy/linux/deploy.cmake \
                --replace-fail 'message(FATAL_ERROR "Unable to read file /etc/os-release")' 'set(RELEASE_FILE_CONTENTS "")'
            '';

            # This fork ships no README.md / doc/user/configuration.md
            postInstall = "";

            # Run the individual ctest tests; upstream's `./bin/legacytests`
            # binary is not built by this fork. checkPhase runs inside the cmake
            # build dir and this fork only calls enable_testing() in
            # src/unittests, hence the relative --test-dir below.
            # The EiKeyState tests compile real xkb layouts (de/neo_qwertz);
            # libxkbcommon has xkeyboard-config baked into its default include
            # path, so they find it in the sandbox.
            checkPhase = ''
              runHook preCheck
              export QT_QPA_PLATFORM=offscreen
              ctest --test-dir src/unittests --output-on-failure \
                --exclude-regex "OSX(KeyState|Clipboard)Tests"
              runHook postCheck
            '';
          });
        }
      );
    };
}
