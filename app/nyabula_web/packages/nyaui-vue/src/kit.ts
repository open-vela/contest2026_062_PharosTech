/* Feature Kit registration — maps @nyabula/ui Nk* components onto NyaUI DSL
 * types (nyaui.md "v1.1 proposal"). NkRow is exposed as `settingRow` because
 * v1 `row` is the layout container; NkBanner as `notice` for the same reason.
 * Each wrapper receives the NyaUiNode,
 * pulls props from node.props and plumbs input values / events through
 * NYAUI_CTX exactly like the built-in slider / switch branches:
 *   - inputs: ctx.getValue(id) overlays props.value; ctx.setValue(node, v,
 *     immediate) records the optimistic edit and fires `change`.
 *   - actions: ctx.fire(node, event) — `tap` for tiles/rows/contacts,
 *     named events for the media player / action bar / banner.
 * Container kinds render their children through NyaUiNode recursively. */
import { defineComponent, h, inject, type Component, type PropType, type VNode } from 'vue';
import {
  NkActionBar,
  NkBanner,
  NkChipSelect,
  NkColorSwatch,
  NkContactList,
  NkDial,
  NkGauge,
  NkHeader,
  NkKeyValue,
  NkListSection,
  NkMediaPlayer,
  NkProgressRing,
  NkRow,
  NkSegmentRow,
  NkSliderRow,
  NkStatTile,
  NkTile,
  NkTimeWheel,
  NkToggleRow,
  NkWeatherCard,
} from '@nyabula/ui';
import { NYAUI_CTX, type NyaUiContext } from './context.js';
import { registerNyaComponent, type NyaComponentKind } from './registry.js';
import type { NyaUiNode } from './types.js';
import NyaUiNodeComp from './components/NyaUiNode.vue';

type Props = Record<string, unknown>;

function num(v: unknown, dflt: number): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : dflt;
}
function optNum(v: unknown): number | undefined {
  return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
}
function str(v: unknown, dflt = ''): string {
  return typeof v === 'string' ? v : dflt;
}
function optStr(v: unknown): string | undefined {
  return typeof v === 'string' ? v : undefined;
}
function bool(v: unknown, dflt = false): boolean {
  return typeof v === 'boolean' ? v : dflt;
}
function arr<T = unknown>(v: unknown): T[] {
  return Array.isArray(v) ? (v as T[]) : [];
}
function strOrNum(v: unknown, dflt: string | number = ''): string | number {
  return typeof v === 'string' || typeof v === 'number' ? v : dflt;
}

/** Build a wrapper component for one DSL type. */
function wrap(
  name: string,
  render: (node: NyaUiNode, p: Props, ctx: NyaUiContext, kids: () => VNode[]) => VNode,
): Component {
  return defineComponent({
    name: `NyaKit_${name}`,
    props: { node: { type: Object as PropType<NyaUiNode>, required: true } },
    setup(props) {
      const ctx = inject(NYAUI_CTX)!;
      const kids = (): VNode[] =>
        (props.node.children ?? []).map((c, i) => h(NyaUiNodeComp, { key: c.id ?? i, node: c }));
      return () => render(props.node, props.node.props ?? {}, ctx, kids);
    },
  });
}

/** Current value of an input node: optimistic overlay first, props.value fallback. */
function val(node: NyaUiNode, p: Props, ctx: NyaUiContext): unknown {
  const v = ctx.getValue(node.id);
  return v === undefined ? p.value : v;
}

/* Volume slider streams values; coalesce to one `volume` event per 300 ms. */
const volumeTimers = new WeakMap<NyaUiNode, ReturnType<typeof setTimeout>>();
function volumeDebounced(node: NyaUiNode, ctx: NyaUiContext, v: number): void {
  const t = volumeTimers.get(node);
  if (t) clearTimeout(t);
  volumeTimers.set(
    node,
    setTimeout(() => {
      volumeTimers.delete(node);
      ctx.fire(node, 'volume', { volume: v });
    }, 300),
  );
}

interface KitDef {
  type: string;
  kind: NyaComponentKind;
  component: Component;
}

