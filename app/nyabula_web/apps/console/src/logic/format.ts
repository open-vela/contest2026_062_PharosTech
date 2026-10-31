/* Display formatters shared by every skin. */

export function fmtNum(n: number): string {
  if (n >= 1e6) return (n / 1e6).toFixed(n >= 1e7 ? 1 : 2) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'k';
  return String(Math.round(n));
}

export function fmtHours(sec: number): string {
  return `${(sec / 3600).toFixed(1)}h`;
}

export function fmtFrames(n: number): string {
  return n.toLocaleString();
}

/** "YYYY-MM-DD" -> "MM-DD". */
export function dayLabel(date: string): string {
  return date.slice(5);
}

export function fmtDateTime(s: string): string {
  if (!s) return '—';
  const d = new Date(s);
  return isNaN(d.getTime()) ? s : d.toLocaleString();
}

export function fmtDate(s: string): string {
  if (!s) return '—';
  const d = new Date(s);
  return isNaN(d.getTime()) ? s : d.toLocaleDateString();
}

/** Relative time for lastSeen ("刚刚" / "5 分钟前" / date). */
export function fmtAgo(s: string, now = Date.now()): string {
  if (!s) return '—';
  const t = new Date(s).getTime();
  if (isNaN(t)) return s;
  const sec = Math.max(0, Math.round((now - t) / 1000));
  if (sec < 60) return '刚刚';
  const m = Math.round(sec / 60);
  if (m < 60) return `${m} 分钟前`;
  const h = Math.round(m / 60);
  if (h < 48) return `${h} 小时前`;
  return `${Math.round(h / 24)} 天前`;
}

/** Local "YYYY-MM-DD" (matches the Cloud's time.Now().Format date keys). */
export function isoDay(d: Date): string {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

/** The last `n` calendar days ending today, oldest first. */
export function lastDays(n: number, today = new Date()): string[] {
  return Array.from({ length: n }, (_, i) => {
    const d = new Date(today);
    d.setDate(d.getDate() - (n - 1 - i));
    return isoDay(d);
  });
}
