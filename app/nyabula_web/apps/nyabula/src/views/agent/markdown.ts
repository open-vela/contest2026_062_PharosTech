/* Markdown for replies, and the plain text behind it.
 *
 * The model is asked for Markdown and told to leave emoji out, but neither
 * is a guarantee, so the reader gets what it can render and the speech
 * synthesizer gets words only.  Rendering builds DOM nodes instead of HTML
 * text: nothing a reply contains can become markup, so no sanitizer and no
 * third-party parser are needed -- this runs from the device's own flash.
 */

export type Inline =
  | { kind: 'text'; text: string }
  | { kind: 'code'; text: string }
  | { kind: 'strong'; parts: Inline[] }
  | { kind: 'em'; parts: Inline[] }
  | { kind: 'strike'; parts: Inline[] }
  | { kind: 'link'; href: string; parts: Inline[] };

export type Block =
  | { kind: 'p'; parts: Inline[] }
  | { kind: 'h'; level: number; parts: Inline[] }
  | { kind: 'ul' | 'ol'; items: Inline[][]; start: number }
  | { kind: 'quote'; blocks: Block[] }
  | { kind: 'pre'; text: string; language: string }
  | { kind: 'hr' };

/* Only schemes a reply may send the reader to. */
function safeHref(raw: string): string {
  const href = raw.trim();
  return /^(https?:\/\/|mailto:)/i.test(href) ? href : '';
}

function pushText(parts: Inline[], text: string): void {
  if (!text) return;
  const last = parts[parts.length - 1];
  if (last && last.kind === 'text') last.text += text;
  else parts.push({ kind: 'text', text });
}

/* `**a**`, `*a*`, `~~a~~`, `` `a` ``, `[a](href)`; anything unpaired stays
 * the literal characters the model wrote. */
export function parseInline(source: string): Inline[] {
  const parts: Inline[] = [];
  let at = 0;
  while (at < source.length) {
    const char = source[at];
    if (char === '\\' && at + 1 < source.length && /[\\`*_~[\]()#+\-.!>]/.test(source[at + 1])) {
      pushText(parts, source[at + 1]);
      at += 2;
      continue;
    }
    if (char === '`') {
      const fence = /^`+/.exec(source.slice(at))![0];
      const end = source.indexOf(fence, at + fence.length);
      if (end > 0) {
        parts.push({ kind: 'code', text: source.slice(at + fence.length, end).trim() });
        at = end + fence.length;
        continue;
      }
    }
    if (char === '[') {
      const match = /^\[([^\]]*)\]\(([^)\s]*)(?:\s+"[^"]*")?\)/.exec(source.slice(at));
      if (match) {
        const href = safeHref(match[2]);
        const inner = parseInline(match[1]);
        if (href) parts.push({ kind: 'link', href, parts: inner });
        else parts.push(...inner);
        at += match[0].length;
        continue;
      }
    }
    if (source.startsWith('**', at) || source.startsWith('__', at) || source.startsWith('~~', at)) {
      const mark = source.slice(at, at + 2);
      const end = source.indexOf(mark, at + 2);
      if (end > at + 2) {
        parts.push({
          kind: mark === '~~' ? 'strike' : 'strong',
          parts: parseInline(source.slice(at + 2, end)),
        });
        at = end + 2;
        continue;
      }
      pushText(parts, mark);
      at += 2;
      continue;
    }
    if (char === '*' || char === '_') {
      const end = source.indexOf(char, at + 1);
      if (end > at + 1) {
        parts.push({ kind: 'em', parts: parseInline(source.slice(at + 1, end)) });
        at = end + 1;
        continue;
      }
    }
    pushText(parts, char);
    at += 1;
  }
  return parts;
}

export function parseMarkdown(source: string): Block[] {
  const lines = source.replace(/\r\n?/g, '\n').split('\n');
  const blocks: Block[] = [];
  let at = 0;
  while (at < lines.length) {
    const line = lines[at];
    if (!line.trim()) { at += 1; continue; }
    const fence = /^\s{0,3}(```+|~~~+)\s*([^\s`]*)/.exec(line);
    if (fence) {
      const body: string[] = [];
      at += 1;
      while (at < lines.length && !new RegExp(`^\\s{0,3}${fence[1][0]}{${fence[1].length},}\\s*$`).test(lines[at])) {
        body.push(lines[at]);
        at += 1;
      }
      at += 1;
      blocks.push({ kind: 'pre', text: body.join('\n'), language: fence[2] ?? '' });
      continue;
    }
    if (/^\s{0,3}(?:[-*_]\s*){3,}$/.test(line)) { blocks.push({ kind: 'hr' }); at += 1; continue; }
    const heading = /^\s{0,3}(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      blocks.push({ kind: 'h', level: heading[1].length, parts: parseInline(heading[2].replace(/\s+#+\s*$/, '')) });
      at += 1;
      continue;
    }
    if (/^\s{0,3}>/.test(line)) {
      const body: string[] = [];
      while (at < lines.length && (/^\s{0,3}>/.test(lines[at]) || lines[at].trim())) {
        if (!/^\s{0,3}>/.test(lines[at]) && !body.length) break;
        body.push(lines[at].replace(/^\s{0,3}>\s?/, ''));
        at += 1;
      }
      blocks.push({ kind: 'quote', blocks: parseMarkdown(body.join('\n')) });
      continue;
    }
    const bullet = /^\s{0,3}([-*+]|\d{1,9}[.)])\s+/.exec(line);
    if (bullet) {
      const ordered = /\d/.test(bullet[1]);
      const start = ordered ? Number.parseInt(bullet[1], 10) : 1;
      const items: Inline[][] = [];
      let current = '';
      while (at < lines.length) {
        const next = /^\s{0,3}([-*+]|\d{1,9}[.)])\s+(.*)$/.exec(lines[at]);
        if (next && /\d/.test(next[1]) === ordered) {
          if (current) items.push(parseInline(current.trim()));
          current = next[2];
          at += 1;
          continue;
        }
        if (!lines[at].trim() || /^\s{0,3}(#{1,6}\s|>|```|~~~)/.test(lines[at])) break;
        current += ' ' + lines[at].trim();
        at += 1;
      }
      if (current) items.push(parseInline(current.trim()));
      blocks.push({ kind: ordered ? 'ol' : 'ul', items, start });
      continue;
    }
    const body: string[] = [];
    while (at < lines.length && lines[at].trim() &&
           !/^\s{0,3}(#{1,6}\s|>|```|~~~|([-*+]|\d{1,9}[.)])\s)/.test(lines[at]) &&
           !/^\s{0,3}(?:[-*_]\s*){3,}$/.test(lines[at])) {
      body.push(lines[at]);
      at += 1;
    }
    if (!body.length) { pushBlockFallback(blocks, lines[at]); at += 1; continue; }
    blocks.push({ kind: 'p', parts: parseInline(body.join('\n')) });
  }
  return blocks;
}

