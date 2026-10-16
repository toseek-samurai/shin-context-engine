import { appendFileSync, existsSync, realpathSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { createHash } from 'node:crypto';

// Test-only OpenCode hook. The ledger and configuration are outside agent writes.
export default async function guard({ directory }) {
  const root = realpathSync(directory);
  const ledger = process.env.SHIN_PILOT_LEDGER;
  if (!ledger || existsSync(ledger)) throw new Error('pilot-ledger-missing-or-reused');
  const started = performance.now();
  let calls = 0;
  let bytes = 0;
  let stopped = false;
  const record = (value) => appendFileSync(ledger, JSON.stringify(value) + '\n', { mode: 0o600 });
  record({ event: 'started', root, calls: 12, output_bytes: 65536, per_call_bytes: 8192 });
  const quote = (text) => "'" + text.replaceAll("'", "'\\''") + "'";
  const policy = '(version 1)(deny default)(allow process*)(allow sysctl-read)(allow mach-lookup)' +
    '(allow file-read-metadata)' +
    '(allow file-read* (require-all (require-not (subpath "/Users"))' +
    '(require-not (subpath "/private/tmp"))(require-not (subpath "/private/var"))))' +
    '(allow file-read* (subpath ' + JSON.stringify(root) + '))' +
    '(allow file-write* (subpath ' + JSON.stringify(root) + ')(literal "/dev/null"))';
  return {
    'tool.execute.before': async (input, output) => {
      const remaining = 180000 - (performance.now() - started);
      if (stopped || calls >= 12 || bytes >= 65536 || remaining <= 0) {
        stopped = true;
        record({ event: 'stop', reason: 'budget', calls, bytes });
        throw new Error('pilot-budget-exhausted');
      }
      if (input.tool !== 'bash' || typeof output.args.command !== 'string' || output.args.command.length > 32768) {
        stopped = true;
        record({ event: 'stop', reason: 'unexpected-tool', tool: input.tool });
        throw new Error('pilot-only-bash-allowed');
      }
      calls++;
      record({ event: 'call', number: calls, id: input.callID, command: output.args.command });
      output.args.workdir = root;
      output.args.timeout = Math.max(1, Math.min(30000, remaining));
      output.args.command = '/usr/bin/sandbox-exec -p ' + quote(policy) +
        ' /usr/bin/env -i PATH=/opt/homebrew/bin:/usr/bin:/bin LC_ALL=C PYTHONDONTWRITEBYTECODE=1' +
        ' HOME=' + quote(root) + ' TMPDIR=' + quote(root) +
        ' /bin/bash --noprofile --norc -c ' + quote(output.args.command);
    },
    'tool.execute.after': async (input, output) => {
      const raw = Buffer.from(output.output ?? '');
      const limit = Math.min(8192, 65536 - bytes);
      const marker = '\n[pilot output clipped; read a narrower range]\n';
      let delivered = raw.toString('utf8');
      const clipped = raw.length > limit;
      if (clipped) {
        const end = Math.max(0, limit - Buffer.byteLength(marker));
        let safe = end;
        while (safe > 0 && (raw[safe] & 0xc0) === 0x80) safe--;
        delivered = limit < Buffer.byteLength(marker) ? '' : raw.subarray(0, safe).toString('utf8') + marker;
      }
      const size = Buffer.byteLength(delivered);
      if (size > limit) throw new Error('pilot-invalid-output-encoding');
      bytes += size;
      const nativeTruncated = output.metadata?.truncated === true;
      output.output = delivered;
      output.title = 'Isolated workspace command';
      output.metadata = { pilot_clipped: clipped, native_truncated: nativeTruncated };
      record({ event: 'output', id: input.callID, bytes: size, total_bytes: bytes,
        tool_bytes: raw.length, clipped, native_truncated: nativeTruncated,
        sha256: createHash('sha256').update(delivered).digest('hex') });
    },
  };
}
