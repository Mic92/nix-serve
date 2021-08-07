{
  inputs.nixpkgs.url = "nixpkgs/nixos-21.05";

  outputs = { self, nixpkgs }:

    let
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
      systems = [ "x86_64-linux" "aarch64-linux" "i686-linux" "x86_64-darwin" ];
    in {

      overlay = final: prev: {
        nix-serve = with final; let
          cpp-httplib = stdenv.mkDerivation rec {
            pname = "cpp-httplib";
            version = "0.9.2";
            src = fetchFromGitHub {
              owner = "yhirose";
              repo = "cpp-httplib";
              rev = "v${version}";
              sha256 = "sha256-BIZrH/kMokr7UTfbQcZXHDQKcAvE1Z/7/LlxSn40Oa4=";
            };
            installPhase = ''
              install -D httplib.h $out/include/httplib.h
            '';
            meta = with lib; {
              description = "A C++ header-only HTTP/HTTPS server and client library";
              homepage = "https://github.com/yhirose/cpp-httplib";
              license = licenses.mit;
              platforms = platforms.unix;
            };
          };
        in stdenv.mkDerivation {
          name = "nix-serve-${self.lastModifiedDate}";

          src = self;

          buildInputs = [
            nixUnstable
            boost
            nlohmann_json
            cpp-httplib
          ];
          installFlags = [ "PREFIX=$(out)" ];
          nativeBuildInputs = [ pkg-config ];
        };
      };

      packages = forAllSystems (system: {
        nix-serve = (import nixpkgs { inherit system; overlays = [ self.overlay ]; }).nix-serve;
      });

      defaultPackage = forAllSystems (system: self.packages.${system}.nix-serve);

      checks = forAllSystems (system: {
        build = self.defaultPackage.${system};
        # FIXME: add a proper test.
      });

    };
}
