// Renders the Home Screen icon, webapp/app/icon-180.png, from the app's own wordmark (docs/architecture/webapp.md,
// Home Screen): app.html's .wordmark rules and the face in wordmark.css (make_wordmark.py), set as "micro" over
// "FT8", white on the app's blue. Rendered by a browser so the icon and the header cannot drift apart.
// Run after changing either, from the repository's root:
//   npm install --no-save --prefix webapp/tools playwright && npx --prefix webapp/tools playwright install chromium
//   node webapp/tools/make_icon.mjs
// CHROME=<path> uses an installed Chrome or Chromium instead of Playwright's own.
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const app = join(here, '..', 'app');
const { chromium } = createRequire(import.meta.url)('playwright');

const html = readFileSync(join(app, 'app.html'), 'utf8');
const face = readFileSync(join(app, 'wordmark.css'), 'utf8');
const rules = html.split('\n').filter((l) => l.startsWith('.wordmark')).join('\n');
const accent = /--accent:(#[0-9a-f]{6})/i.exec(html)[1];
if (!rules) throw new Error('no .wordmark rules in app.html');

const page = `<!doctype html><meta charset=utf-8><style>${face}
html,body{margin:0}
${rules}
.icon{width:180px;height:180px;background:${accent};display:flex;align-items:center;justify-content:center}
.icon .wordmark{display:flex;flex-direction:column;align-items:center;color:#fff;letter-spacing:-.03em}
.icon .wm-m{font-size:38px;line-height:1;color:#dbeafe}
.icon .wm-f{font-size:80px;line-height:.86;color:#fff}
</style><div class=icon><h1 class=wordmark><span class=wm-m>micro</span><span class=wm-f>FT8</span></h1></div>`;

const browser = await chromium.launch(process.env.CHROME ? { executablePath: process.env.CHROME } : {});
const tab = await browser.newPage({ viewport: { width: 180, height: 180 }, deviceScaleFactor: 1 });
await tab.setContent(page);
await tab.evaluate(() => document.fonts.ready);
await tab.screenshot({ path: join(app, 'icon-180.png'), clip: { x: 0, y: 0, width: 180, height: 180 } });
await browser.close();
console.log('wrote webapp/app/icon-180.png');
