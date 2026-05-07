const path = require('path');
const { WebpackManifestPlugin } = require('webpack-manifest-plugin');
const MiniCssExtractPlugin = require('mini-css-extract-plugin');
const ProgmemGenerator = require('./progmem-generator.js');
const TerserPlugin = require('terser-webpack-plugin');

// Where to write the baked WWWData.h. The consumer's sync_ui.py
// resolves the active esp-rack library location (sibling clone vs
// PIO-cloned at .pio/libdeps/<env>/ESPRack/) and exports the include
// path via ESPRACK_INCLUDE_DIR. We honor that env var; the relative-
// path fallback is the legacy local-dev hardcode and only fires when
// somebody runs `npm run build` outside the PIO-driven flow (rare).
function resolveProgmemOutputPath() {
  const fromEnv = process.env.ESPRACK_INCLUDE_DIR;
  if (fromEnv) {
    return path.join(fromEnv, 'WWWData.h');
  }
  return '../../esp-rack/lib/ESPRack/include/WWWData.h';
}

module.exports = function override(config, env) {
  if (env === "production") {
    // rename the ouput file, we need it's path to be short, for embedded FS
    config.output.filename = 'js/[id].[chunkhash:4].js';
    config.output.chunkFilename = 'js/[id].[chunkhash:4].js';

    // take out the manifest plugin
    config.plugins = config.plugins.filter((plugin) => !(plugin instanceof WebpackManifestPlugin));

    // shorten css filenames
    const miniCssExtractPlugin = config.plugins.find((plugin) => plugin instanceof MiniCssExtractPlugin);
    miniCssExtractPlugin.options.filename = "css/[id].[contenthash:4].css";
    miniCssExtractPlugin.options.chunkFilename = "css/[id].[contenthash:4].c.css";

    // don't emit license file
    const terserPlugin = config.optimization.minimizer.find((plugin) => plugin instanceof TerserPlugin);
    terserPlugin.options.extractComments = false;

    // build progmem data files — written into ESPRack's include path so
    // App.cpp's `#include <WWWData.h>` resolves. The library's .gitignore
    // excludes WWWData.h so it never gets committed.
    config.plugins.push(new ProgmemGenerator({
      outputPath: resolveProgmemOutputPath(),
      bytesPerLine: 20,
    }));
  }
  return config;
};
