/** Presence, not truthiness, distinguishes zero from an unavailable reading. */
export function weatherSceneData(payload: Record<string, unknown>, kind: string) {
  const fields = ['temperature_c', 'feels_like_c', 'humidity_percent', 'wind_kph', 'visibility_km'];
  const explicit = typeof payload.weather_fields === 'number' ? payload.weather_fields : null;
  const has = (field: string) => typeof payload[field] === 'number' && Number.isFinite(payload[field]) &&
    (explicit === null || !!(explicit & (1 << fields.indexOf(field))));
  const round = (field: string) => Math.round(Number(payload[field]));
  let leftLabel = '湿度', rightLabel = '体感';
  let left = has('humidity_percent') ? String(round('humidity_percent')) + '%' : '--';
  let right = has('feels_like_c') ? String(round('feels_like_c')) + '°' : '--';
  if (kind === 'storm' && has('wind_kph')) { leftLabel = '风速'; left = String(round('wind_kph')); }
  if (kind === 'fog' && has('visibility_km')) { leftLabel = '能见'; left = Number(payload.visibility_km).toFixed(1); }
  if (kind === 'storm' && has('humidity_percent')) { rightLabel = '湿度'; right = String(round('humidity_percent')) + '%'; }
  const names: Record<string, string> = { sunny:'晴朗', cloudy:'多云', rain:'小雨', storm:'雷雨', snow:'降雪', fog:'有雾' };
  return {
    value: has('temperature_c') ? String(round('temperature_c')) + '°' : '--',
    title: fields.some(has) ? (typeof payload.subtitle === 'string' && payload.subtitle ? payload.subtitle : names[kind] ?? '--') : '--',
    city: typeof payload.title === 'string' ? payload.title : '',
    leftLabel, left, rightLabel, right,
  };
}
