{
  description = "Zine Press Basecamp ui_qml module: a pure QML view over zine_press_core (Basecamp 0.3).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/16e2f6bd3c06a5119f8c34448933de183205646c";   # tag 0.3.1, pinned to its commit
    zine_press_core.url = "path:../zine_press_core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
