#pragma once
// agent 本地设备详情页：单文件、无外部依赖（不引 CDN / 字体 / 图标），
// 直接内嵌进二进制，适合离线设备。数据来自 GET /api/v1/sysinfo。

static const char kAgentWebHtml[] = R"AGENTWEB(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>设备详情 · rtunnel</title>
<style>
:root{
  --bg:#f4f6fa;--panel:#fff;--text:#1f2633;--muted:#7b8494;--line:#e4e8f0;
  --brand:#3b6ef5;--ok:#2e9e63;--warn:#c98a18;--bad:#d9534f;
  --bar-bg:#eef1f7;--shadow:0 1px 2px rgba(16,24,40,.04),0 4px 14px rgba(16,24,40,.05);
}
@media (prefers-color-scheme:dark){
  :root{--bg:#0f131a;--panel:#171c26;--text:#e6ebf5;--muted:#8f99ab;--line:#252c3a;
        --bar-bg:#222938;--shadow:none}
}
*{box-sizing:border-box}
[hidden]{display:none!important}
html,body{margin:0}
body{background:var(--bg);color:var(--text);font:14px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
.page{max-width:1180px;margin:0 auto;padding:16px 18px 40px}
.sticky{position:sticky;top:0;z-index:20;background:var(--bg);padding:2px 0 10px;margin-top:-2px}
.head{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px 16px;box-shadow:var(--shadow)}
.head-top{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
h1{font-size:18px;line-height:1.25;margin:0;font-weight:650}
.head .tag{height:24px}
.head .ver{font-variant-numeric:tabular-nums}
.actions{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-left:auto}
.actions label{display:flex;gap:6px;align-items:center;color:var(--muted);font-size:13px;cursor:pointer;user-select:none}
select,.btn{height:32px;border:1px solid var(--line);background:var(--panel);color:var(--text);border-radius:8px;padding:0 10px;font-size:13px;cursor:pointer;font-family:inherit}
select{padding-right:6px}
.btn:hover{border-color:var(--brand);color:var(--brand)}
.btn.ghost{background:transparent}
.head-meta{display:flex;flex-wrap:wrap;align-items:center;gap:3px 14px;margin-top:8px;font-size:12px;color:var(--muted)}
.head-meta span{white-space:nowrap}
.tabs{display:flex;gap:2px;margin-top:8px;padding:4px;background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:var(--shadow);overflow-x:auto;scrollbar-width:none}
.tabs::-webkit-scrollbar{display:none}
.tabs button{flex:none;border:0;background:transparent;padding:7px 15px;border-radius:7px;font:inherit;font-size:13px;color:var(--muted);cursor:pointer;white-space:nowrap}
.tabs button:hover{background:var(--bar-bg);color:var(--text)}
.tabs button.on{background:rgba(59,110,245,.1);color:var(--brand);font-weight:600}
.tag{display:inline-flex;align-items:center;gap:5px;height:26px;padding:0 9px;border-radius:999px;font-size:12px;white-space:nowrap;background:var(--bar-bg);color:var(--muted)}
.tag::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}
.tag.ok{background:rgba(46,158,99,.12);color:var(--ok)}
.tag.warn{background:rgba(201,138,24,.14);color:var(--warn)}
.tag.bad{background:rgba(217,83,79,.13);color:var(--bad)}
.kpis{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-bottom:12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:13px 15px;box-shadow:var(--shadow);min-width:0}
.card .ck{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:12px}
.card .ck .cs{margin-left:auto;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.card .cv{font-size:28px;line-height:1.15;font-weight:650;letter-spacing:-.02em;margin-top:5px;font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.card .cv small{font-size:14px;font-weight:500;color:var(--muted);margin-left:2px}
.card .cv.sm{font-size:16px;padding:9px 0 6px}
.card .cs2{color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.bar{height:6px;border-radius:999px;background:var(--bar-bg);overflow:hidden;margin-top:9px}
.bar i{display:block;height:100%;border-radius:999px;background:var(--brand);transition:width .35s}
.bar i.warn{background:var(--warn)}.bar i.bad{background:var(--bad)}
.grid-main{display:grid;grid-template-columns:minmax(0,1.7fr) minmax(0,1fr);gap:12px;margin-bottom:12px;align-items:start}
.grid2{display:grid;grid-template-columns:minmax(0,1.4fr) minmax(0,1fr);gap:12px;align-items:start}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 16px;box-shadow:var(--shadow);min-width:0}
.panel-head{display:flex;align-items:center;gap:10px;min-height:24px;margin-bottom:8px}
h2{font-size:14px;margin:0;font-weight:650}
.hint{color:var(--muted);font-size:12px;margin-left:auto;white-space:nowrap}
.link{border:0;background:transparent;color:var(--brand);font:inherit;font-size:12px;cursor:pointer;padding:0 0 0 10px;white-space:nowrap}
#chart{display:block;width:100%;height:150px}
.legend{display:flex;gap:14px;align-items:center;color:var(--muted);font-size:12px;margin-top:2px;padding-left:34px}
.legend i{width:16px;height:3px;border-radius:2px;display:inline-block;margin-right:5px}
.legend .c1{background:#3b6ef5}.legend .c2{background:#e08a2e}
.kv{display:grid;grid-template-columns:68px minmax(0,1fr);gap:5px 10px;font-size:12px;line-height:18px}
.kv .k{color:var(--muted)}
.kv .v{min-width:0;overflow-wrap:anywhere}
.cert{margin-top:10px;padding-top:9px;border-top:1px solid var(--line);font-size:12px;overflow-wrap:anywhere}
.cert .ch{color:var(--muted);margin-bottom:3px}
.disk{padding:8px 0;border-bottom:1px solid var(--line)}
.disk:last-child{border-bottom:none}
.disk-top{display:flex;gap:10px;align-items:baseline;margin-bottom:5px;font-size:12px}
.disk-top b{font-family:ui-monospace,Menlo,monospace;font-size:13px}
.disk-top .muted{font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.disk-top .pct{margin-left:auto;font-variant-numeric:tabular-nums;font-size:12px;color:var(--muted)}
.disk-top .pct.warn{color:var(--warn)}.disk-top .pct.bad{color:var(--bad)}
.disk-sub{color:var(--muted);font-size:12px;margin-top:4px}
.nic{display:grid;grid-template-columns:auto auto minmax(0,1fr);gap:4px 8px;align-items:center;padding:8px 0;border-bottom:1px solid var(--line);font-size:12px}
.nic:last-child{border-bottom:none}
.nic .nm{font-family:ui-monospace,Menlo,monospace;font-size:13px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:130px}
.nic .ip{text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--muted)}
.nic .io{grid-column:1/-1;color:var(--muted);font-variant-numeric:tabular-nums}
.emptyp{color:var(--muted);font-size:12px;padding:14px 0}
.table-wrap{overflow-x:auto}
table{width:100%;border-collapse:collapse}
th,td{padding:7px 8px;text-align:left;border-bottom:1px solid var(--line);font-size:13px;vertical-align:middle;white-space:nowrap}
th{color:var(--muted);font-weight:500;font-size:12px}
tr:last-child td{border-bottom:none}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}
.muted{color:var(--muted)}
.fsmeta{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.fsmeta .root{font-family:ui-monospace,Menlo,monospace;font-size:13px;overflow-wrap:anywhere}
.fsmeta .tag{margin-left:auto}
.fsnote{color:var(--muted);font-size:12px;margin-top:4px}
.host{margin-top:10px}
.chips{display:flex;gap:7px;flex-wrap:wrap}
.chip{background:var(--bar-bg);border-radius:7px;padding:3px 9px;font-size:12px;color:var(--text)}
.args{margin-top:11px;padding-top:10px;border-top:1px dashed var(--line);color:var(--muted);font-size:12px;overflow-wrap:anywhere}
.args code{font-family:ui-monospace,Menlo,monospace;color:var(--text)}
footer{color:var(--muted);font-size:12px;text-align:center;padding:6px 0 0}
@media (max-width:900px){
  .kpis{grid-template-columns:repeat(2,minmax(0,1fr))}
  .grid-main,.grid2{grid-template-columns:1fr}
}
@media (max-width:640px){
  .page{padding:10px 12px 30px}
  .head{padding:10px 12px}
  h1{font-size:16px}
  .actions{width:100%;margin-left:0;gap:6px}
  .actions label{font-size:12px}
  .head-meta{font-size:11px;gap:2px 10px;margin-top:6px}
  .head-meta .hide-sm{display:none}
  .tabs{padding:3px;mask-image:linear-gradient(to right,#000 calc(100% - 20px),transparent)}
  .tabs button{padding:7px 11px;font-size:13px}
  .kpis{gap:8px}
  .card{padding:11px 12px}
  .card .cv{font-size:23px}
  .card .cv.sm{font-size:14px;padding:7px 0 5px}
  #chart{height:130px}
  .export{display:none}
}
</style>
</head>
<body>
<div class="page">
  <div class="sticky">
    <header class="head">
      <div class="head-top">
        <h1 id="devid">设备详情</h1>
        <span class="tag" id="state">加载中</span>
        <span class="tag ver" id="ver" hidden></span>
        <div class="actions">
          <label><input type="checkbox" id="auto" checked> 自动刷新</label>
          <select id="period" title="自动刷新间隔">
            <option value="2000">2s</option>
            <option value="5000">5s</option>
            <option value="10000">10s</option>
          </select>
          <button class="btn" id="refresh">刷新</button>
          <button class="btn ghost export" id="export">导出 JSON</button>
        </div>
      </div>
      <div class="head-meta" id="meta"></div>
    </header>
    <nav class="tabs" id="tabs" role="tablist" aria-label="设备视图">
      <button type="button" role="tab" data-tab="overview" aria-selected="true" class="on">概览</button>
      <button type="button" role="tab" data-tab="storage" aria-selected="false">存储</button>
      <button type="button" role="tab" data-tab="file" aria-selected="false">文件</button>
      <button type="button" role="tab" data-tab="service" aria-selected="false">服务</button>
      <button type="button" role="tab" data-tab="tty" aria-selected="false">串口</button>
      <button type="button" role="tab" data-tab="caps" aria-selected="false">能力</button>
    </nav>
  </div>

  <main>
    <section data-page="overview">
      <div class="kpis" id="kpis"></div>
      <div class="grid-main">
        <div class="panel">
          <div class="panel-head"><h2>资源趋势</h2><span class="hint">CPU / 内存 · 最近 2 分钟</span></div>
          <canvas id="chart" height="150"></canvas>
          <div class="legend"><span><i class="c1"></i>CPU</span><span><i class="c2"></i>内存</span></div>
        </div>
        <div class="panel">
          <div class="panel-head"><h2>系统信息</h2><span class="hint" id="sysHint"></span></div>
          <div class="kv" id="sysinfo"></div>
          <div class="cert" id="cert" hidden></div>
        </div>
      </div>
      <div class="grid2">
        <div class="panel">
          <div class="panel-head"><h2>存储</h2><span class="hint" id="ovDiskHint"></span><button class="link" type="button" data-go="storage">全部 →</button></div>
          <div id="ovDisks"></div>
        </div>
        <div class="panel">
          <div class="panel-head"><h2>网卡</h2><span class="hint" id="nicHint"></span></div>
          <div id="ovNics"></div>
        </div>
      </div>
    </section>

    <section data-page="storage" hidden>
      <div class="panel">
        <div class="panel-head"><h2>存储</h2><span class="hint" id="diskHint"></span></div>
        <div id="disks"></div>
      </div>
    </section>

    <section data-page="file" hidden>
      <div class="panel">
        <div class="panel-head"><h2>文件管理</h2><span class="hint">浏览、上传、下载和编辑设备文件</span></div>
        <div class="fsmeta" id="fsMeta" hidden>
          <span class="root mono" id="fsRoot">—</span>
          <span class="tag" id="fsMode">只读</span>
        </div>
        <div class="fsnote" id="fsInfo"></div>
        <div class="emptyp" id="fsEmpty" hidden>该设备未启用文件服务（可在能力配置中开启）</div>
        <div class="host" id="fsHost"></div>
      </div>
    </section>

    <section data-page="service" hidden>
      <div class="panel">
        <div class="panel-head"><h2>服务管理</h2><span class="hint">启动、停止、自启与日志</span></div>
        <div class="fsmeta" id="svcMeta" hidden>
          <span class="root" id="svcSummary">systemd 服务管理</span>
          <span class="tag" id="svcMode">只读</span>
        </div>
        <div class="fsnote" id="svcInfo"></div>
        <div class="emptyp" id="svcEmpty" hidden>该设备未启用 systemd 服务管理</div>
        <div class="host" id="svcHost"></div>
      </div>
    </section>

    <section data-page="tty" hidden>
      <div class="panel">
        <div class="panel-head"><h2>串口</h2><span class="hint" id="ttyCount"></span></div>
        <div class="table-wrap" id="serial"></div>
      </div>
    </section>

    <section data-page="caps" hidden>
      <div class="panel">
        <div class="panel-head"><h2>运行时能力</h2><span class="hint" id="capsHint"></span></div>
        <div class="chips" id="caps"></div>
        <div class="args" id="args"></div>
      </div>
    </section>
  </main>

  <footer id="foot">本地页面 · agent 采集</footer>
</div>
<script src="assets/fs-ui.js"></script>
<script src="assets/svc-ui.js"></script>
<script src="assets/file-view.js"></script>
<script>
(function () {
  'use strict';
  var $ = function (s) { return document.querySelector(s); };
  var $$ = function (s) { return Array.prototype.slice.call(document.querySelectorAll(s)); };
  var TABS = ['overview', 'storage', 'file', 'service', 'tty', 'caps'];
  var state = { data: null, hist: [], timer: null, period: 2000, tab: 'overview' };

  // 相对路径：直接访问（/）和经 relay /device/<devid>/<port>/ 代理都能用
  function wsUrl(path) {
    var base = location.pathname.replace(/\/?$/, '/');
    return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + base + path;
  }

  function esc(v) {
    return String(v == null ? '' : v).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  function toNum(v) { var n = Number(v); return isFinite(n) ? n : 0; }
  function fmtBytes(b) {
    var n = toNum(b), u = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0;
    if (n <= 0) return '0 B';
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i === 0 ? n.toFixed(0) : n.toFixed(n >= 100 ? 0 : 1)) + ' ' + u[i];
  }
  function fmtKB(kb) { return fmtBytes(toNum(kb) * 1024); }
  function fmtBytesShort(b) { return fmtBytes(b).replace(' ', ''); }
  function fmtDur(sec) {
    var s = Math.max(0, Math.floor(toNum(sec)));
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (d) return d + ' 天 ' + h + ' 小时';
    if (h) return h + ' 小时 ' + m + ' 分';
    if (m) return m + ' 分 ' + (s % 60) + ' 秒';
    return s + ' 秒';
  }
  function memAvail(d) {
    return d.mem_available_kb != null ? toNum(d.mem_available_kb) : toNum(d.mem_free_kb);
  }
  function memPct(d) {
    var t = toNum(d.mem_total_kb);
    return t > 0 ? Math.max(0, Math.min(100, (t - memAvail(d)) * 100 / t)) : 0;
  }
  function tone(p) { return p >= 85 ? 'bad' : p >= 70 ? 'warn' : ''; }
  function caps() { return (state.data && state.data.caps) || {}; }
  function nicUp(n) { return n.operstate ? n.operstate === 'up' : !!n.up; }
  function diskPct(m) {
    var used = toNum(m.used_kb), avail = toNum(m.avail_kb), base = used + avail;
    return base ? Math.ceil(used * 100 / base) : 0;
  }

  function diskHtml(m, withType) {
    var used = toNum(m.used_kb), total = toNum(m.total_kb), p = diskPct(m), cls = tone(p);
    return '<div class="disk"><div class="disk-top"><b>' + esc(m.mount) + '</b>' +
      (withType ? '<span class="muted">' + esc(m.fstype || '') + '</span>' : '') +
      '<span class="pct ' + cls + '">' + p + '%</span></div>' +
      '<div class="bar"><i class="' + cls + '" style="width:' + p + '%"></i></div>' +
      '<div class="disk-sub">' + fmtKB(used) + ' / ' + fmtKB(total) + '</div></div>';
  }

  function sortedMounts(d) {
    return (d.mounts || []).slice().sort(function (a, b) { return diskPct(b) - diskPct(a); });
  }

  // ---------------------------------------------------------------- tab
  function tabFromHash() {
    var h = (location.hash || '').replace(/^#\/?/, '');
    return TABS.indexOf(h) >= 0 ? h : 'overview';
  }

  function setTab(name, push) {
    if (TABS.indexOf(name) < 0) name = 'overview';
    state.tab = name;
    $$('.tabs button').forEach(function (b) {
      var on = b.getAttribute('data-tab') === name;
      b.classList.toggle('on', on);
      b.setAttribute('aria-selected', on ? 'true' : 'false');
    });
    $$('[data-page]').forEach(function (el) { el.hidden = el.getAttribute('data-page') !== name; });
    if (push) {
      try { history.pushState({ tab: name }, '', '#' + name); }
      catch (e) { location.hash = name; }
    }
    if (name === 'overview') setTimeout(drawChart, 0);
    else if (name === 'file') ensureFs();
    else if (name === 'service') ensureSvc();
  }

  $$('.tabs button').forEach(function (b) {
    b.addEventListener('click', function () { setTab(b.getAttribute('data-tab'), true); });
  });
  $$('[data-go]').forEach(function (b) {
    b.addEventListener('click', function () { setTab(b.getAttribute('data-go'), true); });
  });
  window.addEventListener('popstate', function () { setTab(tabFromHash(), false); });
  window.addEventListener('hashchange', function () { setTab(tabFromHash(), false); });

  // ---------------------------------------------------------------- render
  function render(d) {
    state.data = d;
    document.title = (d.devid || '设备') + ' · rtunnel';
    $('#devid').textContent = d.devid || '设备详情';
    var ver = $('#ver');
    ver.hidden = !d.version;
    ver.textContent = d.version ? 'v' + d.version : '';

    var on = !!d.online, st = $('#state');
    st.textContent = on ? 'relay 已连接' : 'relay 未连接';
    st.className = 'tag ' + (on ? 'ok' : 'warn');

    var meta = [];
    if (d.machine || d.kernel) meta.push('<span>' + esc([d.machine, d.kernel].filter(Boolean).join(' · ')) + '</span>');
    if (d.relay) meta.push('<span class="mono">' + esc(d.relay) + '</span>');
    meta.push('<span>运行 ' + esc(fmtDur(d.uptime_sec)) + '</span>');
    if (d.ts) meta.push('<span class="hide-sm">采集 ' + esc(new Date(d.ts).toLocaleTimeString()) + '</span>');
    $('#meta').innerHTML = meta.join('');

    var mp = memPct(d), la = Array.isArray(d.loadavg) ? d.loadavg : [];
    var total = toNum(d.mem_total_kb), avail = memAvail(d);
    var nics = d.nics || [];
    var upCount = nics.filter(nicUp).length;
    var rx = 0, tx = 0;
    nics.forEach(function (n) { rx += toNum(n.rx_bytes); tx += toNum(n.tx_bytes); });

    var kpis = [
      { k: 'CPU 使用率', v: toNum(d.cpu_usage).toFixed(1) + '<small>%</small>', s: toNum(d.cpu_count) + ' 核', p: toNum(d.cpu_usage) },
      { k: '内存', v: mp.toFixed(1) + '<small>%</small>', s: fmtKB(Math.max(0, total - avail)) + ' / ' + fmtKB(total), p: mp },
      { k: '负载', v: la.length ? toNum(la[0]).toFixed(2) : '—', s: la.length ? toNum(la[1]).toFixed(2) + ' / ' + toNum(la[2]).toFixed(2) : '—', sm: true },
      { k: '网络', v: '↓ ' + fmtBytesShort(rx) + ' · ↑ ' + fmtBytesShort(tx), s: nics.length + ' / ' + upCount + ' 已连接', sm: true }
    ];
    $('#kpis').innerHTML = kpis.map(function (c) {
      return '<div class="card"><div class="ck"><span>' + esc(c.k) + '</span><span class="cs">' +
        esc(c.s) + '</span></div><div class="cv' + (c.sm ? ' sm mono' : '') + '">' + c.v + '</div>' +
        (c.p != null ? '<div class="bar"><i class="' + tone(c.p) + '" style="width:' + Math.max(0, Math.min(100, c.p)) + '%"></i></div>' : '') +
        '</div>';
    }).join('');

    var mounts = sortedMounts(d);
    $('#ovDisks').innerHTML = mounts.length
      ? mounts.slice(0, 3).map(function (m) { return diskHtml(m, false); }).join('')
      : '<div class="emptyp">无挂载点信息</div>';
    $('#ovDiskHint').textContent = mounts.length > 3 ? '共 ' + mounts.length + ' 个挂载点' : '';
    $('#disks').innerHTML = mounts.length
      ? mounts.map(function (m) { return diskHtml(m, true); }).join('')
      : '<div class="emptyp">无挂载点信息</div>';
    $('#diskHint').textContent = mounts.length ? mounts.length + ' 个挂载点' : '';

    var ovNics = nics.slice(0, 4);
    $('#ovNics').innerHTML = ovNics.length
      ? ovNics.map(function (n) {
          var p = n.up ? 'ok' : 'bad';
          return '<div class="nic"><span class="nm" title="' + esc(n.name) + '">' + esc(n.name) + '</span>' +
            '<span class="tag ' + p + '">' + esc(n.operstate || (n.up ? 'up' : 'down')) + '</span>' +
            '<span class="ip mono">' + esc(n.ip || '—') + '</span>' +
            '<span class="io">↓ ' + fmtBytes(n.rx_bytes) + ' · ↑ ' + fmtBytes(n.tx_bytes) + '</span></div>';
        }).join('')
      : '<div class="emptyp">暂无网卡信息</div>';
    $('#nicHint').textContent = nics.length
      ? (nics.length > 4 ? '共 ' + nics.length + ' 个 · ' : nics.length + ' 个 · ') + upCount + ' 个已连接'
      : '';

    var rows = [
      ['架构', d.machine], ['内核', d.kernel], ['CPU', d.cpu_model],
      ['核心数', d.cpu_count ? d.cpu_count + ' 核' : ''],
      ['板卡', d.board], ['开机', fmtDur(d.uptime_sec)],
      ['agent', d.version ? 'v' + d.version + ' · ' + fmtDur(d.agent_uptime_sec) : ''],
      ['mTLS', d.mtls ? '已启用' : '未启用'],
      ['采集', d.ts ? new Date(d.ts).toLocaleString() : '']
    ];
    $('#sysinfo').innerHTML = rows.map(function (r) {
      return '<span class="k">' + esc(r[0]) + '</span><span class="v">' + esc(r[1] || '—') + '</span>';
    }).join('');

    var cert = $('#cert');
    cert.hidden = !d.mtls;
    if (d.mtls) {
      cert.innerHTML = '<div class="ch">证书 · <span class="tag ' + (d.cert_valid ? 'ok' : 'bad') + '">' +
        (d.cert_valid ? '有效' : '已过期') + '</span></div>' +
        '<div class="mono">' + esc(d.cert_subject || '—') + '</div>' +
        '<div class="mono muted">' + esc(d.cert_issuer || '—') + '</div>';
    }

    var serial = d.serial || [];
    $('#ttyCount').textContent = serial.length ? serial.length + ' 个' : '';
    $('#serial').innerHTML = serial.length
      ? '<table><thead><tr><th>设备</th><th>参数</th><th>RX</th><th>TX</th><th>状态</th></tr></thead><tbody>' +
        serial.map(function (s) {
          var param = s.configured ? (s.baud + ' · ' + s.data_bits + s.parity + s.stop_bits) : '—';
          var tag = s.configured ? (s.enabled ? '<span class="tag ok">启用</span>' : '<span class="tag">禁用</span>')
                                 : '<span class="tag">未配置</span>';
          return '<tr><td class="mono">' + esc(s.path) + '</td><td class="mono">' + esc(param) +
                 '</td><td class="mono">' + fmtBytes(s.rx) + '</td><td class="mono">' + fmtBytes(s.tx) +
                 '</td><td>' + tag + '</td></tr>';
        }).join('') + '</tbody></table>'
      : '<div class="emptyp">未发现串口</div>';

    var c = d.caps || {}, chips = [];
    (c.ports || []).forEach(function (p) { chips.push('TCP ' + p); });
    if (c.shell) chips.push('Shell');
    if (c.sysinfo) chips.push('SYSINFO');
    if (c.ota) chips.push('OTA');
    chips.push(c.mtls ? 'mTLS' : 'mTLS 未启用');
    if (c.fs) chips.push('文件管理' + (c.fs_rw ? '（读写）' : '（只读）'));
    if (c.svc) chips.push('服务管理' + (c.svc_rw ? '（可控）' : '（只读）'));
    if (c.allow_tty && c.allow_tty.length) chips.push('串口白名单 ' + c.allow_tty.length);
    $('#caps').innerHTML = chips.map(function (x) { return '<span class="chip">' + esc(x) + '</span>'; }).join('');
    $('#capsHint').textContent = c.config_loaded ? '配置来自文件' : '';
    $('#args').innerHTML = (c.config_file ? '<div><span class="muted">config</span> <code>' + esc(c.config_file) + '</code></div>' : '') +
      (c.file_dir ? '<div><span class="muted">file_dir</span> <code>' + esc(c.file_dir) + '</code></div>' : '') +
      (d.args ? '<div><span class="muted">启动参数</span> <code>' + esc(d.args) + '</code></div>' : '');

    updateFsMeta(c);
    updateSvcMeta(c);

    if (state.tab === 'file') ensureFs();
    else if (state.tab === 'service') ensureSvc();
    drawChart();
  }

  // ---------------------------------------------------------------- 图表
  function pushHist(d) {
    state.hist.push({ cpu: Math.max(0, Math.min(100, toNum(d.cpu_usage))), mem: memPct(d) });
    if (state.hist.length > 60) state.hist.shift();
  }

  function drawChart() {
    if (state.tab !== 'overview') return;
    var cv = $('#chart');
    if (!cv) return;
    var dpr = window.devicePixelRatio || 1;
    var w = cv.clientWidth || 600, h = cv.clientHeight || 150;
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
    var ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    var cs = getComputedStyle(document.documentElement);
    var line = cs.getPropertyValue('--line').trim() || '#e4e8f0';
    var muted = cs.getPropertyValue('--muted').trim() || '#7b8494';
    var pad = { l: 34, r: 8, t: 8, b: 16 };
    var cw = Math.max(1, w - pad.l - pad.r), ch = Math.max(1, h - pad.t - pad.b);

    ctx.font = '11px -apple-system,PingFang SC,sans-serif';
    ctx.textBaseline = 'middle';
    ctx.lineWidth = 1;
    [0, 50, 100].forEach(function (v) {
      var y = pad.t + ch * (1 - v / 100);
      ctx.strokeStyle = line;
      ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(pad.l + cw, y); ctx.stroke();
      ctx.fillStyle = muted;
      ctx.fillText(v + '%', 4, y);
    });

    function series(key, color) {
      var pts = state.hist;
      if (!pts.length) return;
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.lineJoin = 'round';
      ctx.beginPath();
      for (var i = 0; i < pts.length; i++) {
        var x = pad.l + (pts.length === 1 ? cw : cw * i / (pts.length - 1));
        var y = pad.t + ch * (1 - Math.max(0, Math.min(100, pts[i][key])) / 100);
        if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      }
      ctx.stroke();
    }
    series('cpu', '#3b6ef5');
    series('mem', '#e08a2e');
  }

  // ---------------------------------------------------------------- 文件 / 服务
  var fm = null, sm = null;
  var fsBaseInfo = '', fsSpace = null;

  function updateFsMeta(c) {
    var on = !!c.fs;
    $('#fsMeta').hidden = !on;
    $('#fsInfo').hidden = !on;
    $('#fsHost').hidden = !on;
    $('#fsEmpty').hidden = on;
    if (!on) {
      if (fm) { fm.destroy(); fm = null; }
      return;
    }
    $('#fsRoot').textContent = c.fs_root || '/';
    $('#fsMode').textContent = c.fs_rw ? '读写' : '只读';
    $('#fsMode').className = 'tag' + (c.fs_rw ? ' ok' : '');
    fsBaseInfo = 'WebSocket 文件通道' + (c.fs_token ? ' · 需要 Token' : '') +
      ' · 单文件上限 ' + fmtKB((c.fs_max_mb || 512) * 1024);
    renderFsInfo();
  }

  function renderFsInfo() {
    var txt = fsBaseInfo;
    if (fsSpace && fsSpace.total_kb) {
      txt += ' · 已用 ' + fmtKB(Math.max(0, fsSpace.total_kb - fsSpace.free_kb)) + ' / ' + fmtKB(fsSpace.total_kb);
    }
    $('#fsInfo').textContent = txt;
  }

  function ensureFs() {
    if (!state.data) return;
    var c = caps();
    if (!c.fs) return;
    if (!window.RtunnelFs) {
      $('#fsEmpty').hidden = false;
      $('#fsEmpty').textContent = '文件管理组件加载失败，请刷新页面';
      $('#fsHost').hidden = true;
      return;
    }
    if (!fm) {
      fm = window.RtunnelFs.createManager({
        inline: true,
        container: $('#fsHost'),
        wsUrl: wsUrl('ws/fs'),
        editorUrl: 'assets/cm-editor.js',
        title: '文件管理 - ' + ((state.data && state.data.devid) || '设备'),
        onSpace: function (s) { fsSpace = s; renderFsInfo(); }
      });
    }
    if (!fm.visible) fm.open();
  }

  function updateSvcMeta(c) {
    var on = !!c.svc;
    $('#svcMeta').hidden = !on;
    $('#svcInfo').hidden = !on;
    $('#svcHost').hidden = !on;
    $('#svcEmpty').hidden = on;
    if (!on) {
      if (sm) { sm.destroy(); sm = null; }
      return;
    }
    $('#svcSummary').textContent = 'systemd';
    $('#svcMode').textContent = c.svc_rw ? '可控制' : '只读';
    $('#svcMode').className = 'tag' + (c.svc_rw ? ' ok' : '');
    $('#svcInfo').textContent = c.svc_token ? '服务管理需要 Token' : '';
    $('#svcInfo').hidden = !c.svc_token;
  }

  function ensureSvc() {
    if (!state.data) return;
    var c = caps();
    if (!c.svc) return;
    if (!window.RtunnelSvc) {
      $('#svcEmpty').hidden = false;
      $('#svcEmpty').textContent = '服务管理组件加载失败，请刷新页面';
      $('#svcHost').hidden = true;
      return;
    }
    if (!sm) {
      sm = window.RtunnelSvc.createManager({
        inline: true,
        container: $('#svcHost'),
        wsUrl: wsUrl('ws/svc'),
        title: '服务管理 - ' + ((state.data && state.data.devid) || '设备'),
        onClose: function () {}
      });
    }
    if (!sm.visible) sm.open();
  }

  // ---------------------------------------------------------------- 轮询
  function schedule() {
    if (state.timer) clearInterval(state.timer);
    state.timer = null;
    if ($('#auto').checked) state.timer = setInterval(load, state.period);
  }

  function load() {
    fetch('api/v1/sysinfo', { cache: 'no-store' }).then(function (r) {
      return r.json().then(function (d) {
        if (!r.ok || !d.ok) throw new Error(d.error || ('HTTP ' + r.status));
        pushHist(d);
        render(d);
        $('#foot').textContent = '本地页面 · 采集于 ' + new Date(d.ts).toLocaleTimeString() +
          (d.web && d.web.listen ? ' · 监听 ' + d.web.listen : '');
      });
    }).catch(function (e) {
      var st = $('#state');
      st.textContent = '页面离线';
      st.className = 'tag bad';
      $('#foot').textContent = '数据读取失败：' + e.message;
    });
  }

  $('#auto').addEventListener('change', function () {
    schedule();
    if ($('#auto').checked) load();
  });
  $('#period').addEventListener('change', function (e) {
    state.period = Number(e.target.value) || 2000;
    schedule();
  });
  $('#refresh').addEventListener('click', load);
  $('#export').addEventListener('click', function () {
    if (!state.data) return;
    var blob = new Blob([JSON.stringify(state.data, null, 2)], { type: 'application/json' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = (state.data.devid || 'device') + '-sysinfo.json';
    a.click();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 1000);
  });
  document.addEventListener('visibilitychange', function () { if (!document.hidden) load(); });
  window.addEventListener('resize', function () { drawChart(); });

  setTab(tabFromHash(), false);
  schedule();
  load();
})();
</script>
</body>
</html>
)AGENTWEB";
