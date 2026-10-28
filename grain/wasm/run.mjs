// Runs a Grain program on the WebAssembly build and prints what it printed.
//
//   node grain/wasm/run.mjs <grain_wasm.js> <program.gr> [step_limit]
//
// Exit status 0 when the program ran, 1 when it did not compile or raised an error (printed to stderr with its line).
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const [, , module_path, program, limit] = process.argv;
if (!module_path || !program) {
  console.error('usage: node run.mjs <grain_wasm.js> <program.gr> [step_limit]');
  process.exit(2);
}
const { default: createGrain } = await import(pathToFileURL(resolve(module_path)).href);
const grain = await createGrain();
const run = grain.cwrap('grain_run', 'number', ['string', 'string', 'number']);
const free = grain.cwrap('grain_free', null, ['number']);

const ptr = run(readFileSync(program, 'utf8'), program, limit ? Number(limit) : 0);
const answer = JSON.parse(grain.UTF8ToString(ptr));
free(ptr);
for (const line of answer.output) console.log(line);
if (!answer.ok) {
  console.error(`${program}:${answer.line}:${answer.column}: ${answer.error}`);
  process.exit(1);
}