function pushBlockFallback(blocks: Block[], line: string): void {
  blocks.push({ kind: 'p', parts: parseInline(line) });
}

/* Every emoji and pictograph the model may still emit, plus the variation
 * selector and zero-width joiner that hold sequences together. */
const EMOJI =
  /[\u{1F000}-\u{1FAFF}\u{1F004}\u{2190}-\u{21FF}\u{2300}-\u{27BF}\u{2B00}-\u{2BFF}\u{FE0E}\u{FE0F}\u{200D}\u{E0020}-\u{E007F}]/gu;

/* Kaomoji and the ASCII emoticons a model falls back on. */
const EMOTICON = /(?:^|\s)(?:[:;=8xX][-~o^']?[)\](\[dDpPoO/\\|3*]|[)\](\[dDpP][-~o^']?[:;=8]|\([^()\n]{0,12}[;^ωﾟ・･°][^()\n]{0,12}\))(?=\s|$)/g;

/* What the reply says, with nothing the speech synthesizer would read out
 * as punctuation: no markup, no emoji, no bare URLs. */
export function speakableText(source: string): string {
  const spoken = (parts: Inline[]): string =>
    parts.map(part => {
      switch (part.kind) {
        case 'text': return part.text;
        case 'code': return part.text;
        case 'link': return spoken(part.parts);
        default: return spoken(part.parts);
      }
    }).join('');
  const walk = (blocks: Block[]): string[] =>
    blocks.flatMap(block => {
      switch (block.kind) {
        case 'p':
        case 'h':
          return [spoken(block.parts)];
        case 'ul':
        case 'ol':
          return block.items.map(item => spoken(item));
        case 'quote':
          return walk(block.blocks);
        case 'pre':
        case 'hr':
        default:
          return [];
      }
    });
  return walk(parseMarkdown(source))
    .join('\n')
    .replace(EMOJI, '')
    .replace(EMOTICON, ' ')
    .replace(/https?:\/\/\S+/g, '')
    .replace(/[ \t]{2,}/g, ' ')
    .replace(/\n{2,}/g, '\n')
    .split('\n')
    .map(line => line.trim())
    .filter(Boolean)
    .join('\n')
    .trim();
}
