// Builds the getting-started page from its one source, webapp/getting-started.md, into
// webapp/getting-started.html: a page of the site on screen, and the one-page sheet that goes in the
// box when printed (webapp/tools/getting-started.css). Generated, never committed; pages.yml builds it
// before deploying and CI's Host tests run it. Rules for the source are in its own header comment.
//
//   node webapp/tools/getting_started.mjs         check, then write the page
//   node webapp/tools/getting_started.mjs --pdf   also print webapp/dist/getting-started.pdf through
//                                                 headless Chrome (CHROME=/path to choose one) and
//                                                 hold it to the source's `pages:` limit
//
// CI runs --pdf on every push and posts the PDF with the firmware images (docs/RELEASE_PROCESS.md).

import { execFileSync, spawn, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, extname, join, relative, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { Marked } from 'marked';
import QRCode from 'qrcode';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..', '..');
const SOURCE = join(ROOT, 'webapp', 'getting-started.md');
const CSS = join(HERE, 'getting-started.css');
const PAGE = join(ROOT, 'webapp', 'getting-started.html');
const PRINT_PAGE = join(ROOT, 'webapp', 'dist', 'getting-started.print.html');
const PDF = join(ROOT, 'webapp', 'dist', 'getting-started.pdf');
const LATEST = 'micro-ft8.kalikstein.com/getting-started';

// The printed sheet is set in IBM Plex (OFL; @fontsource packages pinned in package.json), embedded in
// the copy Chrome prints, so the PDF breaks across columns and pages the same on a Mac and on CI's
// Linux, whose system fonts differ. The site's own copy carries no fonts. The Latin subsets lack
// → ✓ ✕, which come from a system font.
const FONT_DIR = join(HERE, 'node_modules', '@fontsource');
const SHEET_FONTS = [
  ['IBM Plex Sans', 'normal', 400, 'ibm-plex-sans/files/ibm-plex-sans-latin-400-normal.woff2'],
  ['IBM Plex Sans', 'italic', 400, 'ibm-plex-sans/files/ibm-plex-sans-latin-400-italic.woff2'],
  ['IBM Plex Sans', 'normal', 600, 'ibm-plex-sans/files/ibm-plex-sans-latin-600-normal.woff2'],
  ['IBM Plex Sans', 'normal', 700, 'ibm-plex-sans/files/ibm-plex-sans-latin-700-normal.woff2'],
  ['IBM Plex Mono', 'normal', 400, 'ibm-plex-mono/files/ibm-plex-mono-latin-400-normal.woff2'],
];

function sheetFontCss() {
  return SHEET_FONTS.map(([family, style, weight, file]) => {
    const woff2 = readFileSync(join(FONT_DIR, file)).toString('base64');
    return `@font-face { font-family: "${family}"; font-style: ${style}; font-weight: ${weight}; ` +
      `src: url(data:font/woff2;base64,${woff2}) format("woff2"); }`;
  }).join('\n');
}

// Where the station's on-screen text lives. Every **bold** and `code` span in the source must appear
// in one of these files, comments stripped, so renaming a button or a page fails this build instead of
// leaving a page, and a printed sheet, that names something the screen no longer shows.
const UI_SOURCES = ['webapp/app', 'sidekick/main', 'poe_p4/main'];
const UI_EXTENSIONS = new Set(['.html', '.js', '.c', '.h', '.cpp']);

const ENTITIES = {
  amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ', rarr: '→', larr: '←', rsaquo: '›',
  lsaquo: '‹', mdash: '—', ndash: '–', middot: '·', times: '×', minus: '−', hellip: '…', rsquo: '’',
  lsquo: '‘', rdquo: '”', ldquo: '“', bull: '•', deg: '°',
};

function decodeEntities(s) {
  return s.replace(/&(#x[0-9a-f]+|#[0-9]+|[a-z]+);/gi, (m, e) => {
    if (e[0] !== '#') return ENTITIES[e.toLowerCase()] ?? m;
    return String.fromCodePoint(e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10));
  });
}

const squash = (s) => s.replace(/\s+/g, ' ');

const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);

// Text as a reader of the screen would find it: no comments, entities and \u escapes decoded, inline
// tags that can split a sentence removed, whitespace collapsed. A string a comment is the only home of
// does not count. A `//` with no whitespace before it (http://) is not a comment.
function uiText(src) {
  return squash(decodeEntities(src
    .replace(/<!--[\s\S]*?-->/g, ' ')
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/(^|[ \t])\/\/.*$/gm, '$1')
    .replace(/<\/?(?:a|b|i|em|strong|code|span)\b[^>]*>/gi, '')
    .replace(/\\u([0-9a-fA-F]{4})/g, (_, h) => String.fromCharCode(parseInt(h, 16)))));
}

function loadUiText() {
  const parts = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      const path = join(dir, name);
      if (statSync(path).isDirectory()) walk(path);
      else if (UI_EXTENSIONS.has(extname(name))) parts.push(uiText(readFileSync(path, 'utf8')));
    }
  };
  for (const dir of UI_SOURCES) walk(join(ROOT, dir));
  return parts.join(' \u0000 '); // no needle contains NUL, so a match cannot span two files
}

