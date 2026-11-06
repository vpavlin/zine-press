{
  description = "Zine Press CORE module: a PDF in, the files for printing an A5 saddle-stitched booklet on A4 out (Basecamp 0.3).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/16e2f6bd3c06a5119f8c34448933de183205646c";   # tag 0.3.1, pinned to its commit
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
