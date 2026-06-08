// =============================================================================
// bench/ajv_bench.mjs
//
// 与 iris_validate 完全对称的参考实现，使用 ajv —— ebdrup/json-schema-benchmark
// 长期榜首选手。
//
// 用法：
//   node bench/ajv_bench.mjs <schema.json> <data.jsonl> [iterations=1]
// =============================================================================
import fs from "node:fs";
import process from "node:process";
import Ajv from "ajv";

if (process.argv.length < 4) {
    console.error("usage: node ajv_bench.mjs <schema.json> <data.jsonl> [iters=1]");
    process.exit(2);
}
const schemaPath = process.argv[2];
const dataPath   = process.argv[3];
const iters      = Math.max(1, parseInt(process.argv[4] ?? "1", 10));

const schema = JSON.parse(fs.readFileSync(schemaPath, "utf8"));
const ajv = new Ajv({
    allErrors: false,
    strict: false,
    coerceTypes: false,
    useDefaults: false,
});
const validate = ajv.compile(schema);

const raw = fs.readFileSync(dataPath, "utf8");
const lines = raw.split("\n").filter(l => l.length > 0);

console.log(`[ajv]  schema=${schemaPath} lines=${lines.length} iters=${iters}`);

// ajv 处理的是已 parse 的 JS 对象。为公平对比 IRIS（含 parse），
// 我们把 JSON.parse 算进去——这是工业现场实际成本。
let ok = 0;
const t0 = process.hrtime.bigint();
for (let it = 0; it < iters; ++it) {
    for (let i = 0; i < lines.length; ++i) {
        let obj;
        try { obj = JSON.parse(lines[i]); } catch { continue; }
        if (validate(obj)) ok++;
    }
}
const t1 = process.hrtime.bigint();
const secs = Number(t1 - t0) / 1e9;
const total = iters * lines.length;
const mops  = total / secs / 1e6;
const mbps  = (iters * raw.length) / secs / (1024 * 1024);
const avg_ns = secs / total * 1e9;

console.log(`[ajv]  ${secs.toFixed(3)} s | ${mops.toFixed(2)} Mops/s | ` +
            `${mbps.toFixed(2)} MiB/s | avg ${avg_ns.toFixed(1)} ns/op | ` +
            `ok=${ok}/${total}`);
