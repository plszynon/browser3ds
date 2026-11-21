// browse3ds: simplifying web proxy for a Nintendo 3DS browser.
// GET ?k=<key>&u=<url>[&p=<page>]   -> simplified page
// GET ?k=<key>&q=<query>            -> search results
// Output is plain text, one record per line, first char = record type:
//   T title | U final url | G page<TAB>total | H heading | P paragraph | B list item
//   L n<TAB>url<TAB>label | E error message

const KEY = "xk3ds-7Tq9Lm2Vd8Rp4Wa"; // must match key= in the 3DS app config
const PAGE_CHARS = 12000;
const MAX_BODY = 1_000_000;
const MAX_LINKS = 400;
const UA =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";

const NAMED: Record<string, string> = {
  amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", ndash: "-", mdash: "-",
  hellip: "...", laquo: "<<", raquo: ">>", copy: "(c)", reg: "(R)", middot: "-",
  bull: "*", lsquo: "'", rsquo: "'", ldquo: '"', rdquo: '"', euro: "EUR", trade: "(TM)",
};
const MAP: Record<string, string> = {
  "ł": "l", "Ł": "L", "ø": "o", "Ø": "O", "đ": "d", "Đ": "D", "ß": "ss", "æ": "ae",
  "Æ": "AE", "œ": "oe", "Œ": "OE", "–": "-", "—": "-", "‘": "'", "’": "'", "“": '"',
  "”": '"', "…": "...", "•": "*", "«": "<<", "»": ">>", "€": "EUR", "™": "(TM)",
  "©": "(c)", "®": "(R)", "×": "x", "→": "->", "←": "<-", "·": "-",
};

export function decodeEntities(s: string): string {
  return s.replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z]+);/g, (m, e: string) => {
    if (e[0] === "#") {
      const cp = e[1].toLowerCase() === "x" ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
      try { return String.fromCodePoint(cp); } catch { return ""; }
    }
    return NAMED[e] ?? m;
  });
}

export function ascii(s: string): string {
  s = s.normalize("NFD").replace(/[\u0300-\u036f]/g, "");
  return s.replace(/[^\x20-\x7e]/g, (ch) => MAP[ch] ?? (/\s/.test(ch) ? " " : ""));
}

const clean = (s: string) => ascii(decodeEntities(s)).replace(/\s+/g, " ").trim();

const SKIP = new Set([
  "script", "style", "noscript", "svg", "template", "iframe", "nav", "footer",
  "aside", "select", "textarea", "button", "object", "canvas", "video", "audio",
]);
const BLOCK = new Set([
  "p", "div", "section", "article", "li", "ul", "ol", "tr", "table", "br", "blockquote",
  "pre", "dd", "dt", "dl", "figcaption", "figure", "main", "header", "form", "hr",
  "h1", "h2", "h3", "h4", "h5", "h6", "details", "summary", "address", "center",
]);

type Block = { t: "H" | "P" | "B"; s: string };
type Doc = { title: string; blocks: Block[]; links: { url: string; label: string }[] };

function resolveUrl(href: string, base: string): string | null {
  try {
    const u = new URL(decodeEntities(href.trim()), base);
    if (u.protocol !== "http:" && u.protocol !== "https:") return null;
    u.hash = "";
    const out = u.toString();
    return out.length > 450 ? null : out;
  } catch { return null; }
}

