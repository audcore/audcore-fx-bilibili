const videoId = /^(BV[0-9A-Za-z]{10}|av[1-9][0-9]{0,18})$/i;

export function parseVideo(input) {
  const clean = input.trim();
  if (!clean || clean.length > 512) return null;
  const direct = clean.match(videoId)?.[1];
  let id = direct;
  let part = 1;
  let time = 0;
  if (!direct) {
    let url;
    try { url = new URL(/^bilibili\.com\/|^www\.bilibili\.com\//i.test(clean) ? 'https://' + clean : clean); }
    catch { return null; }
    if (!['https:', 'http:'].includes(url.protocol) ||
        !['www.bilibili.com', 'bilibili.com', 'm.bilibili.com'].includes(url.hostname) ||
        url.username || url.password || url.port) return null;
    id = url.pathname.match(/^\/video\/([^/]+)\/?$/i)?.[1]?.match(videoId)?.[1];
    part = integer(url.searchParams.get('p'), 1, 9999);
    time = integer(url.searchParams.get('t'), 0, 864000);
  }
  return id ? {id: id.slice(0, 2).toUpperCase() + id.slice(2), part, time} : null;
}

function integer(value, fallback, max) {
  const number = Number(value);
  return value && Number.isSafeInteger(number) && number >= fallback && number <= max ? number : fallback;
}

export function routeUrl(current, route) {
  const path = new URL(current);
  path.search = '';
  if (route.q) path.searchParams.set('q', route.q);
  if (route.id) {
    path.searchParams.set('video', route.id);
    if (route.part > 1) path.searchParams.set('p', route.part);
    if (route.time) path.searchParams.set('t', route.time);
  }
  return path;
}

export function routeFromUrl(url) {
  const params = new URL(url).searchParams;
  const id = params.get('video');
  if (id && videoId.test(id)) return {id, part: integer(params.get('p'), 1, 9999), time: integer(params.get('t'), 0, 864000)};
  return {q: params.get('q')?.slice(0, 100) || ''};
}

export function formatTime(seconds) {
  const value = Math.floor(seconds);
  if (!Number.isFinite(value) || value < 0) return '0:00';
  return [Math.floor(value / 60), String(value % 60).padStart(2, '0')].join(':');
}