// The `---` block at the top of the source: `title:`, `description:` and `pages:` (the most sides
// the printed sheet may take).
function frontMatter(src) {
  const m = /^---\r?\n([\s\S]*?)\r?\n---\r?\n/.exec(src);
  if (!m) return { meta: {}, body: src };
  const meta = {};
  for (const line of m[1].split(/\r?\n/)) {
    const kv = /^\s*([\w-]+)\s*:\s*(.*?)\s*$/.exec(line);
    if (kv) meta[kv[1]] = kv[2];
  }
  return { meta, body: src.slice(m[0].length) };
}

// `![caption](qr:<text>)` prints <text> as a QR code. Four modules of quiet zone, as the spec asks.
function qrFigure(data, caption) {
  const qr = QRCode.create(data, { errorCorrectionLevel: 'M' });
  const n = qr.modules.size;
  const dark = qr.modules.data;
  const quiet = 4;
  let d = '';
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (!dark[y * n + x]) continue;
      let w = 1;
      while (x + w < n && dark[y * n + x + w]) w++;
      d += `M${x + quiet} ${y + quiet}h${w}v1h${-w}z`;
      x += w - 1;
    }
  }
  const s = n + 2 * quiet;
  return `<span class="qr"><svg viewBox="0 0 ${s} ${s}" shape-rendering="crispEdges" aria-hidden="true">` +
    `<path d="${d}"/></svg><span class="qr-cap">${escapeHtml(caption)}</span></span>`;
}

const marked = new Marked({
  gfm: true,
  renderer: {
    // "## 3. Pair your phone" prints its number as a step badge.
    heading({ tokens, depth }) {
      const inner = this.parser.parseInline(tokens);
      const step = depth === 2 && /^(\d+)\.\s+([\s\S]*)$/.exec(inner);
      return step ? `<h2><span class="step">${step[1]}</span>${step[2]}</h2>\n` : false;
    },
    image({ href, text }) {
      return href.startsWith('qr:') ? qrFigure(href.slice(3), text) : false;
    },
  },
});

function lineOf(src, raw) {
  const i = src.indexOf(raw);
  return i < 0 ? '?' : src.slice(0, i).split('\n').length;
}

function missingOnScreenText(tokens, src, ui) {
  const missing = [];
  marked.walkTokens(tokens, (t) => {
    if (t.type !== 'strong' && t.type !== 'codespan') return;
    const needle = squash(decodeEntities(t.text)).trim();
    if (needle && !ui.includes(needle)) missing.push(`  line ${lineOf(src, t.raw)}: ${t.raw}`);
  });
  return missing;
}

// Everything before the first `##` is the header, across the full width; each `##` and what follows it
// is a section, in one of the printed sheet's two columns.
function renderBody(tokens) {
  const groups = [[]];
  for (const t of tokens) {
    if (t.type === 'heading' && t.depth === 2) groups.push([]);
    groups[groups.length - 1].push(t);
  }
  const html = (group) => {
    group.links = tokens.links;
    return marked.parser(group);
  };
  const [head, ...sections] = groups;
  return (head.length ? `<header>\n${html(head)}</header>\n` : '') + '<div class="cols">\n' +
    sections.map((s) => `<section>\n${html(s)}</section>\n`).join('') + '</div>\n';
}

// `<short sha>[-dirty] <UTC date>`, the same shape as the web app's build id, so a printed sheet says
// which revision it is. Dirty means the source, this script or its stylesheet has uncommitted changes.
function buildStamp() {
  const git = (...args) => execFileSync('git', ['-C', ROOT, ...args], { encoding: 'utf8' }).trim();
  let id = 'nogit';
  try {
    id = git('rev-parse', '--short', 'HEAD');
    if (git('status', '--porcelain', '--', ...[SOURCE, CSS, fileURLToPath(import.meta.url)].map((p) => relative(ROOT, p)))) {
      id += '-dirty';
    }
  } catch {
    // Not a git checkout: the stamp says so.
  }
  return `${id} ${new Date().toISOString().slice(0, 10)}`;
}

