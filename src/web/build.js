#!/usr/bin/env node
'use strict';

/*
 * TerebikkoEmu standalone web build.
 *
 * Reads the plain-JavaScript sources in src/web/ and assembles one self-contained
 * HTML file per build flavour. No runtime files are fetched by the generated pages:
 * CSS, the decoder worker, the engine and the controller are all inlined.
 *
 *   node src/web/build.js --release    -> web/index.html
 *   node src/web/build.js --debug      -> web/debug.html
 *   node src/web/build.js              -> both
 *
 * Options:
 *   --release             build the player page
 *   --debug               build the developer page
 *   --out <file>          output path (only valid for a single build)
 *   --build-id <id>       override the <meta name="terebikko-build"> value
 *   -h, --help            show usage
 */

const fs = require('fs');
const path = require('path');

const WEB_SRC = __dirname;
const REPO_ROOT = path.resolve(WEB_SRC, '..', '..');
const DEFAULT_OUTPUT = {
  release: path.join(REPO_ROOT, 'web', 'index.html'),
  debug: path.join(REPO_ROOT, 'web', 'debug.html')
};

const SOURCE = {
  page: path.join(WEB_SRC, 'templates', 'page.html'),
  body: path.join(WEB_SRC, 'templates', 'body.html'),
  screen: path.join(WEB_SRC, 'templates', 'screen.html'),
  aside: path.join(WEB_SRC, 'templates', 'aside.html'),
  settingsModal: path.join(WEB_SRC, 'templates', 'settings-modal.html'),
  stateModal: path.join(WEB_SRC, 'templates', 'state-modal.html'),
  debugConfig: path.join(WEB_SRC, 'templates', 'debug-config.html'),
  worker: path.join(WEB_SRC, 'js', 'decoder-worker.js'),
  engine: path.join(WEB_SRC, 'js', 'engine.js'),
  controller: path.join(WEB_SRC, 'js', 'controller.js'),
  css: path.join(WEB_SRC, 'styles', 'app.css'),
  pickupCoin: path.join(REPO_ROOT, 'pickupCoin.json')
};

const USAGE = `TerebikkoEmu web build

Usage:
  node src/web/build.js [--release] [--debug] [options]

Options:
  --release             build the release player (default output: web/index.html)
  --debug               build the developer page (default output: web/debug.html)
  --out <file>          write to a custom path (single build only)
  --build-id <id>       override the generated build identifier
  -h, --help            show this help

With no flavour selected, both release and debug are built.
`;

function readText(file) {
  return fs.readFileSync(file, 'utf8');
}

function readPickupCoinPreset() {
  try {
    return JSON.parse(readText(SOURCE.pickupCoin));
  } catch (err) {
    if (err && err.code === 'ENOENT') return null;
    process.stderr.write('warning: could not read pickupCoin.json: ' + (err.message || err) + '\n');
    return null;
  }
}

function parseArgs(argv) {
  const options = { builds: [], out: null, buildId: null, help: false };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--release') {
      options.builds.push('release');
    } else if (arg === '--debug') {
      options.builds.push('debug');
    } else if (arg === '--all') {
      options.builds.push('release', 'debug');
    } else if (arg === '--out') {
      if (i + 1 >= argv.length) throw new Error('--out needs a file path');
      options.out = argv[++i];
    } else if (arg === '--build-id') {
      if (i + 1 >= argv.length) throw new Error('--build-id needs a value');
      options.buildId = argv[++i];
    } else if (arg === '-h' || arg === '--help') {
      options.help = true;
    } else {
      throw new Error('unknown option: ' + arg);
    }
  }
  options.builds = Array.from(new Set(options.builds));
  if (!options.help && !options.builds.length) options.builds = ['release', 'debug'];
  if (!options.help && options.out && options.builds.length !== 1) {
    throw new Error('--out can only be used with one build flavour');
  }
  return options;
}

function defaultBuildId(flavour) {
  let stamp = new Date().toISOString().slice(0, 10);
  const epoch = Number(process.env.SOURCE_DATE_EPOCH);
  if (Number.isFinite(epoch) && epoch > 0) {
    stamp = new Date(epoch * 1000).toISOString().slice(0, 10);
  }
  return 'terebikko-' + flavour + '-' + stamp;
}

// Replace a marker exactly once. Using split/join avoids treating '$' sequences in
// the inserted JavaScript as String.replace replacement patterns.
function replaceOnce(text, marker, value) {
  const parts = text.split(marker);
  if (parts.length !== 2) {
    throw new Error('expected exactly one ' + marker + ' marker, found ' + (parts.length - 1));
  }
  return parts[0] + value + parts[1];
}

