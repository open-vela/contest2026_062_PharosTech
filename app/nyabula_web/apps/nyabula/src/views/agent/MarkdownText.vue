<script lang="ts">
/* A reply, rendered.  The nodes are built from the parsed blocks, so a
 * reply's own characters can never become markup: no v-html anywhere. */
import { defineComponent, h, type PropType, type VNode } from 'vue';
import { parseMarkdown, type Block, type Inline } from './markdown';

function inline(parts: Inline[]): (VNode | string)[] {
  return parts.map(part => {
    switch (part.kind) {
      case 'text': return part.text;
      case 'code': return h('code', part.text);
      case 'strong': return h('strong', inline(part.parts));
      case 'em': return h('em', inline(part.parts));
      case 'strike': return h('s', inline(part.parts));
      case 'link':
        return h('a', { href: part.href, target: '_blank', rel: 'noopener noreferrer nofollow' }, inline(part.parts));
      default: return '';
    }
  });
}

function block(item: Block): VNode {
  switch (item.kind) {
    case 'h': return h(`h${Math.min(item.level + 2, 6)}`, inline(item.parts));
    case 'ul': return h('ul', item.items.map(parts => h('li', inline(parts))));
    case 'ol': return h('ol', { start: item.start }, item.items.map(parts => h('li', inline(parts))));
    case 'quote': return h('blockquote', item.blocks.map(block));
    case 'pre': return h('pre', [h('code', item.text)]);
    case 'hr': return h('hr');
    default: return h('p', inline(item.parts));
  }
}

export default defineComponent({
  name: 'MarkdownText',
  props: { text: { type: String as PropType<string>, required: true } },
  render() {
    return h('div', { class: 'markdown' }, parseMarkdown(this.text).map(block));
  },
});
</script>

<style scoped>
.markdown { display: flow-root; }
.markdown :deep(> :first-child) { margin-top: 0; }
.markdown :deep(> :last-child) { margin-bottom: 0; }
.markdown :deep(p) { margin: 0 0 8px; }
.markdown :deep(h3), .markdown :deep(h4), .markdown :deep(h5), .markdown :deep(h6) {
  margin: 12px 0 6px;
  font: 600 1em var(--font-title, inherit);
}
.markdown :deep(ul), .markdown :deep(ol) { margin: 6px 0 8px; padding-left: 1.4em; }
.markdown :deep(li) { margin: 2px 0; }
.markdown :deep(code) {
  font-family: var(--font-mono, ui-monospace, monospace);
  font-size: 0.92em;
  padding: 1px 5px;
  border-radius: 5px;
  background: color-mix(in srgb, currentColor 12%, transparent);
  overflow-wrap: anywhere;
}
.markdown :deep(pre) {
  margin: 8px 0;
  padding: 10px 12px;
  border-radius: 10px;
  background: color-mix(in srgb, currentColor 10%, transparent);
  overflow-x: auto;
}
.markdown :deep(pre code) { padding: 0; background: none; white-space: pre; }
.markdown :deep(blockquote) {
  margin: 8px 0;
  padding-left: 10px;
  border-left: 3px solid color-mix(in srgb, currentColor 30%, transparent);
  opacity: 0.88;
}
.markdown :deep(hr) { margin: 10px 0; border: 0; border-top: 1px solid color-mix(in srgb, currentColor 22%, transparent); }
.markdown :deep(a) { color: inherit; text-decoration: underline; overflow-wrap: anywhere; }
</style>
