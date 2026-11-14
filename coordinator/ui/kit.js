/* Shared UI kit: app shell + hash router, SVG charts, tables, modal, toast, live/demo API detection. */
const Kit = (() => {
  const escapeHtml = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const seededRandom = seed => () => (seed = (seed * 1664525 + 1013904223) % 4294967296) / 4294967296;
  const formatNumber = n => n >= 1e9 ? (n / 1e9).toFixed(1) + 'B' : n >= 1e6 ? (n / 1e6).toFixed(1) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(1) + 'k' : String(Math.round(n * 10) / 10);
  const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB'];
  const formatBytes = bytes => { let i = 0; while (bytes >= 1024 && i < 4) { bytes /= 1024; i++; } return bytes.toFixed(i ? 1 : 0) + ' ' + BYTE_UNITS[i]; };
  const formatDuration = ms => ms < 1000 ? Math.round(ms) + ' ms' : ms < 60000 ? (ms / 1000).toFixed(1) + ' s' : Math.floor(ms / 60000) + 'm ' + Math.round(ms % 60000 / 1000) + 's';
  const RELATIVE_STEPS = [[86400, 'd'], [3600, 'h'], [60, 'm']];
  const formatRelative = time => { const s = (Date.now() - new Date(time)) / 1000; if (isNaN(s)) return '—'; const step = RELATIVE_STEPS.find(([size]) => s >= size); return step ? Math.floor(s / step[0]) + step[1] + ' ago' : 'just now'; };
  const pick = (random, list) => list[Math.floor(random() * list.length)];
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

  const STATUS_TONES = {ok: 'ok', healthy: 'ok', up: 'ok', succeeded: 'ok', success: 'ok', active: 'ok', completed: 'ok', running: 'info', info: 'info', pending: 'warn', warn: 'warn', warning: 'warn', degraded: 'warn', queued: 'warn',
    failed: 'bad', error: 'bad', down: 'bad', critical: 'bad', firing: 'bad', dead: 'bad', cancelled: '', debug: '', paused: ''};
  const badge = (text, tone) => `<span class="badge ${tone ?? STATUS_TONES[String(text).toLowerCase()] ?? ''}">${escapeHtml(text)}</span>`;
  const statCard = ({label, value, delta, deltaGood = true, trend, color}) => `<div class="panel stat"><div class="k">${escapeHtml(label)}</div><div class="v">${value}</div>
    ${delta == null ? '' : `<div class="d ${(delta >= 0) === deltaGood ? 'up' : 'down'}">${delta >= 0 ? '▲' : '▼'} ${Math.abs(delta)}%<span class="hint"> vs prev</span></div>`}${trend ? sparkline(trend, color) : ''}</div>`;
  const panel = (title, body, {actions = '', flush = false} = {}) => `<section class="panel"><div class="head"><h3>${title}</h3><div class="grow"></div>${actions}</div><div class="body ${flush ? 'flush' : ''}">${body}</div></section>`;
  const emptyState = message => `<div class="empty">${escapeHtml(message)}</div>`;
  const table = ({columns, rows, rowKey}) => `<div class="scroll"><table class="tbl"><thead><tr>${columns.map(c => `<th>${escapeHtml(c.title)}</th>`).join('')}</tr></thead><tbody>${
    rows.length ? rows.map(r => `<tr class="${rowKey ? 'click' : ''}" ${rowKey ? `data-key="${escapeHtml(rowKey(r))}"` : ''}>${columns.map(c => `<td>${c.cell(r)}</td>`).join('')}</tr>`).join('') : `<tr><td colspan="${columns.length}">${emptyState('Nothing here yet.')}</td></tr>`}</tbody></table></div>`;
  const keyValueList = pairs => `<dl class="kv">${pairs.map(([k, v]) => `<dt>${escapeHtml(k)}</dt><dd>${v}</dd>`).join('')}</dl>`;
  const barList = (rows, {format = formatNumber} = {}) => { const max = Math.max(1, ...rows.map(r => r[1])); return rows.map(([k, v]) => `<div class="bar-row"><span class="mono">${escapeHtml(k)}</span><span class="track"><i style="width:${Math.max(3, v / max * 100)}%"></i></span><b>${format(v)}</b></div>`).join(''); };
  const toggle = (name, checked) => `<label class="switch"><input type="checkbox" name="${name}" ${checked ? 'checked' : ''}><i></i></label>`;

  /* ---------- charts ---------- */
  const PALETTE = ['var(--accent)', '#22c1a8', '#f59e0b', '#ef5da8', '#3b82f6', '#8b5cf6'];
  const sparkline = (values, color = 'var(--accent)') => { const {path, area} = linePaths(values, 200, 34, 3); return `<svg class="chart" viewBox="0 0 200 34" preserveAspectRatio="none" height="34"><path d="${area}" fill="${color}" opacity=".12"/><path d="${path}" fill="none" stroke="${color}" stroke-width="1.8"/></svg>`; };
  function linePaths(values, width, height, pad, maxValue) {
    const max = maxValue ?? (Math.max(...values, 1) * 1.1), step = (width - pad * 2) / Math.max(values.length - 1, 1);
    const points = values.map((v, i) => [pad + i * step, height - pad - v / max * (height - pad * 2)]);
    const path = points.map(([x, y], i) => (i ? 'L' : 'M') + x.toFixed(1) + ' ' + y.toFixed(1)).join(' ');
    return {path, area: path + ` L${points.at(-1)[0].toFixed(1)} ${height} L${pad} ${height} Z`};
  }
  const lineChart = ({series, labels = [], height = 200, unit = '', stacked = false}) => {
    const width = 640, pad = 28, max = Math.max(...series.flatMap(s => s.values), 1) * 1.1, id = 'c' + Math.random().toString(36).slice(2, 7);
    const grid = [0, .25, .5, .75, 1].map(f => { const y = height - 18 - f * (height - 30); return `<line class="grid-line" x1="${pad}" x2="${width - pad}" y1="${y}" y2="${y}"/><text x="0" y="${y + 3}">${formatNumber(max * f)}</text>`; }).join('');
    const drawn = series.map((s, i) => { const {path, area} = linePaths(s.values, width, height - 14, pad, max), color = s.color || PALETTE[i % PALETTE.length];
      return `<defs><linearGradient id="${id}${i}" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="${color}" stop-opacity=".28"/><stop offset="1" stop-color="${color}" stop-opacity="0"/></linearGradient></defs><path d="${area}" fill="url(#${id}${i})"/><path d="${path}" fill="none" stroke="${color}" stroke-width="2" stroke-linejoin="round"/>`; }).join('');
    const xLabels = labels.length ? [0, .25, .5, .75, 1].map(f => `<text x="${pad + f * (width - pad * 2)}" y="${height - 2}" text-anchor="middle">${escapeHtml(labels[Math.round(f * (labels.length - 1))])}</text>`).join('') : '';
    const data = escapeHtml(JSON.stringify({series: series.map((s, i) => ({name: s.name, color: s.color || PALETTE[i % PALETTE.length], values: s.values})), labels, unit, pad, width}));
    return `<div class="chart-wrap" data-chart="${data}"><svg class="chart" viewBox="0 0 ${width} ${height}" height="${height}" preserveAspectRatio="none">${grid}${drawn}</svg><div class="tip"></div></div>
      <div class="legend">${series.map((s, i) => `<span><i style="background:${s.color || PALETTE[i % PALETTE.length]}"></i>${escapeHtml(s.name)}</span>`).join('')}</div>`;
  };
  const barChart = ({values, labels = [], height = 160, color = 'var(--accent)'}) => { const max = Math.max(...values, 1), w = 640, gap = 3, bw = (w - 20) / values.length - gap;
    return `<svg class="chart" viewBox="0 0 ${w} ${height}" height="${height}" preserveAspectRatio="none">${values.map((v, i) => { const h = v / max * (height - 22); return `<rect x="${10 + i * (bw + gap)}" y="${height - 16 - h}" width="${bw}" height="${h}" rx="2" fill="${color}" opacity=".85"><title>${escapeHtml(labels[i] ?? i)}: ${formatNumber(v)}</title></rect>`; }).join('')}</svg>`; };
  const donutChart = parts => { const total = parts.reduce((a, p) => a + p.value, 0) || 1; let offset = 0;
    const rings = parts.map((p, i) => { const frac = p.value / total * 100, el = `<circle r="15.9" cx="21" cy="21" fill="none" stroke="${p.color || PALETTE[i % PALETTE.length]}" stroke-width="5" stroke-dasharray="${frac} ${100 - frac}" stroke-dashoffset="${25 - offset}"/>`; offset += frac; return el; }).join('');
    return `<div style="display:flex;gap:18px;align-items:center"><svg viewBox="0 0 42 42" width="120" height="120"><circle r="15.9" cx="21" cy="21" fill="none" stroke="var(--panel-2)" stroke-width="5"/>${rings}</svg>
      <div>${parts.map((p, i) => `<div><i class="dot" style="background:${p.color || PALETTE[i % PALETTE.length]}"></i>${escapeHtml(p.label)} <b>${formatNumber(p.value)}</b></div>`).join('')}</div></div>`; };
  function enableChartTooltips(root) {
    $$('[data-chart]', root).forEach(wrap => {
      const info = JSON.parse(wrap.dataset.chart), tip = $('.tip', wrap), svg = $('svg', wrap);
      wrap.onmousemove = event => { const rect = svg.getBoundingClientRect(), count = info.series[0].values.length;
        const index = Math.max(0, Math.min(count - 1, Math.round(((event.clientX - rect.left) / rect.width * info.width - info.pad) / ((info.width - info.pad * 2) / (count - 1)))));
        tip.style.display = 'block'; tip.style.left = Math.min(event.clientX - rect.left + 12, rect.width - 150) + 'px'; tip.style.top = '6px';
        tip.innerHTML = `<b>${escapeHtml(info.labels[index] ?? '#' + index)}</b>` + info.series.map(s => `<div><i class="dot" style="background:${s.color}"></i>${escapeHtml(s.name)}: ${formatNumber(s.values[index])}${info.unit}</div>`).join(''); };
      wrap.onmouseleave = () => tip.style.display = 'none';
    });
  }

  /* ---------- overlays ---------- */
  function toast(message, isError) { const el = document.createElement('div'); el.className = 'toast' + (isError ? ' err' : ''); el.textContent = message; $('#toasts').append(el); setTimeout(() => el.remove(), 3600); }
  function openModal({title, body, confirmLabel = 'Save', onConfirm}) {
    const dialog = $('#dialog'); dialog.innerHTML = `<form method="dialog"><h3>${title}</h3>${body}<div class="actions"><button type="button" class="btn" data-close>Cancel</button>${onConfirm ? `<button class="btn primary">${confirmLabel}</button>` : ''}</div></form>`;
    dialog.showModal(); $('[data-close]', dialog).onclick = () => dialog.close();
    $('form', dialog).onsubmit = async event => { event.preventDefault(); try { await onConfirm(Object.fromEntries(new FormData(event.target))); dialog.close(); } catch (error) { toast(error.message, true); } };
  }

  /* ---------- API with live/demo detection ---------- */
  function createApiClient({defaultBase = '', healthPath, storageKey, credentialLabel, authHeaders = token => ({Authorization: 'Bearer ' + token})}) {
    const client = {live: false, base: localStorage.getItem(storageKey) ?? defaultBase, credentialLabel, token: sessionStorage.getItem(storageKey + '.token') || ''};
    const setToken = token => { client.token = token; sessionStorage.setItem(storageKey + '.token', token); };
    client.setCredential = () => { const token = prompt(credentialLabel); client.declined = token === null; if (token === null) return false; setToken(token.trim()); return true; };
    client.request = async (path, {method = 'GET', body, raw = false, headers = {}} = {}, retried = false) => {
      if (client.token) Object.assign(headers, authHeaders(client.token));
      if (body !== undefined && typeof body !== 'string') { body = JSON.stringify(body); headers['Content-Type'] = 'application/json'; }
      const response = await fetch(client.base + path, {method, body, headers}), text = await response.text();
      if (response.status === 401 && credentialLabel && !retried && !client.declined && client.setCredential()) return client.request(path, {method, body, raw, headers}, true);
      if (!response.ok) throw Object.assign(new Error(text.trim().slice(0, 240) || `${response.status} ${response.statusText}`), {status: response.status});
      if (raw) return text;
      try { return JSON.parse(text); } catch { return text; }
    };
    client.detect = async () => { try { const c = new AbortController(); setTimeout(() => c.abort(), 2500); await fetch(client.base + healthPath, {signal: c.signal}); client.live = true; } catch { client.live = false; } return client.live; };
    client.configure = base => { client.base = base.replace(/\/+$/, ''); localStorage.setItem(storageKey, client.base); };
    return client;
  }

  /* ---------- app shell ---------- */
  async function startApplication({name, glyph, hue, mode, pages, api, entryPages, tagline, requireLive = false}) {
    document.documentElement.style.setProperty('--hue', hue);
    const savedTheme = localStorage.getItem('ui.theme'); if (savedTheme) document.documentElement.dataset.theme = savedTheme;
    document.title = `${name} ${mode === 'admin' ? 'Admin' : 'Console'}`;
    const visiblePages = pages.filter(p => (p.mode || 'core') === mode), groups = [...new Set(visiblePages.map(p => p.group))];
    document.body.innerHTML = `<div class="shell"><aside class="sidebar" id="sidebar"><div class="brand"><span class="glyph">${glyph}</span>${escapeHtml(name)}</div>
      ${entryPages?.core && entryPages?.admin ? `<div class="switcher"><a href="${entryPages.core}" class="${mode === 'core' ? 'on' : ''}">Console</a><a href="${entryPages.admin}" class="${mode === 'admin' ? 'on' : ''}">Admin</a></div>` : ''}
      <nav class="nav">${groups.map(g => `<div class="group">${escapeHtml(g)}</div>` + visiblePages.filter(p => p.group === g && !p.hidden).map(p => `<a data-page="${p.id}"><span>${p.icon}</span>${escapeHtml(p.title)}</a>`).join('')).join('')}</nav>
      <div class="hint" style="padding:0 8px">${escapeHtml(tagline || '')}</div></aside>
      <div class="main"><header class="topbar"><button class="btn sm burger" id="burger">☰</button><h1 id="title"></h1><div class="grow"></div>
      <button class="pill" id="connection"><i></i><span></span></button>${api.credentialLabel ? '<button class="btn sm" id="credential" title="Change credential">Token</button>' : ''}<button class="btn sm" id="themeToggle" title="Theme">◐</button></header><main class="page" id="page"></main></div></div><div id="toasts"></div><dialog id="dialog"></dialog>`;
    const page = $('#page'); let activeTimer = null;
    const paintConnection = () => { const pill = $('#connection'); pill.className = 'pill ' + (api.live ? 'live' : requireLive ? 'down' : 'demo'); $('span', pill).textContent = api.live ? 'Connected' + (api.base ? ' · ' + api.base : '') : requireLive ? 'Disconnected · click to configure' : 'Demo data · click to connect'; };
    $('#connection').onclick = async () => { const base = prompt(`${name} API base URL (blank = this origin)`, api.base); if (base !== null) { api.configure(base); await api.detect(); paintConnection(); route(); } };
    $('#themeToggle').onclick = () => { const dark = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim() === '#0b0d17'; const next = dark ? 'light' : 'dark'; document.documentElement.dataset.theme = next; localStorage.setItem('ui.theme', next); route(); };
    if (api.credentialLabel) $('#credential').onclick = () => { if (api.setCredential()) route(); };
    $('#burger').onclick = () => $('#sidebar').classList.toggle('open');
    $('.nav').onclick = event => { const link = event.target.closest('a'); if (link) location.hash = link.dataset.page; };
    async function route() {
      clearInterval(activeTimer); const id = location.hash.slice(1).split('/')[0], current = visiblePages.find(p => p.id === id) || visiblePages[0], argument = location.hash.slice(1).split('/')[1];
      $$('.nav a').forEach(a => a.classList.toggle('active', a.dataset.page === current.id)); $('#title').textContent = current.title; $('#sidebar').classList.remove('open');
      const context = {api, page, argument, toast, openModal, refresh: route};
      if (requireLive && !api.live) { page.innerHTML = `<div class="panel"><div class="body">${emptyState('Cannot reach the ' + name + ' API at ' + (api.base || location.origin) + '.')}<div style="text-align:center"><button class="btn primary" id="retry">Retry</button></div></div></div>`; $('#retry').onclick = async () => { await api.detect(); paintConnection(); route(); }; return; }
      try { page.innerHTML = await current.render(context); enableChartTooltips(page); await current.mount?.(context); if (current.pollMs) activeTimer = setInterval(() => current.poll?.(context), current.pollMs); }
      catch (error) { page.innerHTML = `<div class="panel"><div class="body">${emptyState('Could not load: ' + error.message)}<div style="text-align:center"><button class="btn" id="retry">Retry</button></div></div></div>`; $('#retry').onclick = route; }
    }
    window.onhashchange = route; await api.detect(); paintConnection(); route();
  }
  return {escapeHtml, seededRandom, formatNumber, formatBytes, formatDuration, formatRelative, pick, $, $$, badge, statCard, panel, table, keyValueList, barList, toggle, emptyState,
    sparkline, lineChart, barChart, donutChart, enableChartTooltips, toast, openModal, createApiClient, startApplication, PALETTE};
})();
