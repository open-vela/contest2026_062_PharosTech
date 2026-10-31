import { describe, expect, it } from 'vitest';
import { weatherSceneData } from '../src/weatherScene.js';

describe('weather scene real data', () => {
  it('shows zero temperature, humidity and feels-like without demo substitutions', () => {
    expect(weatherSceneData({ temperature_c:0, humidity_percent:0, feels_like_c:0, title:'测试城市' }, 'snow'))
      .toMatchObject({ value:'0°', left:'0%', right:'0°', city:'测试城市' });
  });
  it('keeps unknown fields unknown, including native snapshots full of zero defaults', () => {
    expect(weatherSceneData({}, 'sunny')).toMatchObject({ value:'--', left:'--', right:'--' });
    expect(weatherSceneData({ temperature_c:0, humidity_percent:0, weather_fields:0 }, 'rain').value).toBe('--');
    expect(weatherSceneData({ temperature_c:0, humidity_percent:0, weather_fields:1 }, 'rain'))
      .toMatchObject({ value:'0°', left:'--' });
  });
  it('uses wind and visibility in their appropriate scene variants', () => {
    expect(weatherSceneData({ wind_kph:0, humidity_percent:75 }, 'storm'))
      .toMatchObject({ leftLabel:'风速', left:'0', right:'75%' });
    expect(weatherSceneData({ visibility_km:0 }, 'fog')).toMatchObject({ leftLabel:'能见', left:'0.0' });
  });
});