export function simplify(html: string, base: string): Doc {
  const blocks: Block[] = [];
  const links: { url: string; label: string }[] = [];
  const linkIdx = new Map<string, number>();
  const addLink = (url: string, label: string): number => {
    let n = linkIdx.get(url);
    if (n === undefined) {
      if (links.length >= MAX_LINKS) return 0;
      links.push({ url, label });
      n = links.length;
      linkIdx.set(url, n);
    }
    return n;
  };

  let cur = "";
  let curType: "H" | "P" | "B" = "P";
  let title = "";
  let inTitle = false;
  let skipName = "";
  let skipDepth = 0;
  let link: { url: string; start: number } | null = null;

  const flush = () => {
    const s = cur.replace(/\s+/g, " ").trim();
    if (s && /[A-Za-z0-9]/.test(s)) blocks.push({ t: curType, s });
    cur = "";
    curType = "P";
  };

  const re = /<!--[\s\S]*?-->|<(\/?)([a-zA-Z][a-zA-Z0-9]*)([^>]*)>|([^<]+)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(html)) !== null) {
    if (m[4] !== undefined) {
      if (inTitle) title += m[4];
      else if (skipDepth === 0) cur += ascii(decodeEntities(m[4]));
      continue;
    }
    if (m[2] === undefined) continue; // comment
    const closing = m[1] === "/";
    const name = m[2].toLowerCase();
    const attrs = m[3] ?? "";

    if (skipDepth > 0) {
      if (name === skipName) skipDepth += closing ? -1 : attrs.trimEnd().endsWith("/") ? 0 : 1;
      continue;
    }
    if (!closing && SKIP.has(name) && !attrs.trimEnd().endsWith("/")) {
      skipName = name;
      skipDepth = 1;
      continue;
    }
    if (name === "title") { inTitle = !closing; continue; }

    if (name === "a") {
      if (!closing) {
        const hm = /\bhref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/i.exec(attrs);
        const url = hm ? resolveUrl(hm[1] ?? hm[2] ?? hm[3] ?? "", base) : null;
        link = url ? { url, start: cur.length } : null;
      } else if (link) {
        const label = cur.slice(link.start).replace(/\s+/g, " ").trim();
        if (label) {
          const n = addLink(link.url, label);
          if (n) cur += ` [${n}]`;
        }
        link = null;
      }
      continue;
    }
    if (name === "img" && !closing) {
      const am = /\balt\s*=\s*(?:"([^"]*)"|'([^']*)')/i.exec(attrs);
      const alt = am ? clean(am[1] ?? am[2] ?? "") : "";
      if (alt && alt.length < 80) cur += ` [img: ${alt}] `;
      continue;
    }
    if (name === "td" || name === "th") {
      if (!closing && cur.trim()) cur += " | ";
      continue;
    }
    if (BLOCK.has(name)) {
      flush();
      if (!closing) {
        if (/^h[1-6]$/.test(name)) curType = "H";
        else if (name === "li") curType = "B";
      }
    }
  }
  flush();
  return { title: clean(title) || "(untitled)", blocks, links };
}

function stripTags(s: string): string {
  return clean(s.replace(/<[^>]+>/g, " "));
}

type Result = { url: string; title: string; snippet: string };

export function parseDDG(html: string): Result[] {
  const out: Result[] = [];
  const re = /<a\b([^>]*)>([\s\S]*?)<\/a>/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(html)) !== null) {
    const attrs = m[1];
    if (attrs.includes("result__a")) {
      const hm = /href="([^"]+)"/.exec(attrs);
      if (!hm) continue;
      let href = decodeEntities(hm[1]);
      if (href.startsWith("//")) href = "https:" + href;
      let target: string | null = null;
      try {
        const u = new URL(href, "https://duckduckgo.com");
        target = u.searchParams.get("uddg");
        if (!target && !/duckduckgo\.com$/.test(u.hostname)) target = u.toString();
      } catch { /* ignore */ }
      if (!target || !/^https?:\/\//.test(target) || target.length > 450) continue;
      out.push({ url: target, title: stripTags(m[2]), snippet: "" });
    } else if (attrs.includes("result__snippet") && out.length) {
      const last = out[out.length - 1];
      if (!last.snippet) last.snippet = stripTags(m[2]);
    }
  }
  return out;
}

export function parseMojeek(html: string): Result[] {
  const out: Result[] = [];
  const re = /<a\b([^>]*)>([\s\S]*?)<\/a>|<p\b[^>]*class="s"[^>]*>([\s\S]*?)<\/p>/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(html)) !== null) {
    if (m[1] !== undefined && /class="title"/.test(m[1])) {
      const hm = /href="([^"]+)"/.exec(m[1]);
      if (!hm) continue;
      const url = decodeEntities(hm[1]);
      if (!/^https?:\/\//.test(url) || url.length > 450) continue;
      out.push({ url, title: stripTags(m[2]), snippet: "" });
    } else if (m[3] !== undefined && out.length && !out[out.length - 1].snippet) {
      out[out.length - 1].snippet = stripTags(m[3]);
    }
  }
  return out;
}

function blockedHost(h: string): boolean {
  h = h.toLowerCase();
  return (
    h === "localhost" || h.endsWith(".local") || h.endsWith(".internal") || h.includes(":") ||
    /^(127|10|0)\./.test(h) || /^192\.168\./.test(h) || /^169\.254\./.test(h) ||
    /^172\.(1[6-9]|2\d|3[01])\./.test(h)
  );
}

