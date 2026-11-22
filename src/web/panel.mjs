import {parseVideo, routeUrl, routeFromUrl, formatTime} from './routes.mjs';

const $ = (id) => document.getElementById(id);
const video = $('video');
const picture = $('picture');
const host = window.chrome?.webview;
const chinese = {
  name: '哔哩哔哩', search: '搜索', try: '试试', example: '音乐现场', results: '搜索结果',
  more: '加载更多', back: '返回', danmaku: '弹幕', parts: '选集',
  play: '播放', pause: '暂停', query: '关键词、BV / AV 号或视频链接', seek: '播放进度',
  loading: '正在加载…', none: '未找到相关视频', failed: '搜索失败，请重试',
  unavailable: '这个视频暂时无法播放', audio: '请先启动声核音频', blocked: '点击播放开始观看', hardware: '当前显卡无法硬解此视频', render: '当前 WebView2 无法显示视频'
};
const english = {
  name: 'bilibili', search: 'Search', try: 'Try', example: 'live music', results: 'Results',
  more: 'More', back: 'Back', danmaku: 'Danmaku', parts: 'Parts',
  play: 'Play', pause: 'Pause', query: 'Keywords, BV / AV, or video URL', seek: 'Playback position',
  loading: 'Loading…', none: 'No videos found', failed: 'Search failed. Try again.',
  unavailable: 'This video is unavailable', audio: 'Start Audcore audio first', blocked: 'Press Play to watch', hardware: 'Hardware decoding unavailable', render: 'Update WebView2 to display video'
};
const japanese = {
  name: 'ビリビリ', search: '検索', try: '例', example: '音楽ライブ', results: '検索結果',
  more: 'もっと見る', back: '戻る', danmaku: '弾幕', parts: '話数',
  play: '再生', pause: '一時停止', query: 'キーワード、BV / AV、動画URL', seek: '再生位置',
  loading: '読込中…', none: '動画が見つかりません', failed: '検索に失敗しました',
  unavailable: '再生できない動画です', audio: '声核の音声を開始してください', blocked: '再生ボタンを押してください', hardware: 'ハードウェア復号不可', render: 'WebView2 の更新が必要です'
};
let words = chinese;
let controller;
let searchCache;
let rackReady = !host;
let requestedPlay = false;
let currentId = 0;
let nativePosition = 0;
let nativeDuration = 0;
let positionAt = 0;
let nativeState = 0;
let pendingMedia;
if (host) {
  video.hidden = true;
  picture.hidden = false;
}
let comments = [];
let onscreen = [];
let nextComment = 0;
let animation = 0;
let scrubbing = false;

function locale(lang) {
  document.documentElement.lang = lang;
  words = lang.startsWith('ja') ? japanese : lang.startsWith('en') ? english : chinese;
  document.title = words.name;
  document.querySelectorAll('[data-text]').forEach(element => { element.textContent = words[element.dataset.text]; });
  $('query').placeholder = words.query;
  $('query').setAttribute('aria-label', words.search);
  $('position').setAttribute('aria-label', words.seek);
  $('player').setAttribute('aria-label', words.danmaku);
  $('back').setAttribute('aria-label', words.back);
  $('back').title = words.back;
  $('danmaku').setAttribute('aria-label', words.danmaku);
  $('danmaku').parentElement.title = words.danmaku;
  const picker = $('parts').querySelector('summary');
  picker.title = words.parts;
  picker.setAttribute('aria-label', words.parts);
  playbackControls();
}
function position() {
  if (!host) return video.currentTime;
  const elapsed = nativeState === 3 ? (performance.now() - positionAt) / 1000 : 0;
  return Math.min(nativeDuration || Infinity, nativePosition + elapsed);
}
function duration() { return host ? nativeDuration : video.duration; }
function paused() { return host ? nativeState !== 3 : video.paused; }

