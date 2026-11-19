/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Copy the freshly built addon to prebuilds/<platform>-<arch>/, the layout
// node-gyp-build loads from. CI runs this once per target, then the publish job
// merges every target's prebuilds/ folder into one package.
//
//   node scripts/prebuild.js [--libc musl|glibc]
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const built = path.join(root, 'build', 'Release', 'datapath.node');
if (!fs.existsSync(built)) {
	console.error(`missing ${built}: run "npm run build" first`);
	process.exit(1);
}

const args = process.argv.slice(2);
const libcAt = args.indexOf('--libc');
const libc = libcAt >= 0 ? args[libcAt + 1] : undefined;
const arch = process.env.npm_config_arch || process.arch;
const platform = process.env.npm_config_platform || process.platform;

// node-gyp-build tags: <name>.napi[.<libc>].node; untagged libc matches any.
const tags = ['datapath', 'napi'];
if (libc) tags.push(libc);
const dir = path.join(root, 'prebuilds', `${platform}-${arch}`);
fs.mkdirSync(dir, { recursive: true });
const target = path.join(dir, `${tags.join('.')}.node`);
fs.copyFileSync(built, target);
console.log(`prebuild: ${path.relative(root, target)}`);