function jsonForScript(value) {
  return JSON.stringify(value)
    .replace(/</g, '\\u003c')
    .replace(/\u2028/g, '\\u2028')
    .replace(/\u2029/g, '\\u2029');
}

function assertInlineSafe(value, name) {
  if (/<\/script/i.test(value)) {
    throw new Error(name + ' contains a closing script tag and cannot be inlined safely');
  }
}

function renderBody(debug, templates) {
  const heading = debug ? 'TerebikkoEmu — Debug' : 'TerebikkoEmu';
  const subtitle = debug
    ? 'Developer build: same generic decoder and game engine as the release player, plus the decoded event timeline, per-channel report, and direct event jumping.'
    : "Drop a Terebikko or See 'N Say video and play along with the four-button phone. Hard mode turns it into a Dragon's-Lair quick-time challenge.";
  const debugBar = debug
    ? '<button class="btn" id="nextBtn" title="Jump to next decoded event">Next event</button>'
    : '';

  const screen = replaceOnce(templates.screen, '{{DEBUG_BAR}}', debugBar);
  let body = templates.body;
  body = replaceOnce(body, '{{HEADING}}', heading);
  body = replaceOnce(body, '{{SUBTITLE}}', subtitle);
  body = replaceOnce(body, '{{SCREEN}}', screen);
  body = replaceOnce(body, '{{DEBUG_PANEL}}', debug ? templates.debugConfig : '');
  body = replaceOnce(body, '{{ASIDE}}', templates.aside);
  body = replaceOnce(body, '{{SETTINGS_MODAL}}', templates.settingsModal);
  body = replaceOnce(body, '{{STATE_MODAL}}', templates.stateModal);
  return body;
}

function renderPage(flavour, buildId, pickupCoinPreset) {
  const debug = flavour === 'debug';
  const templates = {
    page: readText(SOURCE.page),
    body: readText(SOURCE.body),
    screen: readText(SOURCE.screen),
    aside: readText(SOURCE.aside),
    settingsModal: readText(SOURCE.settingsModal),
    stateModal: readText(SOURCE.stateModal),
    debugConfig: readText(SOURCE.debugConfig)
  };
  const body = renderBody(debug, templates);
  const css = readText(SOURCE.css);
  const worker = readText(SOURCE.worker);
  const engine = readText(SOURCE.engine);
  const controller = readText(SOURCE.controller);

  if (/<\/style/i.test(css)) throw new Error('app.css contains a closing style tag');
  assertInlineSafe(worker, 'decoder-worker.js');
  assertInlineSafe(engine, 'engine.js');
  assertInlineSafe(controller, 'controller.js');

  let html = templates.page;
  html = replaceOnce(html, '{{TITLE}}', debug ? 'TerebikkoEmu — Debug' : 'TerebikkoEmu');
  html = replaceOnce(html, '{{BUILD_ID}}', buildId);
  html = replaceOnce(html, '{{CSS}}', css);
  html = replaceOnce(html, '{{BODY}}', body);
  html = replaceOnce(html, '{{DECODER_WORKER}}', worker);
  html = replaceOnce(html, '{{DEBUG}}', debug ? 'true' : 'false');
  html = replaceOnce(html, '{{PICKUP_COIN_PRESET}}', jsonForScript(pickupCoinPreset));
  html = replaceOnce(html, '{{ENGINE}}', engine);
  html = replaceOnce(html, '{{CONTROLLER}}', controller);

  const unresolved = html.match(/\{\{[A-Z_]+\}\}/);
  if (unresolved) throw new Error('unresolved template marker: ' + unresolved[0]);
  return html;
}

function writeFile(file, contents) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, contents);
  const relative = path.relative(REPO_ROOT, file);
  const label = relative && !relative.startsWith('..') && !path.isAbsolute(relative) ? relative : file;
  console.log('wrote ' + label);
}

function main(argv) {
  const options = parseArgs(argv);
  if (options.help) {
    process.stdout.write(USAGE);
    return;
  }

  const pickupCoinPreset = readPickupCoinPreset();
  for (const flavour of options.builds) {
    const output = options.out || DEFAULT_OUTPUT[flavour];
    const buildId = options.buildId || defaultBuildId(flavour);
    const html = renderPage(flavour, buildId, pickupCoinPreset);
    writeFile(path.resolve(output), html);
  }
}

try {
  main(process.argv.slice(2));
} catch (err) {
  process.stderr.write('web build failed: ' + (err && err.message ? err.message : String(err)) + '\n');
  process.exitCode = 1;
}
