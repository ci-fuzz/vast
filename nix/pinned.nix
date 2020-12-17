# See https://nixos.wiki/wiki/FAQ/Pinning_Nixpkgs for more information on pinning
builtins.fetchTarball {
  # Descriptive name to make the store path easier to identify
  name = "nixpkgs-2020-11-18";
  # Commit hash for nixpkgs as of date
  url = https://github.com/NixOS/nixpkgs/archive/00941cd747e9bc1c3326d1362dbc7e9cfe18cf53.tar.gz;
  # Hash obtained using `nix-prefetch-url --unpack <url>`
  sha256 = "12mjfar2ir561jxa1xvw6b1whbqs1rq59byc87icql399zal5z4a";
}
