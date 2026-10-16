import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import guard from './patch_pilot_guard.mjs';

async function fixture(action) {
  const directory = mkdtempSync('/private/tmp/shin-guard-test-');
  const root = directory + '/source';
  mkdirSync(root);
  process.env.SHIN_PILOT_LEDGER = directory + '/ledger.jsonl';
  try { await action(await guard({directory: root}), root, directory); }
  finally { delete process.env.SHIN_PILOT_LEDGER; rmSync(directory, {recursive: true}); }
}
const before = (hooks, number, command = 'printf ok') => {
  const output = {args: {command, description: 'test'}};
  return hooks['tool.execute.before']({tool: 'bash', callID: String(number)}, output).then(() => output);
};
test('exactly twelve calls admitted, thirteenth cannot execute', async () => fixture(async hooks => {
  for (let n = 0; n < 12; n++) await before(hooks, n);
  await assert.rejects(before(hooks, 13), /budget-exhausted/);
}));
test('parallel admissions cannot overspend call count', async () => fixture(async hooks => {
  const calls = await Promise.allSettled(Array.from({length: 20}, (_, n) => before(hooks, n)));
  assert.equal(calls.filter(item => item.status === 'fulfilled').length, 12);
}));
test('only the isolated shell is admitted', async () => fixture(async hooks => {
  await assert.rejects(hooks['tool.execute.before']({tool: 'read'}, {args: {filePath: '/etc/passwd'}}), /only-bash/);
  await assert.rejects(before(hooks, 1), /budget/);
}));
test('per-call and cumulative output caps preserve UTF8 and mark clipping', async () => fixture(async hooks => {
  let bytes = 0;
  for (let n = 0; n < 12; n++) {
    const output = {output: '\u2028'.repeat(10000), metadata: {outputPath: '/private/secret'}, title: 'original'};
    await hooks['tool.execute.after']({callID: String(n)}, output);
    const raw = Buffer.from(output.output);
    assert.ok(raw.length <= 8192);
    assert.ok(!output.output.includes('\ufffd'));
    assert.equal(output.metadata.outputPath, undefined);
    if (raw.length) assert.match(output.output, /output clipped/);
    bytes += raw.length;
  }
  assert.ok(bytes <= 65536);
}));
test('shell may edit its source, not ledger, sibling source or network', async () => fixture(async (hooks, root, directory) => {
  writeFileSync(directory + '/secret', 'private');
  const script = "import pathlib,socket; root=pathlib.Path(" + JSON.stringify(root) + "); " +
    "(root/'own').write_text('ok'); checks=[lambda:pathlib.Path(" + JSON.stringify(directory + '/secret') + ").read_text()," +
    "lambda:pathlib.Path(" + JSON.stringify(directory + '/ledger.jsonl') + ").write_text('tamper')," +
    "lambda:socket.socket().bind(('127.0.0.1',0))]\nfor operation in checks:\n try: operation()\n" +
    " except PermissionError: continue\n raise AssertionError('sandbox operation allowed')\n";
  const quote = text => "'" + text.replaceAll("'", "'\\''") + "'";
  const output = await before(hooks, 1, '/opt/homebrew/bin/python3 -B -c ' + quote(script));
  execFileSync('/bin/bash', ['--noprofile', '--norc', '-c', output.args.command], {cwd: root});
  assert.equal(readFileSync(root + '/own', 'utf8'), 'ok');
  assert.equal(readFileSync(directory + '/secret', 'utf8'), 'private');
}));
test('reloading the guard cannot reset a task budget', async () => fixture(async (_, root) => {
  await assert.rejects(guard({directory: root}), /ledger-missing-or-reused/);
}));