host?.addEventListener('message', ({data}) => {
  if (data.lang && data.lang !== document.documentElement.lang) locale(data.lang);
  if ('ready' in data) {
    rackReady = data.ready;
    if (rackReady && pendingMedia) sendNativeMedia();
  }
  if (data.selected && !routeFromUrl(location.href).id) {
    const selection = parseVideo(data.selected);
    if (selection) open({...selection, part: data.part || 1}, true);
  }
  if (data.id !== currentId || !data.id) return;
  const previous = nativeState;
  nativeState = data.state;
  nativePosition = data.pos / 1000;
  nativeDuration = data.duration / 1000;
  positionAt = performance.now();
  if (nativeState === 2 || nativeState === 3) $('player-status').textContent = '';
  if (nativeState === 5) {
    requestedPlay = false;
    $('player-status').textContent = data.code === -2005270524 ? words.hardware : words.unavailable;
  }
  if (nativeState === 4) requestedPlay = false;
  if (nativeState !== previous) updateComments();
  playbackControls();
});
host?.addEventListener('sharedbufferreceived', event => {
  const buffer = event.getBuffer();
  const data = event.additionalData;
  try {
    if (data.id !== currentId) return;
    const frame = new VideoFrame(buffer, {
      format: 'NV12', codedWidth: data.w, codedHeight: data.h, timestamp: data.ts,
      colorSpace: {matrix: data.matrix, fullRange: data.full, primaries: 'bt709', transfer: 'bt709'}
    });
    try {
      if (picture.width !== data.w || picture.height !== data.h) {
        picture.width = data.w;
        picture.height = data.h;
      }
      picture.getContext('2d', {alpha: false}).drawImage(frame, 0, 0);
    } finally { frame.close(); }
  } catch {
    $('player-status').textContent = words.render;
  } finally {
    host.releaseBuffer(buffer);
    host.postMessage('frame:' + data.seq);
  }
});
if (!host && navigator.language.startsWith('ja')) locale('ja');
host?.postMessage('ready');

function api(name, parameters, signal) {
  return fetch('api/' + name + '?' + new URLSearchParams(parameters), {signal}).then(response => {
    if (!response.ok) throw new Error(name);
    return name === 'dm' ? response.text() : response.json().then(json => {
      if (json.code !== 0) throw new Error(name);
      return json.data;
    });
  });
}

function open(route, replace = false) {
  const previous = history.state || {};
  const scroll = route.id && !$('results').hidden ? window.scrollY : 0;
  if (scroll || route.id && previous.index > 0) history.replaceState({...previous, scroll}, '');
  history[replace ? 'replaceState' : 'pushState']({index: replace ? 0 : (previous.index || 0) + 1}, '', routeUrl(location.href, route));
  void render();
}

function submit(input) {
  const value = input.trim();
  if (!value) return;
  const target = parseVideo(value);
  open(target || {q: value.slice(0, 100)});
}

$('search').addEventListener('submit', event => {
  event.preventDefault();
  submit($('query').value);
});
$('example-search').onclick = () => submit(words.example);
$('example-video').onclick = () => submit($('example-video').dataset.video);
window.addEventListener('popstate', render);

function titleText(title) {
  const doc = new DOMParser().parseFromString(String(title || ''), 'text/html');
  return doc.body.textContent || '';
}

