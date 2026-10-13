# Source-only flake so this checkout can be consumed as a `git+file://` input
# (used by the nix-msca flake: deskflow-src -> shared/deskflow.nix overrides
# pkgs.deskflow's src with this tree). Nothing is built from this flake itself;
# the flake's outPath is the copied working tree (gitignored paths like build/
# are excluded, uncommitted changes are included).
{
  description = "deskflow dev checkout (source-only flake)";

  outputs = { self }: {
    lib = { };
  };
}