// The page is shaped like the site's other pages (downloads.html): no head or body tags, an eyebrow
// back to the landing page, a footer. The print stylesheet keeps the eyebrow and drops the footer, and
// the eyebrow's link is absolute because the PDF keeps it: a bare "/" there would point at the disk.
function page(meta, body, stamp, extraCss = '') {
  const cssString = (s) => `"${s.replace(/["\\]/g, '\\$&')}"`;
  const footer = `Latest: ${LATEST} · ${stamp}`;
  return `<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(meta.title || 'Getting started — Micro-FT8')}</title>
<meta name="description" content="${escapeHtml(meta.description || '')}">
<!-- Generated from webapp/getting-started.md by webapp/tools/getting_started.mjs (${escapeHtml(stamp)}).
     Edit the Markdown, not this file. -->
<style>
${extraCss}${readFileSync(CSS, 'utf8')}
@page { @bottom-right { content: ${cssString(footer)}; } }
</style>
<main>
  <p class="eyebrow"><a href="https://micro-ft8.kalikstein.com/">Micro-FT8</a> · Getting started</p>
${body}  <footer>
    <a href="/">← Announcing Micro-FT8</a> · <a href="downloads.html">Downloads</a>
  </footer>
</main>
</html>
`;
}

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const apps = [
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
  ];
  for (const app of apps) if (existsSync(app)) return app;
  for (const name of ['google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser']) {
    const r = spawnSync('which', [name], { encoding: 'utf8' });
    if (r.status === 0 && r.stdout.trim()) return r.stdout.trim();
  }
  return null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Chrome prints in a second or two, but on macOS it can then sit for minutes without exiting (seen with
// Chrome 154, 2026-10-08). So wait for the finished file, which ends in %%EOF and has stopped growing,
// then end Chrome rather than wait for it.
async function printPdf(chrome, htmlPath, pdfPath) {
  rmSync(pdfPath, { force: true });
  // A throwaway profile, so a Chrome the operator has open is neither used nor disturbed.
  const profile = mkdtempSync(join(tmpdir(), 'microft8-print-'));
  const child = spawn(chrome, [
    '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--disable-background-networking', '--disable-component-update',
    '--disable-sync', '--use-mock-keychain', `--user-data-dir=${profile}`,
    // CI's Ubuntu runners restrict the user namespaces Chrome's sandbox needs. The page is our own
    // file with no network content, so there is nothing for a sandbox to contain.
    ...(process.env.CI ? ['--no-sandbox'] : []),
    '--no-pdf-header-footer', `--print-to-pdf=${pdfPath}`, pathToFileURL(htmlPath).href,
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', (b) => { stderr += b; });
  let exitCode;
  const exited = new Promise((r) => {
    child.on('exit', (code) => { exitCode = code; r(); });
    child.on('error', (e) => { exitCode = -1; stderr = `could not run ${chrome}: ${e.message}`; r(); });
  });
  try {
    let lastSize = -1;
    for (const deadline = Date.now() + 60000; ;) {
      await sleep(250);
      if (existsSync(pdfPath)) {
        const pdf = readFileSync(pdfPath);
        if (pdf.length === lastSize && pdf.subarray(-32).toString('latin1').includes('%%EOF')) return;
        lastSize = pdf.length;
      } else if (exitCode !== undefined) {
        throw new Error(`Chrome exited (${exitCode}) without printing ${relative(ROOT, pdfPath)}:\n${stderr.trim()}`);
      }
      if (Date.now() > deadline) throw new Error(`Chrome did not print ${relative(ROOT, pdfPath)} within 60 s`);
    }
  } finally {
    if (exitCode === undefined) {
      child.kill('SIGKILL');
      await exited;
    }
    rmSync(profile, { recursive: true, force: true });
  }
}

// Chrome writes each page as an uncompressed `/Type /Page` dictionary.
function pdfPages(pdfPath) {
  return (readFileSync(pdfPath, 'latin1').match(/\/Type\s*\/Page\b/g) || []).length;
}

async function main() {
  const wantPdf = process.argv.slice(2).includes('--pdf');
  const chrome = wantPdf ? findChrome() : null;
  if (wantPdf && !chrome) {
    throw new Error('No Chrome or Chromium found. Install one, or set CHROME=/path/to/chrome.');
  }

  const src = readFileSync(SOURCE, 'utf8');
  const { meta, body } = frontMatter(src);
  const tokens = marked.lexer(body);
  const rel = relative(ROOT, SOURCE);

  const missing = missingOnScreenText(tokens, src, loadUiText());
  if (missing.length) {
    throw new Error(`${rel}: on-screen text not found in ${UI_SOURCES.join(', ')}:\n${missing.join('\n')}\n` +
      'Bold and `code` are checked as what the station shows: copy them from the screen exactly, ' +
      'or use *italics* (the rules are in its header comment).');
  }

  const stamp = buildStamp();
  const html = renderBody(tokens);
  writeFileSync(PAGE, page(meta, html, stamp));
  console.log(`${relative(ROOT, PAGE)}: on-screen text checked, ${stamp}`);
  if (!wantPdf) return;

  mkdirSync(dirname(PDF), { recursive: true });
  writeFileSync(PRINT_PAGE, page(meta, html, stamp, `${sheetFontCss()}\n`));
  await printPdf(chrome, PRINT_PAGE, PDF);
  const pages = pdfPages(PDF);
  const limit = Number(meta.pages || 1);
  const sides = `${pages} page${pages === 1 ? '' : 's'}`;
  if (pages > limit) {
    throw new Error(`${relative(ROOT, PDF)}: ${sides}, over its limit of ${limit} (\`pages:\` in ${rel}). ` +
      'Trim it, or raise the limit if another side is meant.');
  }
  console.log(`${relative(ROOT, PDF)}: ${sides} (limit ${limit})`);
}

main().catch((e) => {
  console.error(e.message);
  process.exit(1);
});
