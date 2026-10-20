/** Regression fixtures compile actual C++ against deterministic Win32 fakes, without the addon. */
const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { requires } = require("../helpers/capabilities");
const { WINDOWS_NATIVE_FIXTURES } = require("../lib/gate");

for (const [group, name] of Object.entries(WINDOWS_NATIVE_FIXTURES)) {
  test(name, { timeout: 180000 }, (t) => {
    if (!requires(t, "windows-native-fixtures")) return;
    const result = spawnSync(process.execPath, [path.resolve(__dirname, "../../scripts/test-native-clipboard.js"), group], {
      encoding: "utf8",
      timeout: 170000,
      windowsHide: true,
    });
    assert.ifError(result.error);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, /Failures: 0/);
  });
}
