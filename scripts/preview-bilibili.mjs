import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {Readable} from 'node:stream';
import {pipeline} from 'node:stream/promises';

const panel = new URL('../src/web/', import.meta.url);
const files = new Map([
  ['', ['index.html', 'text/html']], ['panel.css', ['panel.css', 'text/css']],
  ...['panel.mjs', 'routes.mjs'].map(name => [name, [name, 'text/javascript']]),
]);
const endpoints = new Map([
  ['search', '/x/web-interface/wbi/search/type?search_type=video&'],
  ['view', '/x/web-interface/view?'], ['play', '/x/player/playurl?qn=16&fnval=0&platform=html5&'],
  ['dm', '/x/v1/dm/list.so?'],
]);

function mediaUrl(value) {
  const url = new URL(value);
  if (url.protocol === 'https:' && !url.username && !url.password && !url.port &&
      /\.(bilivideo\.com|bilivideo\.cn)$/.test(url.hostname)) return url;
  return null;
}

createServer(async (request, response) => {
  const path = new URL(request.url, 'http://127.0.0.1');
  if (request.method !== 'GET' || !path.pathname.startsWith('/bilibili/') ||
      !['127.0.0.1:38690', 'localhost:38690'].includes(request.headers.host)) {
    response.writeHead(404).end();
    return;
  }
  const name = path.pathname.slice('/bilibili/'.length);
  response.setHeader('Cache-Control', 'no-store');
  response.setHeader('X-Content-Type-Options', 'nosniff');
  response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  response.setHeader('Cross-Origin-Embedder-Policy', 'credentialless');
  response.setHeader('Content-Security-Policy', "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' https://*.hdslb.com; object-src 'none'; base-uri 'none'");
  try {
    if (files.has(name)) {
      const [file, mime] = files.get(name);
      response.setHeader('Content-Type', mime + '; charset=utf-8');
      response.end(await readFile(new URL(file, panel)));
      return;
    }
    const kind = name.startsWith('api/') && name.slice(4);
    const endpoint = endpoints.get(kind);
    const media = kind === 'media.mp4';
    const target = media ? mediaUrl(path.searchParams.get('url')) :
      endpoint && path.search.length <= 2048 && new URL(endpoint + path.search.slice(1), 'https://api.bilibili.com');
    if (!target) { response.writeHead(404).end(); return; }
    const abort = new AbortController();
    response.on('close', () => abort.abort());
    const headers = {'User-Agent': 'Mozilla/5.0', Referer: 'https://www.bilibili.com/'};
    if (media && /^bytes=\d+-\d*$/.test(request.headers.range || '')) headers.Range = request.headers.range;
    const signal = AbortSignal.any([abort.signal, AbortSignal.timeout(media ? 180000 : 10000)]);
    const upstream = await fetch(target, {headers, signal, redirect: 'error'});
    if (!upstream.ok) {
      response.writeHead(502).end();
      await upstream.body?.cancel();
      return;
    }
    response.setHeader('Content-Type', media ? 'video/mp4' : kind === 'dm' ? 'text/xml; charset=utf-8' : 'application/json; charset=utf-8');
    if (media) {
      for (const key of ['content-range', 'content-length', 'accept-ranges']) {
        const value = upstream.headers.get(key);
        if (value !== null) response.setHeader(key, value);
      }
    }
    response.writeHead(upstream.status);
    await pipeline(Readable.fromWeb(upstream.body), response);
  } catch {
    if (response.headersSent) response.destroy();
    else response.writeHead(502).end();
  }
}).listen(38690, '127.0.0.1', () => console.log('http://127.0.0.1:38690/bilibili/'));


