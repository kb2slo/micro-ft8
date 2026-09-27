// Minifies a built web bundle in place: every .html (with its inline scripts
// and styles) and every .js. Run by webapp/tools/build.py, never by hand:
//
//   node webapp/tools/minify.mjs webapp/dist/app
//
// Conservative on purpose (docs/architecture/webapp.md, Build):
// * Top-level names are never renamed. map.html and app.html call globals
//   that callinfo.js and the sidekick's /pairing.js define (CallInfo,
//   MicroFT8Pairing), and a readable top-level name is what makes an error
//   on a device traceable back to the source.
// * Whitespace collapses conservatively (one space kept), so inline text
//   never runs together.
// * After minifying, every script is compiled (not run) with node:vm, so a
//   minifier that emitted broken syntax fails the build instead of shipping.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { minify as minifyHtml } from 'html-minifier-terser';
import { minify as minifyJs } from 'terser';

const JS = {
  compress: true,
  mangle: { toplevel: false },
  format: { comments: false },
};

const HTML = {
  collapseWhitespace: true,
  conservativeCollapse: true,
  removeComments: true,
  minifyCSS: true,
  minifyJS: JS,
};

const INLINE_SCRIPT = /<script(?![^>]*\bsrc\s*=)[^>]*>([\s\S]*?)<\/script>/gi;

function compileCheck(code, filename) {
  // Throws a SyntaxError naming the file; never executes the code.
  new vm.Script(code, { filename });
}

async function main() {
  const dir = process.argv[2];
  if (!dir || !fs.statSync(dir).isDirectory()) {
    console.error('usage: node minify.mjs <bundle dir>');
    process.exit(2);
  }
  for (const name of fs.readdirSync(dir).sort()) {
    const file = path.join(dir, name);
    const ext = path.extname(name);
    if (ext !== '.html' && ext !== '.js') continue;
    const src = fs.readFileSync(file, 'utf8');
    let out;
    if (ext === '.js') {
      out = (await minifyJs(src, JS)).code;
      compileCheck(out, name);
    } else {
      out = await minifyHtml(src, HTML);
      let i = 0;
      for (const m of out.matchAll(INLINE_SCRIPT)) {
        compileCheck(m[1], `${name} (inline script ${++i})`);
      }
    }
    const before = Buffer.byteLength(src), after = Buffer.byteLength(out);
    if (after >= before) {
      console.log(`kept ${name}: minified would be ${after} bytes, source is ${before}`);
      continue; // a tiny page can grow (quotes, doctype); keep the source bytes
    }
    fs.writeFileSync(file, out);
    console.log(`minified ${name}: ${before} -> ${after} bytes`);
  }
}

main().catch((e) => {
  console.error(`minify failed: ${e && e.message ? e.message : e}`);
  process.exit(1);
});
