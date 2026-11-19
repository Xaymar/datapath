/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// npm "install" hook: use a prebuilt binary when one matches this platform,
// otherwise compile from source with cmake-js (needs CMake and a C++17 compiler).
// Set DATAPATH_BUILD_FROM_SOURCE=1 to always compile.
'use strict';

const path = require('node:path');
const { spawnSync } = require('node:child_process');

const root = path.join(__dirname, '..');

function hasBinary() {
	try {
		require('node-gyp-build').path(root);
		return true;
	} catch {
		return false;
	}
}

const force = /^(1|true|yes)$/i.test(process.env.DATAPATH_BUILD_FROM_SOURCE || '');
if (!force && hasBinary()) {
	process.exit(0);
}

const cmakeJs = require.resolve('cmake-js/bin/cmake-js');
const result = spawnSync(process.execPath, [cmakeJs, 'compile'], { cwd: root, stdio: 'inherit' });
if (result.error) {
	console.error(result.error);
	process.exit(1);
}
process.exit(result.status ?? 1);