async function fetchHtml(url: string): Promise<{ html: string; finalUrl: string } | string> {
  let u: URL;
  try { u = new URL(url); } catch { return "bad url"; }
  if (u.protocol !== "http:" && u.protocol !== "https:") return "only http/https";
  if (blockedHost(u.hostname)) return "host not allowed";
  let res: Response;
  try {
    res = await fetch(u.toString(), {
      headers: { "User-Agent": UA, "Accept": "text/html,text/plain;q=0.9,*/*;q=0.5", "Accept-Language": "en,pl;q=0.8" },
      redirect: "follow",
      signal: AbortSignal.timeout(10000),
    });
  } catch (e) { return "fetch failed: " + (e instanceof Error ? e.message : String(e)); }
  if (blockedHost(new URL(res.url || u.toString()).hostname)) return "host not allowed";
  const ct = res.headers.get("content-type") ?? "";
  if (!/text\/|html|xml/i.test(ct) && ct) return "unsupported type: " + ct.split(";")[0];
  const buf = new Uint8Array(await res.arrayBuffer()).slice(0, MAX_BODY);
  let charset = /charset=([\w-]+)/i.exec(ct)?.[1] ?? "";
  if (!charset) {
    const head = new TextDecoder("latin1").decode(buf.slice(0, 2048));
    charset = /<meta[^>]+charset=["']?([\w-]+)/i.exec(head)?.[1] ?? "utf-8";
  }
  let html: string;
  try { html = new TextDecoder(charset).decode(buf); } catch { html = new TextDecoder("utf-8").decode(buf); }
  if (res.status >= 400 && html.length < 200) return "HTTP " + res.status;
  return { html, finalUrl: res.url || u.toString() };
}

function render(doc: Doc, finalUrl: string, page: number): string {
  // split blocks into pages of about PAGE_CHARS
  const pages: Block[][] = [[]];
  let size = 0;
  for (const b of doc.blocks) {
    if (size > 0 && size + b.s.length > PAGE_CHARS) { pages.push([]); size = 0; }
    pages[pages.length - 1].push(b);
    size += b.s.length + 1;
  }
  const total = pages.length;
  const p = Math.min(Math.max(page, 1), total);
  const out: string[] = [`T${doc.title}`, `U${finalUrl}`, `G${p}\t${total}`];
  const seen = new Set<number>();
  for (const b of pages[p - 1]) {
    out.push(b.t + b.s);
    for (const mm of b.s.matchAll(/\[(\d+)\]/g)) seen.add(parseInt(mm[1], 10));
  }
  if (!pages[p - 1].length) out.push("P(this page has no readable text)");
  for (const n of [...seen].sort((a, b) => a - b)) {
    const l = doc.links[n - 1];
    if (l) out.push(`L${n}\t${l.url}\t${l.label.slice(0, 80)}`);
  }
  return out.join("\n") + "\n";
}

function searchDoc(q: string, results: Result[]): Doc {
  const blocks: Block[] = [];
  const links: { url: string; label: string }[] = [];
  results.slice(0, 15).forEach((r, i) => {
    links.push({ url: r.url, label: r.title });
    blocks.push({ t: "H", s: `${r.title || r.url} [${i + 1}]` });
    try { blocks.push({ t: "P", s: new URL(r.url).hostname }); } catch { /* ignore */ }
    if (r.snippet) blocks.push({ t: "P", s: r.snippet });
  });
  return { title: "Search: " + q, blocks, links };
}

async function doSearch(q: string): Promise<string> {
  let results: Result[] = [];
  try {
    const r = await fetch("https://html.duckduckgo.com/html/?q=" + encodeURIComponent(q), {
      headers: { "User-Agent": UA, "Accept-Language": "en,pl;q=0.8" },
      signal: AbortSignal.timeout(8000),
    });
    results = parseDDG(await r.text());
  } catch { /* try fallback */ }
  if (!results.length) {
    try {
      const r = await fetch("https://www.mojeek.com/search?q=" + encodeURIComponent(q), {
        headers: { "User-Agent": UA, "Accept-Language": "en,pl;q=0.8" },
        signal: AbortSignal.timeout(8000),
      });
      results = parseMojeek(await r.text());
    } catch { /* ignore */ }
  }
  if (!results.length) return "Eno search results (search engines blocked or down)\n";
  return render(searchDoc(q, results), "search:" + q, 1);
}

const text = (body: string) =>
  new Response(body, {
    headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" },
  });

export async function handle(req: Request): Promise<Response> {
  const url = new URL(req.url);
  if (url.searchParams.get("k") !== KEY) return text("Ebad key\n");
  const q = (url.searchParams.get("q") ?? "").trim();
  let u = (url.searchParams.get("u") ?? "").trim();
  const page = parseInt(url.searchParams.get("p") ?? "1", 10) || 1;

  if (q) return text(await doSearch(q));
  if (!u) return text("Eno url given\n");
  if (!/^[a-z]+:\/\//i.test(u)) {
    if (/\s/.test(u) || !u.includes(".")) return text(await doSearch(u));
    u = "https://" + u;
  }
  const r = await fetchHtml(u);
  if (typeof r === "string") return text("E" + r + "\n");
  const doc = simplify(r.html, r.finalUrl);
  return text(render(doc, r.finalUrl, page));
}

// @ts-ignore Deno is provided by the Supabase Edge runtime
if (typeof Deno !== "undefined" && Deno.serve) Deno.serve(handle);
