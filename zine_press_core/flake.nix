{
  description = "Zine Press CORE module: a PDF in, the files for printing an A5 saddle-stitched booklet on A4 out (Basecamp 0.3).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/3a64943fe6827bab4c4686fb35c37d24d5df0710";   # tag 0.3.3 (pairs with Basecamp 0.3.2), pinned to its commit
  };

  # mkLogosModule: a headless core module; the plugin glue is generated from
  # src/zine_press_core_impl.h (universal authoring).
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
