// Parse submitted source without loading its imports or executing its contents.
const fs = require('node:fs');
const path = require('node:path');
const babel = require('./babel-parser.cjs');
const root = path.resolve(process.argv[2]);
const errors = [];
let files = 0, bytes = 0, entries = 0;
function scan(directory, depth = 0) {
  if (depth > 32) throw new Error('syntax-depth-budget');
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    if (++entries > 2048) throw new Error('syntax-entry-budget');
    const file = path.join(directory, entry.name);
    if (entry.isSymbolicLink()) throw new Error('syntax-symlink');
    if (entry.isDirectory()) scan(file, depth + 1);
    else if (entry.isFile() && /\.tsx?$/.test(entry.name)) {
      const size = fs.statSync(file).size;
      bytes += size;
      if (++files > 512 || size > 1048576 || bytes > 8388608) throw new Error('syntax-source-budget');
      try {
        babel.parse(fs.readFileSync(file, 'utf8'), {sourceType: 'module',
          plugins: ['typescript', ...(entry.name.endsWith('.tsx') ? ['jsx'] : [])]});
      } catch (error) {
        errors.push({path: path.relative(root, file), reason: error.reasonCode || 'syntax-invalid',
          parser_line: error.loc?.line || null});
      }
    }
  }
}
try {
  scan(root);
  const ok = files > 0 && errors.length === 0;
  console.log(JSON.stringify({ok, files, bytes, errors: errors.slice(0, 20),
    scope: 'TypeScript parse only; not typecheck or behavior proof'}));
  process.exitCode = ok ? 0 : 1;
} catch {
  console.log(JSON.stringify({ok: false, error: 'syntax-check-incomplete'}));
  process.exitCode = 2;
}