function coverUrl(value) {
  try {
    const url = new URL(String(value).replace(/^\/\//, 'https://'));
    if (/(^|\.)hdslb\.com$/i.test(url.hostname)) {
      url.protocol = 'https:';
      return url.href;
    }
  } catch {}
  return '';
}

function addCards(rows) {
  for (const row of rows) {
    const target = parseVideo(row.bvid || 'av' + row.aid);
    if (!target) continue;
    const card = $('card').content.cloneNode(true);
    const link = card.querySelector('a');
    link.href = routeUrl(location.href, target);
    link.onclick = event => {
      if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      open(target);
    };
    const cover = coverUrl(row.pic);
    if (cover) {
      const image = card.querySelector('img');
      image.addEventListener('load', () => { image.setAttribute('data-loaded', ''); }, {once: true});
      image.src = 'api/cover?' + new URLSearchParams({url: cover});
    }
    card.querySelector('.duration').textContent = row.duration || '';
    card.querySelector('.card-detail strong').textContent = titleText(row.title);
    card.querySelector('.meta').textContent = row.author || '';
    $('cards').append(card);
  }
}

async function search(query, signal, more = false) {
  if (!more) $('cards').replaceChildren();
  if (searchCache?.query === query && !more) {
    addCards(searchCache.rows);
    $('status').textContent = searchCache.rows.length ? '' : words.none;
    $('more').hidden = searchCache.page >= searchCache.total;
    window.scrollTo(0, history.state?.scroll || 0);
    return;
  }
  if (!more) $('more').hidden = true;
  $('more').disabled = true;
  $('status').textContent = words.loading;
  try {
    const page = more ? searchCache.page + 1 : 1;
    const data = await api('search', {keyword: query, page}, signal);
    signal.throwIfAborted();
    const rows = (data.result || []).filter(item => item.type === 'video');
    searchCache = {query, page, total: data.numPages || 0, rows: more ? [...searchCache.rows, ...rows] : rows};
    addCards(rows);
    $('status').textContent = searchCache.rows.length ? '' : words.none;
    $('more').hidden = page >= searchCache.total || !rows.length;
  } catch (error) {
    if (!signal.aborted) $('status').textContent = words.failed;
  } finally {
    if (!signal.aborted) $('more').disabled = false;
  }
}
$('more').onclick = () => search(routeFromUrl(location.href).q, controller.signal, true);

function sendNativeMedia() {
  const media = pendingMedia;
  pendingMedia = undefined;
  if (!media) return;
  host.postMessage('select:' + media.bvid + ':' + media.part);
  host.postMessage('load:' + currentId + ':' + Math.round(media.time * 1000) + ':' + media.path);
  if (requestedPlay) host.postMessage('play');
}

async function startPlayback() {
  requestedPlay = true;
  if (host) {
    if (!rackReady) $('player-status').textContent = words.audio;
    else if (pendingMedia) sendNativeMedia();
    else host.postMessage('play');
    return;
  }
  if (!video.getAttribute('src')) return;
  try { await video.play(); }
  catch { $('player-status').textContent = words.blocked; }
}

function renderComments() {
  const now = position();
  const layer = $('danmaku-layer');
  const width = layer.clientWidth;
  const height = layer.clientHeight;
  while (nextComment < comments.length && comments[nextComment].time <= now) {
    const comment = comments[nextComment++];
    if (now - comment.time > 0.3 || width < 1 || height < 30) continue;
    const kind = comment.mode === 4 ? 'bottom' : comment.mode === 5 ? 'top' : 'scroll';
    const lanes = Math.max(1, Math.min(10, Math.floor(height * 0.45 / 27)));
    const line = Array.from({length: lanes}, (_, index) => index).find(index => !onscreen.some(item =>
      item.kind === kind && item.line === index &&
      (kind !== 'scroll' || (now - item.time) / 7 * (width + item.width) < item.width + 24)));
    if (line === undefined) continue;
    const span = document.createElement('span');
    span.textContent = comment.text;
    span.style.color = '#' + comment.color.toString(16).padStart(6, '0');
    span.style[kind === 'bottom' ? 'bottom' : 'top'] = 5 + line * 27 + 'px';
    layer.append(span);
    onscreen.push({span, line, time: now, width: span.offsetWidth, kind});
  }
  onscreen = onscreen.filter(item => {
    const progress = (now - item.time) / (item.kind === 'scroll' ? 7 : 4);
    if (progress >= 1) { item.span.remove(); return false; }
    const x = item.kind === 'scroll' ? width - progress * (width + item.width) : (width - item.width) / 2;
    item.span.style.transform = 'translateX(' + x + 'px)';
    return true;
  });
  animation = requestAnimationFrame(renderComments);
}

function resetComments() {
  $('danmaku-layer').replaceChildren();
  onscreen = [];
  let low = 0;
  let high = comments.length;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (comments[middle].time < position()) low = middle + 1;
    else high = middle;
  }
  nextComment = low;
}
function updateComments() {
  cancelAnimationFrame(animation);
  if ($('danmaku').checked && !paused() && !video.seeking) renderComments();
}
$('danmaku').onchange = () => { resetComments(); updateComments(); };

function showParts(pages, route) {
  const picker = $('parts');
  const list = $('part-list');
  list.replaceChildren();
  picker.hidden = pages.length < 2;
  if (picker.hidden) return;
  for (const part of pages) {
    const row = document.createElement('li');
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = 'P' + part.page + ' · ' + part.part;
    if (part.page === route.part) button.setAttribute('aria-current', 'true');
    button.onclick = () => {
      picker.open = false;
      if (part.page === route.part) return;
      history.replaceState(history.state, '', routeUrl(location.href, {...route, part: part.page, time: 0}));
      void render();
    };
    row.append(button);
    list.append(row);
  }
}
document.addEventListener('pointerdown', event => {
  $('player').removeAttribute('data-keyboard');
  if (!$('parts').contains(event.target)) $('parts').open = false;
});
$('player').addEventListener('pointerleave', () => { $('parts').open = false; });

async function loadVideo(route, signal) {
  $('video-title').textContent = route.id;
  $('player-status').textContent = words.loading;
  try {
    const key = route.id.slice(0, 2).toLowerCase() === 'av' ? {aid: route.id.slice(2)} : {bvid: route.id};
    const details = await api('view', key, signal);
    signal.throwIfAborted();
    const part = details.pages?.find(page => page.page === route.part);
    if (!part) throw new Error('part');
    $('video-title').textContent = details.title;
    showParts(details.pages, route);
    void api('dm', {oid: part.cid}, signal).then(xml => {
      if (signal.aborted) return;
      comments = Array.from(new DOMParser().parseFromString(xml, 'text/xml').querySelectorAll('d'), entry => {
        const [time, mode, , color] = (entry.getAttribute('p') || '').split(',').map(Number);
        return {time, mode, color: (color || 0xffffff) & 0xffffff, text: entry.textContent.slice(0, 160)};
      }).filter(item => Number.isFinite(item.time) && [1, 4, 5].includes(item.mode)).sort((a, b) => a.time - b.time);
      resetComments();
    }).catch(() => {});
    const data = await api('play', {bvid: details.bvid, cid: part.cid}, signal);
    signal.throwIfAborted();
    if (data.durl?.length !== 1 || !data.durl[0].url) throw new Error('format');
    const path = 'api/media.mp4?' + new URLSearchParams({url: data.durl[0].url});
    if (host) {
      pendingMedia = {path, time: route.time, bvid: details.bvid, part: part.page};
      void startPlayback();
    } else {
      video.src = path + (route.time ? '#t=' + route.time : '');
      $('player-status').textContent = '';
      void startPlayback();
    }
  } catch {
    if (!signal.aborted) $('player-status').textContent = words.unavailable;
  }
}

function playbackControls() {
  const play = $('play');
  const playing = !paused();
  play.toggleAttribute('data-playing', playing);
  play.title = playing ? words.pause : words.play;
  play.setAttribute('aria-label', play.title);
  const total = duration();
  const length = Number.isFinite(total) ? total : 0;
  $('position').max = length || 1;
  $('position').disabled = !length;
  if (!scrubbing) $('position').value = position();
  $('time').value = formatTime(scrubbing ? Number($('position').value) : position()) + ' / ' + formatTime(length);
}
$('position').oninput = () => { scrubbing = true; playbackControls(); };
$('position').onchange = () => {
  if (Number.isFinite(duration())) {
    const target = Number($('position').value);
    if (host) {
      nativePosition = target;
      positionAt = performance.now();
      host.postMessage('seek:' + Math.round(target * 1000));
      resetComments();
    } else video.currentTime = target;
  }
  scrubbing = false;
  playbackControls();
};

function togglePlay() {
  if (paused()) void startPlayback();
  else {
    requestedPlay = false;
    if (host) host.postMessage('pause');
    else video.pause();
  }
}
$('play').onclick = togglePlay;
video.onclick = togglePlay;
picture.onclick = togglePlay;
for (const event of ['timeupdate', 'durationchange', 'play', 'pause', 'ended']) video.addEventListener(event, playbackControls);
video.onplaying = () => { $('player-status').textContent = ''; updateComments(); };
video.onwaiting = () => {
  if (!video.paused) $('player-status').textContent = words.loading;
};
video.onpause = updateComments;
video.onseeking = () => { resetComments(); updateComments(); };
video.onseeked = updateComments;
video.onerror = () => { if (video.getAttribute('src')) $('player-status').textContent = words.unavailable; };
$('back').onclick = () => {
  if (history.state?.index > 0) history.back();
  else open({q: ''}, true);
};
document.addEventListener('keydown', event => {
  $('player').setAttribute('data-keyboard', '');
  if ($('player').hidden || !['BODY', 'VIDEO'].includes(event.target.tagName) || event.code !== 'Space') return;
  event.preventDefault();
  togglePlay();
});

async function render() {
  controller?.abort();
  controller = new AbortController();
  const route = routeFromUrl(location.href);
  requestedPlay = false;
  video.pause();
  video.removeAttribute('src');
  video.load();
  if (host) {
    host.postMessage('clear');
    currentId = (currentId % 999999998) + 1;
    nativeState = 0;
    nativePosition = 0;
    nativeDuration = 0;
    positionAt = 0;
    pendingMedia = undefined;
    picture.getContext('2d', {alpha: false}).clearRect(0, 0, picture.width, picture.height);
  }
  comments = [];
  resetComments();
  $('parts').open = false;
  $('parts').hidden = true;
  $('player').hidden = !route.id;
  $('browse').hidden = Boolean(route.id);
  $('results').hidden = !route.q;
  $('browse').dataset.page = route.q ? 'results' : 'search';
  if (route.id) {
    playbackControls();
    await loadVideo(route, controller.signal);
  } else {
    $('query').value = route.q;
    if (route.q) await search(route.q, controller.signal);
    else $('query').focus({preventScroll: true});
  }
}
void render();