export const KIT_DEFS: KitDef[] = [
  {
    type: 'header',
    kind: 'display',
    component: wrap('header', (_n, p) =>
      h(NkHeader, {
        icon: optStr(p.icon),
        title: str(p.title),
        subtitle: optStr(p.subtitle),
        tone: (optStr(p.tone) as 'default' | 'ok' | 'warn' | 'error' | undefined) ?? 'default',
      }),
    ),
  },
  {
    type: 'tile',
    kind: 'display',
    component: wrap('tile', (node, p, ctx) =>
      h(NkTile, {
        icon: optStr(p.icon),
        title: str(p.title),
        sub: optStr(p.sub),
        value: p.value === undefined ? undefined : strOrNum(p.value),
        active: bool(p.active),
        layout: str(p.layout) === 'wide' ? 'wide' : 'square',
        disabled: bool(p.disabled),
        onTap: () => ctx.fire(node, 'tap'),
      }),
    ),
  },
  {
    type: 'statTile',
    kind: 'display',
    component: wrap('statTile', (_n, p) =>
      h(NkStatTile, {
        value: strOrNum(p.value),
        unit: optStr(p.unit),
        label: str(p.label),
        icon: optStr(p.icon),
        trend: optStr(p.trend) as 'up' | 'down' | 'flat' | undefined,
        trendText: optStr(p.trendText),
      }),
    ),
  },
  {
    type: 'settingRow',
    kind: 'display',
    component: wrap('settingRow', (node, p, ctx) =>
      h(NkRow, {
        icon: optStr(p.icon),
        title: str(p.title),
        sub: optStr(p.sub),
        tappable: !!node.on?.tap,
        disabled: bool(p.disabled),
        onTap: () => ctx.fire(node, 'tap'),
      }, p.trailing !== undefined ? () => h('span', { class: 'nya-kit-trailing' }, String(p.trailing)) : undefined),
    ),
  },
  {
    type: 'toggleRow',
    kind: 'input',
    component: wrap('toggleRow', (node, p, ctx) =>
      h(NkToggleRow, {
        modelValue: val(node, p, ctx) === true,
        icon: optStr(p.icon),
        title: str(p.title),
        sub: optStr(p.sub),
        disabled: bool(p.disabled),
        'onUpdate:modelValue': (v: boolean) => ctx.setValue(node, v, true),
      }),
    ),
  },
  {
    type: 'sliderRow',
    kind: 'input',
    component: wrap('sliderRow', (node, p, ctx) =>
      h(NkSliderRow, {
        modelValue: num(val(node, p, ctx), num(p.min, 0)),
        title: str(p.title),
        min: num(p.min, 0),
        max: num(p.max, 100),
        step: num(p.step, 1),
        unit: str(p.unit),
        iconStart: optStr(p.iconStart),
        iconEnd: optStr(p.iconEnd),
        disabled: bool(p.disabled),
        // Drag: optimistic only. Release: fire change (contract: slider fires on release).
        'onUpdate:modelValue': (v: number) => ctx.setValue(node, v, false),
        onCommit: (v: number) => ctx.setValue(node, v, true),
      }),
    ),
  },
  {
    type: 'segmentRow',
    kind: 'input',
    component: wrap('segmentRow', (node, p, ctx) =>
      h(NkSegmentRow, {
        modelValue: str(val(node, p, ctx)),
        title: str(p.title),
        sub: optStr(p.sub),
        items: arr<{ id: string; label: string; icon?: string }>(p.items).map((it) => ({
          id: str(it?.id),
          label: str(it?.label, str(it?.id)),
          icon: optStr(it?.icon),
        })),
        stacked: bool(p.stacked),
        'onUpdate:modelValue': (v: string) => ctx.setValue(node, v, true),
      }),
    ),
  },
  {
    type: 'chipSelect',
    kind: 'input',
    component: wrap('chipSelect', (node, p, ctx) => {
      const v = val(node, p, ctx);
      return h(NkChipSelect, {
        modelValue: Array.isArray(v) ? v.map(String) : optStr(v),
        options: arr<{ id: string; label: string; icon?: string } | string>(p.options).map((o) =>
          typeof o === 'string' ? { id: o, label: o } : { id: str(o?.id), label: str(o?.label, str(o?.id)), icon: optStr(o?.icon) },
        ),
        multi: bool(p.multi),
        label: optStr(p.label),
        disabled: bool(p.disabled),
        'onUpdate:modelValue': (nv: string | string[]) => ctx.setValue(node, nv, true),
      });
    }),
  },
  {
    type: 'gauge',
    kind: 'display',
    component: wrap('gauge', (_n, p) =>
      h(NkGauge, {
        value: num(p.value, 0),
        label: optStr(p.label),
        unit: str(p.unit, '%'),
        size: num(p.size, 120),
        stroke: num(p.stroke, 10),
        warnAt: optNum(p.warnAt),
        errorAt: optNum(p.errorAt),
        invert: bool(p.invert),
      }),
    ),
  },
  {
    type: 'progressRing',
    kind: 'display',
    component: wrap('progressRing', (_n, p) =>
      h(
        NkProgressRing,
        {
          value: num(p.value, 0),
          size: num(p.size, 96),
          stroke: num(p.stroke, 8),
          indeterminate: bool(p.indeterminate),
          tone: (optStr(p.tone) as 'primary' | 'ok' | 'warn' | 'error' | undefined) ?? 'primary',
        },
        p.text !== undefined ? () => String(p.text) : undefined,
      ),
    ),
  },
  {
    type: 'dial',
    kind: 'input',
    component: wrap('dial', (node, p, ctx) =>
      h(NkDial, {
        modelValue: num(val(node, p, ctx), 0),
        min: num(p.min, 0),
        max: num(p.max, 99 * 60 + 59),
        step: num(p.step, 1),
        presets: arr<{ label: string; seconds: number }>(p.presets)
          .filter((x) => x && typeof x.seconds === 'number')
          .map((x) => ({ label: str(x.label, String(x.seconds)), seconds: x.seconds })),
        disabled: bool(p.disabled),
        'onUpdate:modelValue': (v: number) => ctx.setValue(node, v, false),
      }),
    ),
  },
  {
    type: 'timeWheel',
    kind: 'input',
    component: wrap('timeWheel', (node, p, ctx) =>
      h(NkTimeWheel, {
        modelValue: str(val(node, p, ctx), '00:00'),
        minuteStep: num(p.minuteStep, 1),
        disabled: bool(p.disabled),
        'onUpdate:modelValue': (v: string) => ctx.setValue(node, v, false),
      }),
    ),
  },
  {
    type: 'mediaPlayer',
    kind: 'display',
    component: wrap('mediaPlayer', (node, p, ctx) => {
      const lyrics = p.lyrics && typeof p.lyrics === 'object' ? (p.lyrics as Props) : undefined;
      return h(NkMediaPlayer, {
        title: optStr(p.title),
        artist: optStr(p.artist),
        cover: optStr(p.cover),
        playing: bool(p.playing),
        position: num(p.position, 0),
        duration: num(p.duration, 0),
        volume: num(p.volume, 50),
        showVolume: bool(p.showVolume, true),
        lyrics: lyrics ? { prev: optStr(lyrics.prev), current: optStr(lyrics.current), next: optStr(lyrics.next) } : undefined,
        disabled: bool(p.disabled),
        onPlay: () => ctx.fire(node, 'play'),
        onPause: () => ctx.fire(node, 'pause'),
        onPrev: () => ctx.fire(node, 'prev'),
        onNext: () => ctx.fire(node, 'next'),
        // seek / volume carry a payload merged into the command args.
        onSeek: (s: number) => ctx.fire(node, 'seek', { position: s }),
        onVolume: (v: number) => volumeDebounced(node, ctx, v),
      });
    }),
  },
  {
    type: 'listSection',
    kind: 'container',
    component: wrap('listSection', (_n, p, _ctx, kids) =>
      h(NkListSection, { title: optStr(p.title), card: bool(p.card, true) }, () => kids()),
    ),
  },
  {
    type: 'actionBar',
    kind: 'display',
    component: wrap('actionBar', (node, p, ctx) =>
      h(NkActionBar, {
        primaryText: str(p.primaryText, '确定'),
        primaryIcon: optStr(p.primaryIcon),
        secondaryText: optStr(p.secondaryText),
        secondaryIcon: optStr(p.secondaryIcon),
        fixed: bool(p.fixed),
        disabled: bool(p.disabled),
        busy: bool(p.busy),
        danger: bool(p.danger),
        onPrimary: () => ctx.fire(node, 'primary'),
        onSecondary: () => ctx.fire(node, 'secondary'),
      }),
    ),
  },
  {
    type: 'notice',
    kind: 'display',
    component: wrap('notice', (node, p, ctx) =>
      h(NkBanner, {
        text: str(p.text),
        title: optStr(p.title),
        tone: (optStr(p.tone) as 'info' | 'ok' | 'warn' | 'error' | undefined) ?? 'info',
        icon: optStr(p.icon),
        closable: bool(p.closable),
        actionText: optStr(p.actionText),
        onClose: () => ctx.fire(node, 'close'),
        onAction: () => ctx.fire(node, 'action'),
      }),
    ),
  },
  {
    type: 'keyValue',
    kind: 'display',
    component: wrap('keyValue', (_n, p) =>
      h(NkKeyValue, {
        items: arr<Props>(p.items)
          .filter((it) => it && typeof it === 'object')
          .map((it) => ({
            key: str(it.key),
            value: strOrNum(it.value),
            mono: bool(it.mono),
            tone: optStr(it.tone) as 'default' | 'primary' | 'ok' | 'warn' | 'error' | undefined,
          })),
        columns: num(p.columns, 1) === 2 ? 2 : 1,
      }),
    ),
  },
  {
    type: 'colorSwatch',
    kind: 'input',
    component: wrap('colorSwatch', (node, p, ctx) =>
      h(NkColorSwatch, {
        modelValue: str(val(node, p, ctx), '#62dfaf'),
        presets: Array.isArray(p.presets) ? p.presets.map(String) : undefined,
        label: optStr(p.label),
        custom: bool(p.custom, true),
        disabled: bool(p.disabled),
        'onUpdate:modelValue': (v: string) => ctx.setValue(node, v, true),
      }),
    ),
  },
  {
    type: 'weatherCard',
    kind: 'display',
    component: wrap('weatherCard', (_n, p) =>
      h(NkWeatherCard, {
        city: str(p.city),
        temp: strOrNum(p.temp, '--'),
        unit: str(p.unit, '°'),
        text: optStr(p.text),
        kind: (optStr(p.kind) as 'sunny' | 'cloudy' | 'rain' | 'snow' | 'storm' | 'fog' | undefined) ?? 'sunny',
        high: p.high === undefined ? undefined : strOrNum(p.high),
        low: p.low === undefined ? undefined : strOrNum(p.low),
        extra: optStr(p.extra),
      }),
    ),
  },
  {
    type: 'contactList',
    kind: 'display',
    component: wrap('contactList', (node, p, ctx) =>
      h(NkContactList, {
        contacts: arr<Props>(p.contacts)
          .filter((c) => c && typeof c === 'object')
          .map((c) => ({
            id: str(c.id, str(c.name)),
            name: str(c.name),
            status: optStr(c.status),
            avatar: optStr(c.avatar),
            online: typeof c.online === 'boolean' ? c.online : undefined,
            actionIcon: optStr(c.actionIcon),
          })),
        emptyText: optStr(p.emptyText),
        // The tapped contact id travels in the command args as `contact`.
        onTap: (c: { id: string }) => ctx.fire(node, 'tap', { contact: c.id }),
        onAction: (c: { id: string }) => ctx.fire(node, 'action', { contact: c.id }),
      }),
    ),
  },
];

/** DSL type names provided by the kit (for docs / validation). */
export const KIT_TYPES: string[] = KIT_DEFS.map((d) => d.type);

let unregister: (() => void)[] | null = null;

/** Register every kit component. Idempotent; returns an unregister fn. */
export function registerKit(): () => void {
  if (!unregister) {
    unregister = KIT_DEFS.map((d) => registerNyaComponent(d));
  }
  return () => {
    unregister?.forEach((fn) => fn());
    unregister = null;
  };
}
