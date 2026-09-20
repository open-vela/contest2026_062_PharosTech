/* The feeder demo tree from Shared/nyaui/nyaui.md §5 (Simulator built-in). */
import type { NyaUiTree } from '../src/types.js';

export function feederTree(): NyaUiTree {
  return JSON.parse(JSON.stringify(FEEDER)) as NyaUiTree;
}

const FEEDER: NyaUiTree = {
  nyaui: 1,
  page: { title: '智能喂食器', icon: 'pets', placement: 'menu' },
  root: {
    type: 'column',
    props: { gap: 12, padding: 16 },
    children: [
      {
        type: 'row',
        props: { gap: 12 },
        children: [
          {
            type: 'statCard',
            id: 'food',
            props: { label: '余粮', value: '72', unit: '%', trend: 'down', icon: 'inventory' },
          },
          {
            type: 'statCard',
            id: 'fedToday',
            props: { label: '今日已喂', value: '3', unit: '次', icon: 'restaurant' },
          },
        ],
      },
      {
        type: 'card',
        props: { title: '手动喂食' },
        children: [
          {
            type: 'slider',
            id: 'amount',
            props: { min: 5, max: 50, step: 5, value: 20, label: '出粮克数' },
          },
          {
            type: 'button',
            id: 'feed',
            props: { text: '立即喂食', variant: 'filled', icon: 'play_arrow' },
            on: { tap: { command: 'feed.now', args: { $grams: 'amount' } } },
          },
        ],
      },
      {
        type: 'card',
        props: { title: '定时' },
        children: [
          {
            type: 'switch',
            id: 'sched',
            props: { value: true, label: '定时喂食' },
            on: { change: { command: 'feed.schedule.toggle' } },
          },
          {
            type: 'chips',
            id: 'times',
            props: {
              options: ['07:00', '12:00', '19:00', '22:00'],
              value: ['07:00', '19:00'],
              multi: true,
              label: '时间点',
            },
            on: { change: { command: 'feed.schedule.set' } },
          },
        ],
      },
      {
        type: 'chart',
        props: {
          kind: 'bar',
          points: [3, 2, 4, 3, 5, 3, 2],
          labels: ['一', '二', '三', '四', '五', '六', '日'],
          height: 120,
        },
      },
    ],
  },
};
