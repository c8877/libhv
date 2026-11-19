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
html,body{margin:0}
body{background:var(--bg);color:var(--text);font:14px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:22px 18px 40px}
.top{display:flex;gap:16px;align-items:center;flex-wrap:wrap;margin-bottom:18px}
.brand{display:flex;gap:12px;align-items:center;min-width:220px}
.logo{width:40px;height:40px;border-radius:11px;background:var(--brand);color:#fff;display:flex;align-items:center;justify-content:center;font:700 17px/1 ui-monospace,Menlo,monospace;letter-spacing:-.5px}
h1{font-size:20px;line-height:1.2;margin:0}
.sub{color:var(--muted);font-size:12px;margin-top:3px}
.tools{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-left:auto}
.tools label{display:flex;gap:6px;align-items:center;color:var(--muted);font-size:13px;cursor:pointer;user-select:none}
select,.btn{height:32px;border:1px solid var(--line);background:var(--panel);color:var(--text);border-radius:8px;padding:0 10px;font-size:13px;cursor:pointer}
select{padding-right:6px}
.btn:hover{border-color:var(--brand);color:var(--brand)}
.btn.ghost{background:transparent}
.tag{display:inline-flex;align-items:center;gap:5px;height:26px;padding:0 9px;border-radius:999px;font-size:12px;white-space:nowrap;background:var(--bar-bg);color:var(--muted)}
.tag::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}
.tag.ok{background:rgba(46,158,99,.12);color:var(--ok)}
.tag.warn{background:rgba(201,138,24,.14);color:var(--warn)}
.tag.bad{background:rgba(217,83,79,.13);color:var(--bad)}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(178px,1fr));gap:12px;margin-bottom:14px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 15px;box-shadow:var(--shadow);min-width:0}
.card .k{color:var(--muted);font-size:12px}
.card .v{font-size:21px;font-weight:650;letter-spacing:-.3px;margin:3px 0 2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.card .s{color:var(--muted);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px 17px;box-shadow:var(--shadow);margin-bottom:14px;min-width:0}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media (max-width:760px){.grid2{grid-template-columns:1fr}.tools{width:100%;margin-left:0}}
.panel-head{display:flex;align-items:baseline;gap:10px;margin-bottom:10px}
h2{font-size:14px;margin:0;font-weight:650}
.hint{color:var(--muted);font-size:12px;margin-left:auto}
#chart{display:block;width:100%;height:160px}
.legend{display:flex;gap:14px;align-items:center;color:var(--muted);font-size:12px;margin-top:4px;padding-left:34px}
.legend i{width:16px;height:3px;border-radius:2px;display:inline-block;margin-right:5px}
.legend .c1{background:#3b6ef5}.legend .c2{background:#e08a2e}
table{width:100%;border-collapse:collapse}
th,td{padding:7px 8px;text-align:left;border-bottom:1px solid var(--line);font-size:13px;vertical-align:middle}
th{color:var(--muted);font-weight:500;font-size:12px}
tr:last-child td{border-bottom:none}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}
.muted{color:var(--muted)}
.empty{color:var(--muted);font-size:13px;padding:8px 0}
.disk{padding:8px 0;border-bottom:1px solid var(--line)}
.disk:last-child{border-bottom:none}
.disk-top{display:flex;gap:10px;align-items:baseline;margin-bottom:5px}
.disk-top b{font-family:ui-monospace,Menlo,monospace;font-size:13px}
.disk-top .muted{font-size:12px}
.disk-top .pct{margin-left:auto;font-variant-numeric:tabular-nums;font-size:12px;color:var(--muted)}
.disk-top .pct.warn{color:var(--warn)}.disk-top .pct.bad{color:var(--bad)}
.bar{height:6px;border-radius:999px;background:var(--bar-bg);overflow:hidden}
.bar i{display:block;height:100%;border-radius:999px;background:var(--brand);transition:width .35s}
.bar i.warn{background:var(--warn)}.bar i.bad{background:var(--bad)}
.kv .row{display:flex;gap:16px;padding:7px 0;border-bottom:1px dashed var(--line);font-size:13px}
.kv .row:last-child{border-bottom:none}
.kv .k{color:var(--muted);width:92px;flex:none}
.kv .v{min-width:0;overflow-wrap:anywhere}
.chips{display:flex;gap:7px;flex-wrap:wrap}
.chip{background:var(--bar-bg);border-radius:7px;padding:3px 9px;font-size:12px;color:var(--text)}
.args{margin-top:11px;padding-top:10px;border-top:1px dashed var(--line);color:var(--muted);font-size:12px;overflow-wrap:anywhere}
.args code{font-family:ui-monospace,Menlo,monospace;color:var(--text)}
.fsrow{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
.fsrow .grow{flex:1;min-width:200px}
.fsroot{font-size:13px;overflow-wrap:anywhere}
.fsHost{margin-top:10px}
footer{color:var(--muted);font-size:12px;text-align:center;padding:8px 0 0}
</style>
</head>
<body>
<div class="wrap">
  <header class="top">
    <div class="brand">
      <div class="logo">rt</div>
      <div>
        <h1 id="devid">设备详情</h1>
        <div class="sub" id="sub">正在加载…</div>
      </div>
    </div>
    <div class="tools">
      <span class="tag" id="state">加载中</span>
      <label><input type="checkbox" id="auto" checked> 自动刷新</label>
      <select id="period" title="自动刷新间隔">
        <option value="2000">2s</option>
        <option value="5000">5s</option>
        <option value="10000">10s</option>
      </select>
      <button class="btn" id="refresh">刷新</button>
      <button class="btn ghost" id="export">导出 JSON</button>
    </div>
  </header>

  <main>
    <section class="cards" id="cards"></section>

    <section class="panel">
      <div class="panel-head"><h2>资源趋势</h2><span class="hint">CPU / 内存 · 最近 2 分钟</span></div>
      <canvas id="chart" height="160"></canvas>
      <div class="legend"><span><i class="c1"></i>CPU</span><span><i class="c2"></i>内存</span></div>
    </section>

    <div class="grid2">
      <section class="panel"><div class="panel-head"><h2>存储</h2></div><div id="disks"></div></section>
      <section class="panel"><div class="panel-head"><h2>网卡</h2></div><div id="nics"></div></section>
    </div>

    <section class="panel" id="fsPanel" hidden>
      <div class="panel-head"><h2>文件管理</h2><span class="hint" id="fsHint">浏览、上传、下载和编辑设备文件</span></div>
      <div class="fsrow">
        <div class="grow">
          <div class="fsroot mono" id="fsRoot">—</div>
          <div class="muted" id="fsInfo" style="font-size:12px;margin-top:4px">—</div>
        </div>
        <span class="tag" id="fsMode">只读</span>
        <button class="btn" id="fsOpen">打开文件管理</button>
      </div>
      <div id="fsHost" class="fsHost"></div>
    </section>

    <section class="panel" id="svcPanel" hidden>
      <div class="panel-head"><h2>服务管理</h2><span class="hint">启动、停止、自启与日志</span></div>
      <div class="fsrow">
        <div class="grow">
          <div class="fsroot" id="svcSummary">systemd 服务管理</div>
          <div class="muted" id="svcInfo" style="font-size:12px;margin-top:4px">systemd · 展开后读取服务列表</div>
        </div>
        <span class="tag" id="svcMode">只读</span>
        <button class="btn" id="svcOpen">打开服务管理</button>
      </div>
      <div id="svcHost" class="fsHost"></div>
    </section>

    <section class="panel"><div class="panel-head"><h2>系统信息</h2></div><div class="kv" id="sysinfo"></div></section>
    <section class="panel"><div class="panel-head"><h2>能力配置</h2></div><div class="chips" id="caps"></div><div class="args" id="args"></div></section>
    <section class="panel" id="certPanel" hidden><div class="panel-head"><h2>证书</h2></div><div class="kv" id="cert"></div></section>
    <section class="panel"><div class="panel-head"><h2>串口 TTY</h2><span class="hint" id="ttyCount"></span></div><div id="serial"></div></section>
  </main>

  <footer id="foot">本地页面 · agent 采集</footer>
</div>
<script src="/assets/fs-ui.js"></script>
<script src="/assets/svc-ui.js"></script>
<script src="/assets/file-view.js"></script>
<script>
(function () {
  'use strict';
  var $ = function (s) { return document.querySelector(s); };
  var state = { data: null, hist: [], timer: null, period: 2000 };

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

  function render(d) {
    state.data = d;
    document.title = (d.devid || '设备') + ' · rtunnel';
    $('#devid').textContent = d.devid || '设备详情';
    $('#sub').textContent = [d.machine, d.kernel, d.board].filter(Boolean).join(' · ') || '—';

    var on = !!d.online, st = $('#state');
    st.textContent = on ? 'relay 已连接' : 'relay 未连接';
    st.className = 'tag ' + (on ? 'ok' : 'warn');

    var mp = memPct(d), la = Array.isArray(d.loadavg) ? d.loadavg : [];
    var total = toNum(d.mem_total_kb), avail = memAvail(d);
    var cards = [
      ['连接状态', on ? (d.relay || '已连接') : '未连接',
       on ? (toNum(d.relay_count) > 1 ? 'relay ' + (toNum(d.relay_index) + 1) + '/' + d.relay_count : '长连接正常')
          : '隧道断开，本地页面仍可用'],
      ['CPU 使用率', toNum(d.cpu_usage).toFixed(1) + '%',
       toNum(d.cpu_count) + ' 核' + (d.cpu_cores && d.cpu_cores.length ? ' · 逐核采样中' : '')],
      ['内存', mp.toFixed(1) + '%', fmtKB(Math.max(0, total - avail)) + ' / ' + fmtKB(total)],
      ['负载', la.length ? toNum(la[0]).toFixed(2) : '—',
       la.length ? toNum(la[1]).toFixed(2) + ' / ' + toNum(la[2]).toFixed(2) : '—'],
      ['运行时长', fmtDur(d.uptime_sec), 'agent ' + fmtDur(d.agent_uptime_sec)]
    ];
    $('#cards').innerHTML = cards.map(function (c) {
      return '<div class="card"><div class="k">' + esc(c[0]) + '</div><div class="v">' +
             esc(c[1]) + '</div><div class="s">' + esc(c[2]) + '</div></div>';
    }).join('');

    var mounts = d.mounts || [];
    $('#disks').innerHTML = mounts.length ? mounts.map(function (m) {
      var t = toNum(m.total_kb), used = toNum(m.used_kb), avail = toNum(m.avail_kb);
      var base = used + avail, p = base ? Math.ceil(used * 100 / base) : 0, cls = tone(p);
      return '<div class="disk"><div class="disk-top"><b>' + esc(m.mount) + '</b><span class="muted">' +
             esc(m.fstype || '') + '</span><span class="pct ' + cls + '">' + p + '%</span></div>' +
             '<div class="bar"><i class="' + cls + '" style="width:' + p + '%"></i></div>' +
             '<div class="muted" style="font-size:12px;margin-top:4px">' + fmtKB(used) + ' / ' + fmtKB(t) + '</div></div>';
    }).join('') : '<div class="empty">无挂载点信息</div>';

    var nics = d.nics || [];
    $('#nics').innerHTML = nics.length
      ? '<table><thead><tr><th>接口</th><th>IP</th><th>掩码</th><th>状态</th></tr></thead><tbody>' +
        nics.map(function (n) {
          return '<tr><td class="mono">' + esc(n.name) + '</td><td class="mono">' + esc(n.ip || '—') +
                 '</td><td class="mono">' + esc(n.netmask || '—') + '</td><td><span class="tag ' +
                 (n.up ? 'ok' : 'bad') + '">' + (n.up ? 'up' : 'down') + '</span></td></tr>';
        }).join('') + '</tbody></table>'
      : '<div class="empty">暂无网卡信息</div>';

    var rows = [
      ['架构', d.machine], ['内核', d.kernel], ['CPU 型号', d.cpu_model],
      ['核心数', d.cpu_count ? d.cpu_count + ' 核' : ''],
      ['板卡型号', d.board], ['agent 版本', d.version],
      ['启动参数', d.args], ['采集时间', d.ts ? new Date(d.ts).toLocaleString() : '']
    ];
    $('#sysinfo').innerHTML = rows.map(function (r) {
      return '<div class="row"><span class="k">' + esc(r[0]) + '</span><span class="v' +
             (r[0] === '启动参数' ? ' mono' : '') + '">' + esc(r[1] || '—') + '</span></div>';
    }).join('');

    var c = d.caps || {}, chips = [];
    (c.ports || []).forEach(function (p) { chips.push('TCP ' + p); });
    if (c.shell) chips.push('Shell');
    if (c.sysinfo) chips.push('SYSINFO');
    if (c.ota) chips.push('OTA');
    chips.push(c.mtls ? 'mTLS' : 'mTLS 未启用');
    if (c.file_dir) chips.push('文件目录');
    if (c.fs) chips.push('文件管理' + (c.fs_rw ? '（读写）' : '（只读）'));
    if (c.svc) chips.push('服务管理' + (c.svc_rw ? '（可控制）' : '（只读）'));
    $('#caps').innerHTML = chips.map(function (x) { return '<span class="chip">' + esc(x) + '</span>'; }).join('');
    $('#args').innerHTML = c.file_dir ? '<span class="muted">file_dir</span> <code>' + esc(c.file_dir) + '</code>' : '';

    $('#fsPanel').hidden = !c.fs;
    if (c.fs) {
      $('#fsRoot').textContent = c.fs_root || '/';
      $('#fsMode').textContent = c.fs_rw ? '读写' : '只读';
      $('#fsMode').className = 'tag' + (c.fs_rw ? ' ok' : '');
      fsBaseInfo = 'WebSocket 文件通道' + (c.fs_token ? ' · 需要 Token' : '') +
        ' · 单文件上限 ' + fmtKB((c.fs_max_mb || 512) * 1024);
      renderFsInfo();
      // 设备已启用文件管理时默认展开，不用再点按钮
      if (!fsAutoOpened) {
        fsAutoOpened = true;
        setTimeout(function () { if (!fsOpen) $('#fsOpen').click(); }, 80);
      }
    } else if (fsOpen && fm) {
      fm.close();
      fsOpen = false;
      fsBtn.textContent = '打开文件管理';
    }

    $('#svcPanel').hidden = !c.svc;
    if (c.svc) {
      $('#svcSummary').textContent = 'systemd · ' + (c.svc_rw ? '可启停服务' : '只读浏览');
      $('#svcMode').textContent = c.svc_rw ? '可控制' : '只读';
      $('#svcMode').className = 'tag' + (c.svc_rw ? ' ok' : '');
      $('#svcInfo').textContent = 'systemd 服务管理' + (c.svc_token ? ' · 需要 Token' : '');
    } else if (svcOpen && sm) {
      sm.close();
      svcOpen = false;
      $('#svcOpen').textContent = '打开服务管理';
    }

    if (d.mtls) {
      $('#certPanel').hidden = false;
      $('#cert').innerHTML = [
        ['证书主体', d.cert_subject || '—'],
        ['签发者', d.cert_issuer || '—'],
        ['序列号', d.cert_serial || '—'],
        ['状态', d.cert_valid ? '有效' : '已过期', d.cert_valid ? 'ok' : 'bad']
      ].map(function (r) {
        var v = r[2] ? '<span class="tag ' + r[2] + '">' + esc(r[1]) + '</span>' : esc(r[1]);
        return '<div class="row"><span class="k">' + esc(r[0]) + '</span><span class="v">' + v + '</span></div>';
      }).join('');
    } else {
      $('#certPanel').hidden = true;
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
      : '<div class="empty">未发现串口</div>';

    drawChart();
  }

  function pushHist(d) {
    state.hist.push({ cpu: Math.max(0, Math.min(100, toNum(d.cpu_usage))), mem: memPct(d) });
    if (state.hist.length > 60) state.hist.shift();
  }

  function drawChart() {
    var cv = $('#chart');
    if (!cv) return;
    var dpr = window.devicePixelRatio || 1;
    var w = cv.clientWidth || 600, h = 160;
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

  function schedule() {
    if (state.timer) clearInterval(state.timer);
    state.timer = null;
    if ($('#auto').checked) {
      state.timer = setInterval(load, state.period);
    }
  }

  function load() {
    fetch('/api/v1/sysinfo', { cache: 'no-store' }).then(function (r) {
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
  window.addEventListener('resize', drawChart);

  var fm = null, fsOpen = false, fsAutoOpened = false;
  var sm = null, svcOpen = false;
  var fsBaseInfo = '', fsSpace = null;
  function renderFsInfo() {
    var txt = fsBaseInfo;
    if (fsSpace && fsSpace.total_kb) {
      txt += ' · 已用 ' + fmtKB(Math.max(0, fsSpace.total_kb - fsSpace.free_kb)) + ' / ' + fmtKB(fsSpace.total_kb);
    }
    $('#fsInfo').textContent = txt;
  }
  var fsBtn = $('#fsOpen');
  $('#fsOpen').addEventListener('click', function () {
    if (!window.RtunnelFs) { alert('文件管理组件加载失败，请刷新页面'); return; }
    var d = state.data || {};
    if (!fm) {
      fm = window.RtunnelFs.createManager({
        inline: true,
        container: $('#fsHost'),
        wsUrl: (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/fs',
        title: '文件管理 - ' + (d.devid || '设备'),
        onClose: function () { fsOpen = false; fsBtn.textContent = '打开文件管理'; },
        onSpace: function (s) { fsSpace = s; renderFsInfo(); }
      });
    }
    if (fsOpen) {
      fm.close();
      fsOpen = false;
      fsBtn.textContent = '打开文件管理';
      return;
    }
    fsOpen = true;
    fsBtn.textContent = '收起文件管理';
    fm.open();
    setTimeout(function () { $('#fsHost').scrollIntoView({ block: 'nearest', behavior: 'smooth' }); }, 50);
  });

  var svcBtn = $('#svcOpen');
  svcBtn.addEventListener('click', function () {
    if (!window.RtunnelSvc) { alert('服务管理组件加载失败，请刷新页面'); return; }
    var d = state.data || {};
    if (!sm) {
      sm = window.RtunnelSvc.createManager({
        inline: true,
        container: $('#svcHost'),
        wsUrl: (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/svc',
        title: '服务管理 - ' + (d.devid || '设备'),
        onClose: function () { svcOpen = false; svcBtn.textContent = '打开服务管理'; }
      });
    }
    if (svcOpen) {
      sm.close();
      svcOpen = false;
      svcBtn.textContent = '打开服务管理';
      return;
    }
    svcOpen = true;
    svcBtn.textContent = '收起服务管理';
    sm.open();
    setTimeout(function () { $('#svcHost').scrollIntoView({ block: 'nearest', behavior: 'smooth' }); }, 50);
  });

  schedule();
  load();
})();
</script>
</body>
</html>
)AGENTWEB";
