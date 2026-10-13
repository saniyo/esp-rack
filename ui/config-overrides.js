const path = require('path');
const { WebpackManifestPlugin } = require('webpack-manifest-plugin');
const MiniCssExtractPlugin = require('mini-css-extract-plugin');
const ProgmemGenerator = require('./progmem-generator.js');
const TerserPlugin = require('terser-webpack-plugin');

// Where to write the baked WWWData.h (SES-LIB-003): the consumer's own
// <project>/.pio/www/, exported by its sync_ui.py as ESPRACK_WWW_DIR (and
// put on the include path there). ESPRACK_INCLUDE_DIR is the old scheme
// (bundle inside the shared library) — still honoured so a consumer whose
// scripts aren't migrated yet keeps building. A manual `npm run build` in
// <project>/interface/ writes to ../.pio/www/.
function resolveProgmemOutputPath() {
  if (process.env.ESPRACK_WWW_DIR) {
    return path.join(process.env.ESPRACK_WWW_DIR, 'WWWData.h');
  }
  if (process.env.ESPRACK_INCLUDE_DIR) {
    console.warn('[progmem] ESPRACK_INCLUDE_DIR (old scheme): writing into the shared library. '
      + 'Update scripts/ from esp-rack/scripts/consumer (SES-LIB-003).');
    return path.join(process.env.ESPRACK_INCLUDE_DIR, 'WWWData.h');
  }
  return '../.pio/www/WWWData.h';
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

    // build progmem data files — into the consumer's .pio/www/, which its
    // sync_ui.py prepends to the include path for App.cpp's
    // `#include <WWWData.h>`.
    config.plugins.push(new ProgmemGenerator({
      outputPath: resolveProgmemOutputPath(),
      bytesPerLine: 20,
    }));
  }
  return config;
};
