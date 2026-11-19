/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// npm "prepack"/"postpack" hooks. The addon builds the C++ library from the
// repository root, which is outside this package. Before packing, copy the parts
// a source build needs into deps/datapath; after packing, remove the copy.
//
//   node scripts/vendor.js          copy
//   node scripts/vendor.js --clean  remove
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..');
const repo = path.join(root, '..', '..');
const dest = path.join(root, 'deps', 'datapath');

fs.rmSync(path.join(root, 'deps'), { recursive: true, force: true });
if (process.argv.includes('--clean')) {
	fs.rmSync(path.join(root, 'LICENSE'), { force: true });
	process.exit(0);
}

const entries = ['CMakeLists.txt', 'LICENSE', 'cmake/version.hpp.in', 'include', 'source'];
for (const entry of entries) {
	const from = path.join(repo, entry);
	if (!fs.existsSync(from)) {
		console.error(`vendor: missing ${from}`);
		process.exit(1);
	}
	fs.cpSync(from, path.join(dest, entry), { recursive: true });
}
fs.copyFileSync(path.join(repo, 'LICENSE'), path.join(root, 'LICENSE'));
console.log(`vendor: copied ${entries.length} entries to ${path.relative(root, dest)}`);
