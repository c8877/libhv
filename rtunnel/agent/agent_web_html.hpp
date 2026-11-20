#pragma once
// agent 本地设备详情页：单文件、无外部依赖（不引 CDN / 字体 / 图标），
// 直接内嵌进二进制，适合离线设备。数据来自 GET /api/v1/sysinfo，
// 进程/网络/采样按 tab 按需请求 /api/v1/procs、/api/v1/net、/api/v1/metrics。

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
.grid2{display:grid;grid-template-columns:minmax(0,1.4fr) minmax(0,1fr);gap:12px;align-items:stretch}
.nic-card{display:flex;flex-direction:column}
.nic-card #ovNics{flex:1}
.nic-card .more{margin-top:10px;align-self:flex-start;padding:0;font-size:12px}
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
.nic .meta{grid-column:1/-1;color:var(--muted);font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
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
.toolbar{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:2px 0 10px}
.toolbar .search{flex:1 1 200px;min-width:120px;height:32px;border:1px solid var(--line);background:var(--panel);color:var(--text);border-radius:8px;padding:0 10px;font:inherit;font-size:13px}
.toolbar .chk{display:flex;align-items:center;gap:5px;color:var(--muted);font-size:12px;user-select:none;white-space:nowrap}
.spacer{flex:1}
.subtabs{display:flex;gap:5px;flex-wrap:wrap;margin-bottom:10px}
.subtabs button{border:1px solid var(--line);background:var(--panel);color:var(--muted);border-radius:999px;padding:4px 11px;font:inherit;font-size:12px;cursor:pointer;white-space:nowrap}
.subtabs button.on{background:rgba(59,110,245,.1);border-color:transparent;color:var(--brand);font-weight:600}
.subtabs .n{font-variant-numeric:tabular-nums;opacity:.7;margin-left:3px}
.subtabs .dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:5px;vertical-align:1px}
.pager{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-top:10px;color:var(--muted);font-size:12px}
.pager button{height:28px;padding:0 9px;font-size:12px}
.pager button:disabled{opacity:.45;cursor:default;border-color:var(--line);color:var(--muted)}
.pager select{height:28px;font-size:12px}
th.num,td.num{text-align:right;font-variant-numeric:tabular-nums}
td.bad{color:var(--bad)}
table.dense th,table.dense td{padding:6px 8px;font-size:12px}
.tag.sm{height:22px;padding:0 8px;font-size:11px}
.ipath{max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.chartbox{margin-top:12px;padding-top:12px;border-top:1px solid var(--line)}
.chartbox:first-of-type{margin-top:0;padding-top:0;border-top:none}
.chartbox h3{font-size:12px;color:var(--muted);margin:0 0 4px;font-weight:500}
.chartbox canvas{display:block;width:100%;height:170px}
.legend.mon{padding-left:0;flex-wrap:wrap;gap:6px 14px;margin:2px 0 0}
.mon-empty{padding:28px 0;text-align:center}
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
  .toolbar .search{flex-basis:100%}
  .toolbar .spacer{display:none}
  .hide-sm{display:none!important}
  .panel-head{flex-wrap:wrap}
  .panel-head .hint{flex-basis:100%;order:3;margin-left:0;white-space:normal;font-size:11px;line-height:16px}
  .chartbox canvas{height:140px}
  .pager{gap:6px}
  table.dense th,table.dense td{padding:5px 5px;font-size:11px}
  .ipath{max-width:96px}
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
      <button type="button" role="tab" data-tab="proc" aria-selected="false">进程</button>
      <button type="button" role="tab" data-tab="net" aria-selected="false">网络</button>
      <button type="button" role="tab" data-tab="monitor" aria-selected="false">监控</button>
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
        <div class="panel nic-card">
          <div class="panel-head"><h2>网卡</h2><span class="hint" id="nicHint"></span></div>
          <div id="ovNics"></div>
          <button class="link more" type="button" data-go="net">查看网络详情 →</button>
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

    <section data-page="proc" hidden>
      <div class="panel">
        <div class="panel-head"><h2>进程</h2><span class="hint" id="procHint"></span></div>
        <div class="toolbar">
          <input class="search" id="procQ" type="search" placeholder="搜索 PID / 名称 / 用户">
          <select id="procSort">
            <option value="cpu">按 CPU</option>
            <option value="mem">按内存</option>
            <option value="pid">按 PID</option>
            <option value="start">按启动时间</option>
          </select>
          <label class="chk"><input type="checkbox" id="procKernel"> 内核线程</label>
          <select id="procAuto">
            <option value="0">不自动刷新</option>
            <option value="3000">3s</option>
            <option value="5000">5s</option>
          </select>
          <span class="spacer"></span>
          <button class="btn" id="procRefresh">刷新</button>
        </div>
        <div class="table-wrap" id="procBody"></div>
        <div class="pager" id="procPager"></div>
      </div>
    </section>

    <section data-page="net" hidden>
      <div class="panel">
        <div class="panel-head"><h2>网络</h2><span class="hint" id="netHint"></span>
          <button class="link" type="button" id="netRefresh">刷新</button></div>
        <div class="subtabs" id="netTabs"></div>
        <div class="toolbar" id="netTools">
          <input class="search" id="netQ" type="search" placeholder="地址 / 端口 / 进程">
          <label class="chk" id="netListenWrap"><input type="checkbox" id="netListen"> 仅监听</label>
          <div class="subtabs" id="ipcKinds" hidden>
            <button type="button" data-kind="shm" class="on">共享内存</button>
            <button type="button" data-kind="msg">消息队列</button>
            <button type="button" data-kind="sem">信号量</button>
            <button type="button" data-kind="posix">POSIX</button>
          </div>
          <span class="spacer"></span>
        </div>
        <div id="netBody"></div>
        <div class="pager" id="netPager"></div>
      </div>
    </section>

    <section data-page="monitor" hidden>
      <div class="panel">
        <div class="panel-head"><h2>监控</h2><span class="hint" id="monHint"></span></div>
        <div class="subtabs" id="monRange">
          <button type="button" data-range="300000">最近 5 分钟</button>
          <button type="button" data-range="900000" class="on">15 分钟</button>
          <button type="button" data-range="3600000">1 小时</button>
        </div>
        <div class="subtabs" id="monSeries"></div>
        <div class="chartbox" id="monPercentBox">
          <h3>百分比</h3>
          <canvas id="monPercent"></canvas>
          <div class="legend mon" id="monPercentLegend"></div>
        </div>
        <div class="chartbox" id="monLoadBox">
          <h3>负载</h3>
          <canvas id="monLoad"></canvas>
          <div class="legend mon" id="monLoadLegend"></div>
        </div>
        <div class="chartbox" id="monRateBox">
          <h3>速率</h3>
          <canvas id="monRate"></canvas>
          <div class="legend mon" id="monRateLegend"></div>
        </div>
        <div class="emptyp mon-empty" id="monEmpty" hidden>暂无采样数据，等待 agent 采集</div>
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
  var TABS = ['overview', 'storage', 'file', 'service', 'proc', 'net', 'monitor', 'tty', 'caps'];
  var state = {
    data: null, hist: [], timer: null, period: 2000, tab: 'overview',
    proc: { data: null, page: 1, per: 10, sort: 'cpu', q: '', kernel: false, auto: 0, timer: null },
    net: { scope: 'nic', page: 1, per: 10, q: '', listen: false, ipcKind: 'shm', data: null, counts: null },
    mon: { range: 900000, selected: [], data: {}, timer: null }
  };

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
    if (name !== 'proc') stopProcAuto();
    if (name !== 'monitor') stopMonAuto();
    if (name === 'overview') setTimeout(drawChart, 0);
    else if (name === 'file') ensureFs();
    else if (name === 'service') ensureSvc();
    else if (name === 'proc') ensureProc();
    else if (name === 'net') ensureNet();
    else if (name === 'monitor') ensureMonitor();
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
      { k: '负载', v: la.length ? la.slice(0, 3).map(function (x) { return toNum(x).toFixed(2); }).join(' / ') : '—', s: '1 / 5 / 15 分钟', sm: true },
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
          var meta = [];
          if (n.mac) meta.push('MAC ' + n.mac);
          if (toNum(n.mtu)) meta.push('MTU ' + toNum(n.mtu));
          if (toNum(n.speed_mbps)) meta.push(toNum(n.speed_mbps) + 'M');
          meta.push('错误 ' + (toNum(n.rx_errors) + toNum(n.tx_errors)));
          return '<div class="nic"><span class="nm" title="' + esc(n.name) + '">' + esc(n.name) + '</span>' +
            '<span class="tag ' + p + '">' + esc(n.operstate || (n.up ? 'up' : 'down')) + '</span>' +
            '<span class="ip mono">' + esc(n.ip || '—') + '</span>' +
            '<span class="io">↓ ' + fmtBytes(n.rx_bytes) + ' · ↑ ' + fmtBytes(n.tx_bytes) + '</span>' +
            '<span class="meta mono">' + esc(meta.join(' · ')) + '</span></div>';
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
    else if (state.tab === 'net' && state.net.scope === 'nic') renderNicTable();
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

  // ---------------------------------------------------------------- 进程
  var PROC_PER = [10, 20, 50];
  var procSeq = 0;

  function procQuery(refresh) {
    var p = state.proc;
    return 'sort=' + encodeURIComponent(p.sort) +
      '&q=' + encodeURIComponent(p.q) +
      '&kernel=' + (p.kernel ? '1' : '0') +
      '&page=' + p.page + '&page_size=' + p.per +
      (refresh ? '&refresh=1' : '');
  }

  function loadProcs(refresh) {
    var p = state.proc, seq = ++procSeq;
    $('#procHint').textContent = '加载中…';
    fetch('api/v1/procs?' + procQuery(refresh), { cache: 'no-store' }).then(function (r) {
      return r.json();
    }).then(function (d) {
      if (seq !== procSeq) return;
      if (!d || d.error) throw new Error((d && d.error) || '读取失败');
      p.data = d;
      renderProcs();
    }).catch(function (e) {
      if (seq !== procSeq) return;
      $('#procHint').textContent = '';
      $('#procBody').innerHTML = '<div class="emptyp">进程读取失败：' + esc(e.message) + '</div>';
      $('#procPager').innerHTML = '';
    });
  }

  function procHintText(d) {
    var s = d.summary || {};
    var base = toNum(s.total) + ' 进程 · ' + toNum(s.threads) + ' 线程 · 负载 ' +
      toNum(s.load1).toFixed(2) + ' / ' + toNum(s.load5).toFixed(2) + ' / ' + toNum(s.load15).toFixed(2) +
      ' · 内存 ' + fmtKB(s.mem_used_kb) + ' / ' + fmtKB(s.mem_total_kb);
    if (state.proc.q.trim()) return base + ' · 匹配 ' + toNum(d.matched);
    if (!state.proc.kernel) {
      var hidden = Math.max(0, toNum(s.total) - toNum(d.matched));
      if (hidden) return base + ' · 显示 ' + toNum(d.matched) + '（已隐藏 ' + hidden + ' 个内核线程）';
    }
    return base;
  }

  function renderProcs() {
    var d = state.proc.data;
    if (!d) return;
    $('#procHint').textContent = procHintText(d);
    var rows = d.entries || [];
    $('#procBody').innerHTML = rows.length
      ? '<table class="dense"><thead><tr><th>PID</th><th class="hide-sm">用户</th><th>名称</th><th class="num">CPU</th>' +
        '<th class="num">内存</th><th>状态</th><th class="num hide-sm">线程</th><th class="hide-sm">启动</th></tr></thead><tbody>' +
        rows.map(function (r) {
          var cpu = toNum(r.cpu_pct);
          return '<tr><td class="mono">' + toNum(r.pid) + '</td><td class="hide-sm">' + esc(r.user || '—') + '</td>' +
            '<td class="mono ipath" title="' + esc(r.cmd || r.comm) + '">' + esc(r.comm || '—') + '</td>' +
            '<td class="num' + (cpu >= 50 ? ' bad' : '') + '">' + cpu.toFixed(1) + '%</td>' +
            '<td class="num">' + fmtKB(r.rss_kb) + '</td>' +
            '<td>' + esc(r.state_text || r.state || '—') + '</td>' +
            '<td class="num hide-sm">' + toNum(r.threads) + '</td>' +
            '<td class="mono hide-sm">' + fmtWhen(r.start_ms) + '</td></tr>';
        }).join('') + '</tbody></table>'
      : '<div class="emptyp">没有匹配的进程</div>';
    renderPager($('#procPager'), d, PROC_PER, state.proc.per, function (pg) {
      state.proc.page = pg; loadProcs(false);
    }, function (per) {
      state.proc.per = per; state.proc.page = 1; loadProcs(false);
    });
  }

  function ensureProc() {
    if (!state.proc.data) loadProcs(true);
    else renderProcs();
    startProcAuto();
  }

  function startProcAuto() {
    stopProcAuto();
    var ms = toNum(state.proc.auto);
    if (ms > 0 && state.tab === 'proc') {
      state.proc.timer = setInterval(function () {
        if (!document.hidden && state.tab === 'proc') loadProcs(true);
      }, ms);
    }
  }

  function stopProcAuto() {
    if (state.proc.timer) { clearInterval(state.proc.timer); state.proc.timer = null; }
  }

  // ---------------------------------------------------------------- 网络
  var NET_SCOPES = [['nic', '网卡'], ['tcp', 'TCP'], ['udp', 'UDP'], ['unix', 'UNIX'], ['ipc', 'IPC']];
  var IPC_KINDS = [['shm', '共享内存'], ['msg', '消息队列'], ['sem', '信号量'], ['posix', 'POSIX']];
  var NET_PER = [10, 20, 50];
  var netSeq = 0;
  var ipcTried = null; // 默认共享内存为空时，依次试探其它 IPC 类型

  function netCount(scope) {
    if (scope === 'nic') return state.data && state.data.nics ? state.data.nics.length : 0;
    var c = state.net.counts || {};
    return c[scope] == null ? -1 : toNum(c[scope]);
  }

  function renderNetTabs() {
    $('#netTabs').innerHTML = NET_SCOPES.map(function (s) {
      var n = netCount(s[0]);
      return '<button type="button" data-scope="' + s[0] + '" class="' + (state.net.scope === s[0] ? 'on' : '') + '">' +
        s[1] + (n >= 0 ? '<span class="n">' + n + '</span>' : '') + '</button>';
    }).join('');
    $('#netTabs').querySelectorAll('[data-scope]').forEach(function (b) {
      b.addEventListener('click', function () { setNetScope(b.getAttribute('data-scope')); });
    });
  }

  function setNetScope(scope) {
    if (state.net.scope === scope) return;
    state.net.scope = scope;
    state.net.page = 1;
    ipcTried = null;
    state.net.q = '';
    state.net.listen = false;
    $('#netQ').value = '';
    $('#netListen').checked = false;
    renderNetTabs();
    syncNetTools();
    if (scope === 'nic') renderNicTable();
    else loadNet(true);
  }

  function syncNetTools() {
    var ipc = state.net.scope === 'ipc';
    var nic = state.net.scope === 'nic';
    $('#netQ').hidden = nic;
    $('#netListenWrap').hidden = nic || ipc;
    $('#ipcKinds').hidden = !ipc;
  }

  function loadNetCounts() {
    fetch('api/v1/net?scope=tcp&page=1&page_size=1&proc=0', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d || !d.summary) return;
        state.net.counts = d.summary;
        renderNetTabs();
      }).catch(function () {});
  }

  function syncIpcKind() {
    $$('#ipcKinds button').forEach(function (x) {
      x.classList.toggle('on', x.getAttribute('data-kind') === state.net.ipcKind);
    });
  }

  // 设备可能只有 POSIX 共享内存（/dev/shm），SysV 三类全空。
  // 当前类型没有条目但总数大于 0 时，自动换到下一种，避免默认页显示空表。
  function tryNextIpcKind(ipc, list, kind) {
    if (list.length || !toNum(ipc.total)) return false;
    if (!ipcTried) ipcTried = {};
    ipcTried[kind] = true;
    for (var i = 0; i < IPC_KINDS.length; i++) {
      var k = IPC_KINDS[i][0];
      if (!ipcTried[k]) {
        ipcTried[k] = true;
        state.net.ipcKind = k;
        state.net.page = 1;
        syncIpcKind();
        loadNet(false);
        return true;
      }
    }
    return false;
  }

  function loadNet(fresh) {
    var n = state.net;
    if (n.scope === 'nic') { renderNicTable(); return; }
    var seq = ++netSeq;
    $('#netHint').textContent = n.scope === 'ipc' ? 'IPC 采集中…' : '加载中…';
    var qs = 'scope=' + n.scope + '&page=' + n.page + '&page_size=' + n.per +
      '&q=' + encodeURIComponent(n.q) + '&listen=' + (n.listen ? '1' : '0') +
      '&ipc_kind=' + n.ipcKind + '&proc=1' + (fresh ? '&refresh=1' : '');
    fetch('api/v1/net?' + qs, { cache: 'no-store' }).then(function (r) {
      return r.json();
    }).then(function (d) {
      if (seq !== netSeq) return;
      if (!d || d.error) throw new Error((d && d.error) || '读取失败');
      n.data = d;
      if (d.summary) {
        var c = n.counts || {};
        ['tcp', 'udp', 'unix', 'ipc'].forEach(function (k) {
          var v = toNum(d.summary[k]);
          if (v >= 0) c[k] = v;
        });
        n.counts = c;
        renderNetTabs();
      }
      renderNetTable();
    }).catch(function (e) {
      if (seq !== netSeq) return;
      $('#netHint').textContent = '';
      $('#netBody').innerHTML = '<div class="emptyp">网络读取失败：' + esc(e.message) + '</div>';
      $('#netPager').innerHTML = '';
    });
  }

  function renderNicTable() {
    var nics = (state.data && state.data.nics) || [];
    $('#netHint').textContent = nics.length ? nics.length + ' 个网卡' : '';
    $('#netPager').innerHTML = '';
    $('#netBody').innerHTML = nics.length
      ? '<div class="table-wrap"><table class="dense"><thead><tr><th>网卡</th><th>状态</th><th>IP</th>' +
        '<th class="hide-sm">MAC</th><th class="num hide-sm">MTU</th><th class="num hide-sm">速率</th>' +
        '<th class="num">↓ / ↑</th><th class="num hide-sm">错误</th></tr></thead><tbody>' +
        nics.map(function (n) {
          var errs = toNum(n.rx_errors) + toNum(n.tx_errors);
          return '<tr><td class="mono ipath" title="' + esc(n.name) + '">' + esc(n.name) + '</td>' +
            '<td><span class="tag sm ' + (nicUp(n) ? 'ok' : 'bad') + '">' + esc(n.operstate || (n.up ? 'up' : 'down')) + '</span></td>' +
            '<td class="mono">' + esc(n.ip || '—') + '</td>' +
            '<td class="mono hide-sm">' + esc(n.mac || '—') + '</td>' +
            '<td class="num hide-sm">' + (n.mtu ? toNum(n.mtu) : '—') + '</td>' +
            '<td class="num hide-sm">' + (toNum(n.speed_mbps) ? toNum(n.speed_mbps) + 'M' : '—') + '</td>' +
            '<td class="num">' + fmtBytesShort(n.rx_bytes) + '/' + fmtBytesShort(n.tx_bytes) + '</td>' +
            '<td class="num hide-sm">' + errs + '</td></tr>';
        }).join('') + '</tbody></table></div>'
      : '<div class="emptyp">暂无网卡信息</div>';
  }

  function netAddr(a, p) {
    if (!a) return '—';
    return (a.indexOf(':') >= 0 ? '[' + a + ']' : a) + ':' + toNum(p);
  }

  function netProcCell(r) {
    if (!r || !r.pid) return '—';
    return '<span class="mono">' + toNum(r.pid) + '</span> ' + esc(r.comm || '');
  }

  function renderNetTable() {
    var d = state.net.data;
    if (!d) return;
    var scope = state.net.scope;
    var html = '', pageData = null;
    $('#netPager').innerHTML = '';
    if (scope === 'tcp' || scope === 'udp') {
      var sec = d[scope] || {};
      pageData = sec;
      var rows = sec.entries || [];
      html = rows.length
        ? '<div class="table-wrap"><table class="dense"><thead><tr><th>本地</th><th>远端</th><th>状态</th><th class="hide-sm">进程</th></tr></thead><tbody>' +
          rows.map(function (r) {
            return '<tr><td class="mono">' + esc(netAddr(r.local_addr, r.local_port)) + '</td>' +
              '<td class="mono">' + esc(toNum(r.remote_port) ? netAddr(r.remote_addr, r.remote_port) : '—') + '</td>' +
              '<td>' + esc(r.state || '—') + '</td>' +
              '<td class="hide-sm">' + netProcCell(r) + '</td></tr>';
          }).join('') + '</tbody></table></div>'
        : '<div class="emptyp">没有匹配的连接</div>';
      $('#netHint').textContent = sec.truncated ? '条目过多，当前页已截断' : '';
    } else if (scope === 'unix') {
      var us = d.unix || {};
      pageData = us;
      var urows = us.entries || [];
      html = urows.length
        ? '<div class="table-wrap"><table class="dense"><thead><tr><th>路径</th><th class="hide-sm">类型</th><th>状态</th><th class="hide-sm">进程</th></tr></thead><tbody>' +
          urows.map(function (r) {
            return '<tr><td class="mono ipath" title="' + esc(r.path) + '">' + esc(r.path || '—') + '</td>' +
              '<td class="hide-sm">' + esc(r.type || '—') + '</td><td>' + esc(r.state || '—') + '</td>' +
              '<td class="hide-sm">' + netProcCell(r) + '</td></tr>';
          }).join('') + '</tbody></table></div>'
        : '<div class="emptyp">没有 UNIX 套接字</div>';
      $('#netHint').textContent = us.listening != null ? '监听 ' + toNum(us.listening) + ' · 连接 ' + toNum(us.connected) : '';
    } else if (scope === 'ipc') {
      var ipc = d.ipc || {};
      pageData = ipc;
      var kind = ipc.kind || state.net.ipcKind;
      var list = ipc[kind] || [];
      if (tryNextIpcKind(ipc, list, kind)) return;
      var heads = {
        shm: ['键', 'ID', '权限', '大小', '连接', '属主', '进程'],
        msg: ['键', 'ID', '权限', '已用', '消息', '属主'],
        sem: ['键', 'ID', '权限', '信号量', '属主'],
        posix: ['名称', '大小', '权限', '属主', '修改时间']
      }[kind] || ['键'];
      html = list.length
        ? '<div class="table-wrap"><table class="dense"><thead><tr>' +
          heads.map(function (h) { return '<th>' + h + '</th>'; }).join('') + '</tr></thead><tbody>' +
          list.map(function (r) {
            if (kind === 'shm') {
              return '<tr><td class="mono">' + esc(r.key || '—') + '</td><td class="mono">' + esc(r.id || '—') + '</td>' +
                '<td class="mono">' + esc(r.perms || '—') + '</td><td class="num">' + esc(r.size || '—') + '</td>' +
                '<td class="num">' + esc(r.nattch || '0') + '</td><td>' + esc(r.owner || '—') + '</td>' +
                '<td>' + netProcCell({ pid: r.pid, comm: r.comm }) + '</td></tr>';
            }
            if (kind === 'msg') {
              return '<tr><td class="mono">' + esc(r.key || '—') + '</td><td class="mono">' + esc(r.id || '—') + '</td>' +
                '<td class="mono">' + esc(r.perms || '—') + '</td><td class="num">' + esc(r.used || '—') + '</td>' +
                '<td class="num">' + esc(r.msgs || '0') + '</td><td>' + esc(r.owner || '—') + '</td></tr>';
            }
            if (kind === 'sem') {
              return '<tr><td class="mono">' + esc(r.key || '—') + '</td><td class="mono">' + esc(r.id || '—') + '</td>' +
                '<td class="mono">' + esc(r.perms || '—') + '</td><td class="num">' + esc(r.nsems || '0') + '</td>' +
                '<td>' + esc(r.owner || '—') + '</td></tr>';
            }
            return '<tr><td class="mono ipath" title="' + esc(r.name) + '">' + esc(r.name || '—') + '</td>' +
              '<td class="num">' + esc(r.size || '—') + '</td><td class="mono">' + esc(r.perms || '—') + '</td>' +
              '<td>' + esc(r.owner || '—') + '</td><td class="mono">' + (r.mtime_ms ? new Date(toNum(r.mtime_ms)).toLocaleString() : '—') + '</td></tr>';
          }).join('') + '</tbody></table></div>'
        : '<div class="emptyp">没有该类型的 IPC 对象</div>';
      $('#netHint').textContent = ipc.total != null ? '共 ' + toNum(ipc.total) + ' 个 IPC 对象' : '';
    }
    $('#netBody').innerHTML = html;
    if (pageData) {
      renderPager($('#netPager'), pageData, NET_PER, state.net.per, function (pg) {
        state.net.page = pg; loadNet(false);
      }, function (per) {
        state.net.per = per; state.net.page = 1; loadNet(false);
      });
    }
  }

  function ensureNet() {
    if (!state.net.counts) loadNetCounts();
    renderNetTabs();
    syncNetTools();
    if (state.net.scope === 'nic') renderNicTable();
    else if (state.net.data && state.net.data.scope === state.net.scope) renderNetTable();
    else loadNet(true);
  }

  // ---------------------------------------------------------------- 监控
  var MON_COLORS = ['#3b6ef5', '#e08a2e', '#2e9e63', '#9b59b6', '#d9534f', '#16a085'];
  var monSeq = 0;

  function monSeriesList() {
    var arr = (caps().metrics && caps().metrics.series) || [];
    var names = arr.filter(function (s) { return s.enabled; }).map(function (s) { return s.series; });
    return names.length ? names : ['cpu', 'mem', 'load'];
  }

  function monUnit(name) {
    if (name === 'cpu' || name === 'mem') return 'percent';
    if (name === 'load') return 'load';
    return 'rate';
  }

  function monLabel(name) {
    if (name === 'cpu') return 'CPU';
    if (name === 'mem') return '内存';
    if (name === 'load') return '负载';
    if (name.indexOf('net:') === 0) return name.slice(4) + ' 流量';
    if (name.indexOf('serial:') === 0) return name.slice(7) + ' 串口';
    return name;
  }

  function monColor(name) {
    var all = monSeriesList(), i = all.indexOf(name);
    return MON_COLORS[(i < 0 ? 0 : i) % MON_COLORS.length];
  }

  function renderMonChips() {
    var all = monSeriesList(), sel = state.mon.selected;
    $('#monSeries').innerHTML = all.map(function (n) {
      return '<button type="button" data-series="' + esc(n) + '" class="' + (sel.indexOf(n) >= 0 ? 'on' : '') + '">' +
        '<span class="dot" style="background:' + monColor(n) + '"></span>' + esc(monLabel(n)) + '</button>';
    }).join('');
    $('#monSeries').querySelectorAll('[data-series]').forEach(function (b) {
      b.addEventListener('click', function () { toggleMonSeries(b.getAttribute('data-series')); });
    });
  }

  function toggleMonSeries(name) {
    var sel = state.mon.selected, i = sel.indexOf(name);
    if (i >= 0) {
      if (sel.length === 1) return;
      sel.splice(i, 1);
    } else {
      sel.push(name);
    }
    renderMonChips();
    loadMon(true);
  }

  function ensureMonitor() {
    var all = monSeriesList();
    var sel = state.mon.selected.filter(function (n) { return all.indexOf(n) >= 0; });
    if (!sel.length) {
      ['cpu', 'mem', 'load'].forEach(function (n) { if (all.indexOf(n) >= 0) sel.push(n); });
    }
    if (!sel.length) sel = all.slice(0, 3);
    state.mon.selected = sel;
    renderMonChips();
    loadMon(true);
    startMonAuto();
  }

  function startMonAuto() {
    stopMonAuto();
    if (state.tab === 'monitor') {
      state.mon.timer = setInterval(function () {
        if (!document.hidden && state.tab === 'monitor') loadMon(false);
      }, 5000);
    }
  }

  function stopMonAuto() {
    if (state.mon.timer) { clearInterval(state.mon.timer); state.mon.timer = null; }
  }

  function loadMon(full) {
    var mon = state.mon, names = mon.selected.slice();
    if (!names.length) return;
    var seq = ++monSeq;
    var base = Date.now() - mon.range;
    if (full) mon.data = {};
    var jobs = names.map(function (name) {
      var since = base;
      if (!full) {
        var arr = mon.data[name];
        if (arr && arr.length) since = toNum(arr[arr.length - 1].ts);
      }
      return fetch('api/v1/metrics?series=' + encodeURIComponent(name) + '&since_ts=' + since, { cache: 'no-store' })
        .then(function (r) { return r.json(); })
        .then(function (d) {
          if (!d || !d.points) return;
          var arr = mon.data[name] || (mon.data[name] = []);
          d.points.forEach(function (p) {
            if (!arr.length || toNum(p.ts) > toNum(arr[arr.length - 1].ts)) arr.push(p);
          });
        }).catch(function () {});
    });
    Promise.all(jobs).then(function () {
      if (seq !== monSeq) return;
      trimMon();
      drawMon();
    });
  }

  function trimMon() {
    var base = Date.now() - state.mon.range - 60000;
    Object.keys(state.mon.data).forEach(function (name) {
      var arr = state.mon.data[name], i = 0;
      while (i < arr.length && toNum(arr[i].ts) < base) i++;
      if (i) arr.splice(0, i);
    });
  }

  function drawMon() {
    var mon = state.mon, now = Date.now(), xMin = now - mon.range;
    var groups = { percent: [], load: [], rate: [] };
    mon.selected.forEach(function (name) {
      var arr = mon.data[name] || [];
      var pts = arr.filter(function (p) { return toNum(p.ts) >= xMin; });
      if (!pts.length) return;
      var unit = monUnit(name);
      if (unit === 'rate') {
        var rx = [], tx = [], prev = null;
        pts.forEach(function (p) {
          if (prev) {
            var dt = (toNum(p.ts) - toNum(prev.ts)) / 1000;
            if (dt > 0) {
              rx.push([toNum(p.ts), Math.max(0, (toNum(p.rx) - toNum(prev.rx)) / dt)]);
              tx.push([toNum(p.ts), Math.max(0, (toNum(p.tx) - toNum(prev.tx)) / dt)]);
            }
          }
          prev = p;
        });
        if (rx.length) {
          groups.rate.push({
            lines: [
              { label: monLabel(name) + ' ↓', points: rx, color: monColor(name), dash: false },
              { label: '↑', points: tx, color: monColor(name), dash: true }
            ]
          });
        }
      } else {
        groups[unit].push({
          lines: [{ label: monLabel(name), points: pts.map(function (p) { return [toNum(p.ts), toNum(p.v)]; }), color: monColor(name) }]
        });
      }
    });
    var any = false;
    [
      ['percent', '#monPercent', '#monPercentLegend', function (v) { return v + '%'; }, 100],
      ['load', '#monLoad', '#monLoadLegend', function (v) { return v.toFixed(1); }, 0],
      ['rate', '#monRate', '#monRateLegend', function (v) { return fmtBytes(v) + '/s'; }, 0]
    ].forEach(function (g) {
      var items = groups[g[0]], cv = $(g[1]), box = cv.parentNode;
      box.hidden = !items.length;
      if (!items.length) return;
      any = true;
      var lines = [], maxV = 0;
      items.forEach(function (it) {
        it.lines.forEach(function (ln) {
          lines.push(ln);
          ln.points.forEach(function (p) { if (p[1] > maxV) maxV = p[1]; });
        });
      });
      var yMax = g[4] || Math.max(1, maxV * 1.15);
      drawLines(cv, lines, xMin, now, yMax, g[3]);
      $(g[2]).innerHTML = items.map(function (it) {
        return it.lines.map(function (ln) {
          return '<span><i style="background:' + ln.color + '"></i>' + esc(ln.label) + '</span>';
        }).join('');
      }).join('');
    });
    $('#monEmpty').hidden = any;
    var cfg = (caps().metrics && caps().metrics.series) || [];
    var iv = cfg.length ? cfg[0].interval_sec : 5;
    $('#monHint').textContent = '每 ' + iv + ' 秒采样 · ' +
      (mon.range >= 3600000 ? '最近 1 小时' : mon.range >= 900000 ? '最近 15 分钟' : '最近 5 分钟');
  }

  function drawLines(cv, lines, xMin, xMax, yMax, fmtY) {
    var dpr = window.devicePixelRatio || 1;
    var w = cv.clientWidth || 600, h = cv.clientHeight || 170;
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
    var ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    var cs = getComputedStyle(document.documentElement);
    var lineColor = cs.getPropertyValue('--line').trim() || '#e4e8f0';
    var muted = cs.getPropertyValue('--muted').trim() || '#7b8494';
    var pad = { l: 50, r: 8, t: 8, b: 18 };
    var cw = Math.max(1, w - pad.l - pad.r), ch = Math.max(1, h - pad.t - pad.b);
    ctx.font = '11px -apple-system,PingFang SC,sans-serif';
    ctx.textBaseline = 'middle';
    var span = Math.max(1, xMax - xMin);
    for (var i = 0; i <= 4; i++) {
      var y = pad.t + ch * (1 - i / 4);
      ctx.strokeStyle = lineColor;
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(pad.l + cw, y); ctx.stroke();
      ctx.fillStyle = muted;
      ctx.fillText(fmtY(yMax * i / 4), 4, y);
    }
    ctx.textBaseline = 'top';
    for (var k = 0; k <= 3; k++) {
      var d = new Date(xMin + span * k / 3);
      var lb = ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
      ctx.fillStyle = muted;
      ctx.fillText(lb, pad.l + cw * k / 3 - (k === 0 ? 0 : k === 3 ? 28 : 14), pad.t + ch + 4);
    }
    lines.forEach(function (ln) {
      if (!ln.points.length) return;
      ctx.strokeStyle = ln.color;
      ctx.lineWidth = 2;
      ctx.lineJoin = 'round';
      ctx.setLineDash(ln.dash ? [4, 3] : []);
      ctx.beginPath();
      ln.points.forEach(function (p, idx) {
        var x = pad.l + cw * (p[0] - xMin) / span;
        var y = pad.t + ch * (1 - Math.min(1, Math.max(0, p[1] / yMax)));
        if (idx) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      });
      ctx.stroke();
    });
    ctx.setLineDash([]);
  }

  function fmtWhen(ms) {
    if (!ms) return '—';
    var d = new Date(toNum(ms)), now = Date.now();
    var pad = function (n) { return (n < 10 ? '0' : '') + n; };
    if (now - ms < 86400000) return pad(d.getHours()) + ':' + pad(d.getMinutes());
    return (d.getMonth() + 1) + '-' + pad(d.getDate());
  }

  function renderPager(el, d, sizes, per, onPage, onPer) {
    if (!el) return;
    var pages = Math.max(1, toNum(d.pages));
    var page = Math.max(1, toNum(d.page));
    el.innerHTML = '<span>共 ' + toNum(d.matched) + ' 条 · 第 ' + page + ' / ' + pages + ' 页</span>' +
      '<span class="spacer"></span>' +
      '<select class="per">' + sizes.map(function (n) {
        return '<option value="' + n + '"' + (n === per ? ' selected' : '') + '>' + n + ' 条/页</option>';
      }).join('') + '</select>' +
      '<button class="btn" data-pg="-1"' + (page <= 1 ? ' disabled' : '') + '>上一页</button>' +
      '<button class="btn" data-pg="1"' + (page >= pages ? ' disabled' : '') + '>下一页</button>';
    el.querySelectorAll('[data-pg]').forEach(function (b) {
      b.addEventListener('click', function () { onPage(page + Number(b.getAttribute('data-pg'))); });
    });
    el.querySelector('.per').addEventListener('change', function (e) { onPer(Number(e.target.value) || per); });
  }

  function debounce(fn, ms) {
    var t = null;
    return function () {
      if (t) clearTimeout(t);
      t = setTimeout(fn, ms);
    };
  }

  $('#procQ').addEventListener('input', debounce(function () {
    state.proc.q = $('#procQ').value;
    state.proc.page = 1;
    loadProcs(false);
  }, 300));
  $('#procSort').addEventListener('change', function (e) {
    state.proc.sort = e.target.value;
    state.proc.page = 1;
    loadProcs(false);
  });
  $('#procKernel').addEventListener('change', function (e) {
    state.proc.kernel = e.target.checked;
    state.proc.page = 1;
    loadProcs(false);
  });
  $('#procAuto').addEventListener('change', function (e) {
    state.proc.auto = Number(e.target.value) || 0;
    startProcAuto();
    if (state.proc.auto) loadProcs(true);
  });
  $('#procRefresh').addEventListener('click', function () { loadProcs(true); });

  $('#netQ').addEventListener('input', debounce(function () {
    state.net.q = $('#netQ').value;
    state.net.page = 1;
    loadNet(false);
  }, 300));
  $('#netListen').addEventListener('change', function (e) {
    state.net.listen = e.target.checked;
    state.net.page = 1;
    loadNet(false);
  });
  $('#netRefresh').addEventListener('click', function () {
    ipcTried = null;
    if (state.net.scope === 'nic') renderNicTable();
    else loadNet(true);
  });
  $$('#ipcKinds [data-kind]').forEach(function (b) {
    b.addEventListener('click', function () {
      state.net.ipcKind = b.getAttribute('data-kind');
      state.net.page = 1;
      ipcTried = null;
      syncIpcKind();
      loadNet(false);
    });
  });
  $$('#monRange [data-range]').forEach(function (b) {
    b.addEventListener('click', function () {
      state.mon.range = Number(b.getAttribute('data-range')) || 900000;
      $$('#monRange button').forEach(function (x) { x.classList.toggle('on', x === b); });
      loadMon(true);
    });
  });
  window.addEventListener('resize', function () { drawMon(); });

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
