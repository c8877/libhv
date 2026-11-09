// 设备详情页（admin.js 之后加载，共享其全局工具）
// ---------------------------------------------------------------- Pages.设备详情
const AgentDetailPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },   // 由「每 5s 刷新」勾选框控制
  async enter(el, params) {
    this.el = el;
    el.innerHTML = '<div class="panel"><div class="empty">图表组件加载中…</div></div>';
    try { await loadScriptOnce('/vendor/chart.umd.min.js'); } catch (e) {}
    this.devid = decodeURIComponent(params[0] || '');
    this.seriesCfg = [];
    this.lastTs = {};
    this._sysinfoFull = true;
    try { await this.fetchSeriesCfg(); } catch (e) {}
    this.charts = {};
    this.period = 10;
    this.auto = false;   // 状态刷新默认关闭
    this.sampleAuto = false;
    this.seriesLastPull = {};
    this.ttysLoaded = false;
    const p = Page.prototype.enter.call(this, el, params);
    if (this.auto && !this.timer) {
      this.timer = setInterval(() => this.refresh().catch(() => {}), this.period * 1000);
    }
    return p;
  },
  leave() {
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
    if (this._sampler) { clearInterval(this._sampler); this._sampler = null; }
    if (this._retry) { clearTimeout(this._retry); this._retry = null; }
    Object.values(this.charts || {}).forEach(c => { try { c.destroy(); } catch (e) {} });
    this.charts = {};
  },
  capsFormHtml(si) {
    const caps = si.caps || {};
    const shellAllowed = roleOf() === 'admin';
    return UI.form({
      layout: 'grid', cols: 3,
      rows: [
        { label: '转发端口', control: '<input id="caps-ports" value="' + UI.esc((caps.ports || []).join(', ')) +
          '" placeholder="22, 8080, 8099">' },
        { label: '文件目录', control: '<input id="caps-dir" value="' + UI.esc(caps.file_dir || '/opt/data/files') + '">' },
        { raw: '<label class="tty-check"><input type="checkbox" id="caps-shell"' + (caps.shell ? ' checked' : '') +
          (shellAllowed ? '' : ' disabled') + '> 允许 Shell</label>' },
      ],
      left: '<span class="muted" id="caps-msg">' + (shellAllowed ? '' : '仅 admin 可修改 Shell') + '</span>',
      right: '<button data-caps-cancel="1" id="caps-cancel">取消</button>' +
        '<button class="primary" id="caps-save">保存并下发</button>',
    });
  },
  sysGrid(si, ts) {
    const rows = [
      ['架构', si.machine || '—'],
      ['内核', si.kernel || '—'],
      ['CPU 型号', si.cpu_model || '—'],
      ['核心数', (si.cpu_count || 0) + ' 核'],
      ['板卡型号', si.board || '—'],
      ['采集时间', ts ? new Date(ts).toLocaleString() : '—'],
    ];
    return UI.grid(rows.map(r => [UI.esc(r[0]), UI.esc(r[1])]));
  },
  diskColor(p) { return p >= 85 ? cssVar('--bad', '#e5534b') : p >= 70 ? cssVar('--warn', '#f5a623') : cssVar('--accent', '#4a9eff'); },
  updateSnapshot(d, si, rows) {
    const head = document.getElementById('si-head-kv');
    if (head) head.innerHTML = UI.kv({
      '远端地址': UI.esc(d.remote_addr),
      '接入时间': new Date(d.connected_at_ms).toLocaleString(),
      '最近活跃': UI.ago(d.last_seen_ms) + ' 前',
      '流量 ↓/↑': UI.bytes(d.bytes_in) + ' / ' + UI.bytes(d.bytes_out),
    });
    const nics = (si.nics || []).map(n => [UI.esc(n.name), UI.esc(n.ip), UI.esc(n.netmask),
      n.up ? '<span class="pill ok">up</span>' : '<span class="pill bad">down</span>']);
    const nw = document.getElementById('si-nic-wrap');
    if (nw) nw.innerHTML = UI.detailTable(['接口', 'IP', '掩码', '状态'], nics);
    const sw = document.getElementById('si-sys-wrap');
    if (sw) sw.innerHTML = this.sysGrid(si, Date.now());
    const fw = document.getElementById('si-flows-wrap');
    if (fw) fw.innerHTML = UI.detailTable(['A 用户:sid', 'c_sid', '状态', '开启于'], rows);
    this.diskRows = (si.mounts || []).map(m => {
      const t = Number(m.total_kb) || 0, f = Number(m.free_kb) || 0;
      return { mount: m.mount, fstype: m.fstype || '', total: t, used: t - f, free: f,
               pct: t > 0 ? Math.round((t - f) * 100 / t) : 0 };
    });
    const ch = this.charts && this.charts['disk'];
    if (ch && ch.data && ch.data.datasets.length >= 2) {
      const labels = (si.mounts || []).map(m => m.mount.length > 16 ? m.mount.slice(0, 15) + '…' : m.mount);
      if (labels.length === ch.data.labels.length) {
        ch.data.labels = labels;
        ch.data.datasets[0].data = (si.mounts || []).map(m => (Number(m.total_kb) || 0) - (Number(m.free_kb) || 0));
        ch.data.datasets[1].data = (si.mounts || []).map(m => Number(m.free_kb) || 0);
        ch.data.datasets[0].backgroundColor = (si.mounts || []).map(m => {
          const t = Number(m.total_kb) || 0;
          return this.diskColor(t > 0 ? Math.round((t - (Number(m.free_kb) || 0)) * 100 / t) : 0);
        });
        ch.update('none');
      }
    }
    const smap2 = {};
    (si.serial || []).forEach(x => { smap2[x.path] = x; });
    document.querySelectorAll('[data-tty-rx]').forEach(td => {
      const x = smap2[td.dataset.ttyRx];
      td.textContent = x ? UI.bytes(x.rx) : '—';
    });
    document.querySelectorAll('[data-tty-tx]').forEach(td => {
      const x = smap2[td.dataset.ttyTx];
      td.textContent = x ? UI.bytes(x.tx) : '—';
    });
  },
  async refresh() {
    const d = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid));
    let si = null, siTs = 0;
    try {
      const mode = this._sysinfoFull ? 'full' : 'inc';
      const r = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) + '/sysinfo?refresh=1&mode=' + mode);
      si = r.sysinfo; siTs = r.ts;
      if (r.mode === 'inc' && this.si) {
        const base = this.si;
        if (si.swap_free_kb != null) base.swap_free_kb = si.swap_free_kb;
        (si.mounts || []).forEach(p => {
          const m = (base.mounts || []).find(x => x.mount === p.mount);
          if (m && p.free_kb != null) m.free_kb = p.free_kb;
        });
        (si.nics || []).forEach(p => {
          const n = (base.nics || []).find(x => x.name === p.name);
          if (!n) return;
          if (p.ip != null) n.ip = p.ip;
          if (p.up != null) n.up = p.up;
        });
        si = base;
      } else {
        this.si = si;
      }
      this._sysinfoFull = false;
    } catch (e) {}

    const rows = (d.streams || []).map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid, s.c_sid,
      s.opened ? '<span class="pill ok">OPEN</span>' : '<span class="pill info">OPENING</span>',
      UI.ago(s.opened_at_ms),
    ]);

    let html = '<div class="panel mb16"><div class="panel-head"><h2>' + UI.esc(d.devid) + '</h2>' +
      '<span class="pill ok">在线</span><span class="spacer"></span>' +
      (si ? '<label class="si-auto"><input type="checkbox" id="si-auto"' + (this.auto ? ' checked' : '') +
            '> 状态刷新</label>' +
            '<select id="si-period" class="ml4">' +
            [5, 10, 30].map(v => '<option value="' + v + '"' + (this.period === v ? ' selected' : '') + '>' + v + 's</option>').join('') +
            '</select> <button id="si-export">导出 JSON</button>' : '') + '</div><div id="si-head-kv">' + UI.kv({
        '远端地址': UI.esc(d.remote_addr),
        '接入时间': new Date(d.connected_at_ms).toLocaleString(),
        '最近活跃': UI.ago(d.last_seen_ms) + ' 前',
        '流量 ↓/↑': UI.bytes(d.bytes_in) + ' / ' + UI.bytes(d.bytes_out),
      }) + '</div></div>';

    if (this._rendered && si && document.getElementById('si-head-kv')) {
      this.updateSnapshot(d, si, rows);
      return;
    }

    if (si) {
      Object.values(this.charts || {}).forEach(c => { try { c.destroy(); } catch (e) {} });
      this.charts = {};
      const kb = n => UI.bytes((Number(n) || 0) * 1024);

      const diskRows = (si.mounts || []).map(m => {
        const t = Number(m.total_kb) || 0, f = Number(m.free_kb) || 0;
        return { mount: m.mount, fstype: m.fstype || '', total: t, used: t - f, free: f,
                 pct: t > 0 ? Math.round((t - f) * 100 / t) : 0 };
      });
      this.diskRows = diskRows;
      const nics = (si.nics || []).map(n => [UI.esc(n.name), UI.esc(n.ip), UI.esc(n.netmask),
        n.up ? '<span class="pill ok">up</span>' : '<span class="pill bad">down</span>']);
      const nicPanel = UI.panel('网卡', '<div id="si-nic-wrap">' +
        UI.detailTable(['接口', 'IP', '掩码', '状态'], nics) + '</div>');

      const diskPanel = UI.panel('存储', '<div class="si-body"><canvas id="si-disk" height="' +
          Math.max(60, (si.mounts || []).length * 24 + 16) + '"></canvas></div>',
          { right: '<span class="muted-sm">条长按最大分区归一</span>' });
      html += '<div class="si-two">' + diskPanel + nicPanel + '</div>' +
        '<div id="si-series"></div>' +
        '<div id="si-metric"></div>' +
        '<div id="si-tty"></div>';

      const certText = si.mtls
        ? UI.kv({ '证书主体': UI.esc(si.cert_subject || ''), '签发者': UI.esc(si.cert_issuer || ''),
                  '序列号': UI.esc(String(si.cert_serial || '')), '状态': si.cert_valid ? '有效' : '已过期' })
        : '<div class="ui-pad12 muted">未启用 mTLS</div>';
      const caps = si.caps || {};
      const chips = [];
      (caps.ports || []).forEach(p => chips.push('TCP ' + p));
      if (caps.shell) chips.push('Shell');
      if (caps.sysinfo) chips.push('SYSINFO');
      if (caps.file_dir) chips.push('文件 ' + caps.file_dir);
      chips.push(caps.mtls ? 'mTLS' : 'mTLS 未启用');
      const capHtml = chips.map(c => '<span class="chip">' + UI.esc(c) + '</span>').join(' ');
      const capsForm = this.capsOpen ? this.capsFormHtml(si) : '';
      const runtimeTable = '<div class="detail-table-shell"><table class="detail-table detail-kv"><tbody>' +
        '<tr><th>agent 版本</th><td>' + UI.esc(si.version || '') + '</td></tr>' +
        '<tr class="ui-row caps-row' + (this.capsOpen ? ' open' : '') + '" data-caps-row="1"><th>能力</th><td>' +
          capHtml + (canManage() ? '<span class="chev">⌄</span>' : '') + '</td></tr>' +
        (capsForm ? '<tr class="caps-expand"><td colspan="2">' + capsForm + '</td></tr>' : '') +
        '<tr><th>启动参数</th><td><code class="fs12">' + UI.esc(si.args || '') + '</code></td></tr>' +
        '</tbody></table></div>';
      const sysTable = this.sysGrid(si, siTs);
      html += UI.panel('系统', '<div id="si-sys-wrap">' + sysTable + '</div>', { cls: 'mb16' }) +
        UI.panel('能力配置', '<div id="si-runtime-wrap">' + runtimeTable + '</div>', { cls: 'mb16' }) +
        UI.panel('证书', '<div id="si-cert-wrap">' + certText + '</div>', { cls: 'mb16' });
    }

    html += UI.panel('当前会话', '<div id="si-flows-wrap">' +
      UI.detailTable(['A 用户:sid', 'c_sid', '状态', '开启于'], rows) + '</div>');
    this.el.innerHTML = html;
    this._rendered = true;

    if (si) {
      const self = this;
      const c = document.getElementById('si-disk');
      if (c && window.Chart) {
        const labels = (si.mounts || []).map(m =>
          m.mount.length > 16 ? m.mount.slice(0, 15) + '…' : m.mount);
        const used = (si.mounts || []).map(m => (Number(m.total_kb) || 0) - (Number(m.free_kb) || 0));
        const free = (si.mounts || []).map(m => Number(m.free_kb) || 0);
        const maxT = (si.mounts || []).reduce((a, m) => Math.max(a, Number(m.total_kb) || 0), 1);
        const colors = (si.mounts || []).map(m => {
          const t = Number(m.total_kb) || 0, f = Number(m.free_kb) || 0;
          return self.diskColor(t > 0 ? Math.round((t - f) * 100 / t) : 0);
        });
        const pctPlugin = {
          id: 'diskPct',
          afterDatasetsDraw(chart) {
            const ctx2 = chart.ctx;
            const xScale = chart.scales.x;
            const meta = chart.getDatasetMeta(0);
            const rows = self.diskRows || [];
            ctx2.save();
            ctx2.fillStyle = cssVar('--muted', '#8b929c');
            ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
            ctx2.textBaseline = 'middle';
            const fsColors = {
              ext4: 'rgba(74,158,255,.16)', vfat: 'rgba(245,166,35,.18)',
              tmpfs: 'rgba(139,146,156,.18)', overlay: 'rgba(176,131,240,.18)',
              squashfs: 'rgba(87,171,90,.18)'
            };
            meta.data.forEach((bar, i) => {
              const row = rows[i];
              if (!row) return;
              const y0 = bar.y;
              ctx2.textAlign = 'left';
              ctx2.fillStyle = cssVar('--chart-axis', '#8b929c');
              ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
              const fs = row.fstype || '—';
              const fsShort = fs.length > 8 ? fs.slice(0, 7) + '…' : fs;
              const bw = tagTextWidth(ctx2, fsShort);
              const maxRight = 192;
              let nm = row.mount;
              ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
              while (nm.length > 1 && 6 + ctx2.measureText(nm).width + 10 + bw > maxRight) {
                nm = nm.slice(0, -1);
              }
              if (nm !== row.mount) nm = nm.replace(/.$/, '…');
              ctx2.fillStyle = cssVar('--chart-axis', '#8b929c');
              ctx2.fillText(nm, 6, y0);
              const nmW = ctx2.measureText(nm).width;
              const bx = 6 + nmW + 10;
              drawTagPill(ctx2, bx, y0, fsShort,
                          fsColors[row.fstype] || 'rgba(139,146,156,.14)',
                          cssVar('--fg', '#e6e9ee'));
              ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
              ctx2.fillStyle = cssVar('--muted', '#8b929c');
              const x = xScale.getPixelForValue(row.total);
              const y = bar.y;
              ctx2.fillText(row.pct + '%', x + 6, y);
              const pair = fmtKbPair(row.used, row.total);
              ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
              const usedW = ctx2.measureText(pair.used).width;
              const totalW = ctx2.measureText(pair.total).width;
              const slashW = tagTextWidth(ctx2, '/');
              const unitW = tagTextWidth(ctx2, pair.unit);
              const gap = 4;
              const groupW = usedW + gap + slashW + gap + totalW + gap + unitW;
              let gx = chart.width - 8 - groupW;
              ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
              ctx2.fillStyle = cssVar('--muted', '#8b929c');
              ctx2.fillText(pair.used, gx, y);
              gx += usedW + gap;
              gx += drawTagPill(ctx2, gx, y, '/', 'rgba(139,146,156,.18)',
                                cssVar('--fg', '#e6e9ee')) + gap;
              ctx2.font = '11px -apple-system, "Segoe UI", sans-serif';
              ctx2.fillStyle = cssVar('--muted', '#8b929c');
              ctx2.fillText(pair.total, gx, y);
              gx += totalW + gap;
              drawTagPill(ctx2, gx, y, pair.unit, 'rgba(139,146,156,.18)',
                          cssVar('--fg', '#e6e9ee'));
            });
            ctx2.restore();
          }
        };
        this.charts['disk'] = new Chart(c.getContext('2d'), {
          type: 'bar',
          data: { labels: labels, datasets: [
            { label: '已用', data: used, backgroundColor: colors, minBarLength: 2, stack: 's' },
            { label: '空闲', data: free, backgroundColor: cssVar('--border', '#2b3038'), stack: 's' },
          ] },
          options: { indexAxis: 'y', animation: false, responsive: true, maintainAspectRatio: false,
            layout: { padding: { left: 200, right: 150 } },
            interaction: { mode: 'index', intersect: false },
            scales: { x: { stacked: true, max: maxT, display: false },
                      y: { stacked: true, display: false } },
            plugins: {
              legend: { display: false },
              tooltip: {
                callbacks: {
                  title: items => {
                    const r = items[0] ? (self.diskRows || [])[items[0].dataIndex] : null;
                    return r ? (r.mount + (r.fstype ? '  [' + r.fstype + ']' : '')) : '';
                  },
                  label: ctx => {
                    const row = (self.diskRows || [])[ctx.dataIndex];
                    if (!row) return '';
                    return ['已用 ' + kb(row.used) + ' / ' + kb(row.total),
                            '空闲 ' + kb(row.free) + ' · ' + row.pct + '%'].join('\n');
                  }
                }
              }
            } },
          plugins: [pctPlugin]
        });
      }
      this.renderMetricCard(document.getElementById('si-metric'));
      if (document.getElementById('series-charts')) this.pullSeriesPanel(false);
      else this.renderSeriesPanel();
      if (!this.ttysLoaded) this.loadTtys();
      else this.renderTtyCard(document.getElementById('si-tty'));
      const cr = document.querySelector('[data-caps-row]');
      if (cr) cr.onclick = (e) => {
        if (e.target.closest('button,input,select,label')) return;
        this.capsOpen = !this.capsOpen;
        this._rendered = false;
        this.refresh();
      };
      const cs = document.getElementById('caps-save');
      if (cs) cs.onclick = async () => {
        const msg = document.getElementById('caps-msg');
        const raw = document.getElementById('caps-ports').value;
        const ports = raw.split(',').map(x => x.trim()).filter(Boolean).map(x => Number(x));
        if (ports.some(x => !Number.isInteger(x) || x <= 0 || x > 65535)) {
          msg.textContent = '端口必须是 1–65535 的整数'; return;
        }
        const body = { ports: ports };
        const fdir = document.getElementById('caps-dir').value.trim();
        if (fdir) body.file_dir = fdir;
        if (roleOf() === 'admin') body.shell = document.getElementById('caps-shell').checked;
        msg.textContent = '下发中…';
        try {
          const r = await Api.put('/api/v1/agents/' + encodeURIComponent(this.devid) + '/caps', body);
          this.capsOpen = false;
          this._rendered = false;
          if (r && r.persisted === false) UI.toast('已生效，但未能写入配置文件，重启后会丢失', true);
          else UI.toast('能力已更新');
          this.refresh();
        } catch (e) { msg.textContent = '失败：' + e.message; }
      };
      const cc = document.getElementById('caps-cancel');
      if (cc) cc.onclick = () => { this.capsOpen = false; this._rendered = false; this.refresh(); };
      const cb = document.getElementById('si-auto');
      const ps = document.getElementById('si-period');
      if (ps) this.period = Number(ps.value) || 10;
      const startStateTimer = () => {
        if (this.timer) { clearInterval(this.timer); this.timer = null; }
        if (this.auto) {
          this.timer = setInterval(() => this.refresh().catch(() => {}), this.period * 1000);
        }
      };
      if (cb) cb.onchange = () => { this.auto = cb.checked; startStateTimer(); };
      if (ps) ps.onchange = () => { this.period = Number(ps.value) || 10; startStateTimer(); };
      if (si && !si.cpu_count && !this._retry) {
        this._retry = setTimeout(() => { this._retry = null; this.refresh().catch(() => {}); }, 1200);
      }
      const btn = document.getElementById('si-export');
      if (btn) btn.onclick = () => {
        const blob = new Blob([JSON.stringify(si, null, 2)], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = self.devid + '-sysinfo.json';
        a.click();
        URL.revokeObjectURL(a.href);
      };
    }
  },




  seriesCandidates() {
    const out = [];
    ['cpu', 'mem', 'load'].forEach(x => out.push(x));
    ((this.si && this.si.nics) || []).forEach(n => { if (n && n.name) out.push('net:' + n.name); });
    (this.ttyDisc || []).forEach(d => {
      const b = (d.path || '').split('/').pop();
      if (b && b.indexOf('tty') === 0) out.push('serial:' + b);
    });
    return out.filter((v, i) => out.indexOf(v) === i);
  },
  seriesUnit(name) {
    if (name === 'cpu' || name === 'mem') return 'percent';
    if (name === 'load') return 'load';
    return 'rate';
  },
  seriesColor(i) {
    return ['#4a9eff', '#f5a623', '#57ab5a', '#b083f0', '#e5534b'][i % 5];
  },
  renderSeriesPanel() {
    const wrap = document.getElementById('si-series');
    if (!wrap) return;
    const enabled = (this.seriesCfg || []).filter(c => c.enabled);
    if (!enabled.length) { wrap.innerHTML = ''; return; }
    if (!this.seriesSel) {
      this.seriesSel = enabled.map(c => c.series).filter(s => s === 'cpu' || s === 'mem');
      if (!this.seriesSel.length) this.seriesSel = [enabled[0].series];
    }
    this.seriesSel = this.seriesSel.filter(s => enabled.some(c => c.series === s));
    const sampleAuto = '<label class="si-auto"><input type="checkbox" id="sample-auto"' +
      (this.sampleAuto ? ' checked' : '') + '> 自动采集</label>';
    const seriesRight = '<span class="muted" id="series-last"></span> <button id="series-full">全量</button>';
    const seriesBody = '<div class="series-chips" id="series-chips">' +
      '<label class="chip-all"><input type="checkbox" id="sel-all"> 全选</label>' +
      ['percent', 'load', 'rate'].map(u => {
        const list = enabled.filter(c => this.seriesUnit(c.series) === u);
        if (!list.length) return '';
        const label = { percent: '百分比', load: '负载', rate: '速率' }[u];
        return '<span class="chip-group"><b>' + label + '</b> ' +
          '<label><input type="checkbox" data-all="' + u + '"> 全选</label> ' +
          list.map(c => '<label><input type="checkbox" data-sel="' + UI.esc(c.series) + '"' +
            (this.seriesSel.indexOf(c.series) >= 0 ? ' checked' : '') + '> ' + UI.esc(c.series) + '</label>').join('') +
          '</span>';
      }).join('') +
      '</div><div id="series-charts">' +
      '<div class="series-group" id="series-wrap-percent" style="display:none"><canvas id="series-chart-percent" height="140"></canvas></div>' +
      '<div class="series-group" id="series-wrap-load" style="display:none"><canvas id="series-chart-load" height="120"></canvas></div>' +
      '<div class="series-group" id="series-wrap-rate" style="display:none"><canvas id="series-chart-rate" height="140"></canvas></div>' +
      '</div>';
    wrap.innerHTML = UI.panel('采样曲线', seriesBody, { cls: 'mb16', left: sampleAuto, right: seriesRight });
    const all = enabled.map(c => c.series);
    const syncBoxes = () => {
      wrap.querySelectorAll('[data-sel]').forEach(cb => { cb.checked = this.seriesSel.indexOf(cb.dataset.sel) >= 0; });
      wrap.querySelectorAll('[data-all]').forEach(cb => {
        const list = enabled.filter(c => this.seriesUnit(c.series) === cb.dataset.all).map(c => c.series);
        cb.checked = list.length > 0 && list.every(x => this.seriesSel.indexOf(x) >= 0);
      });
      const sa = document.getElementById('sel-all');
      if (sa) sa.checked = all.length > 0 && all.every(x => this.seriesSel.indexOf(x) >= 0);
    };
    const apply = (next) => {
      const added = next.filter(x => this.seriesSel.indexOf(x) < 0);
      this.seriesSel = next;
      syncBoxes();
      this.renderSeriesCharts();
      this.drawSeriesGroups();
      if (added.length) this.pullSeriesNames(added, true).then(() => {
        const now = Date.now();
        added.forEach(n => { this.seriesLastPull[n] = now; });
        this.drawSeriesGroups();
      });
    };
    wrap.querySelectorAll('[data-sel]').forEach(cb => {
      cb.onchange = () => {
        const on = [...wrap.querySelectorAll('[data-sel]:checked')].map(x => x.dataset.sel);
        apply(on);
      };
    });
    wrap.querySelectorAll('[data-all]').forEach(cb => {
      cb.onchange = () => {
        const list = enabled.filter(c => this.seriesUnit(c.series) === cb.dataset.all).map(c => c.series);
        const others = this.seriesSel.filter(x => list.indexOf(x) < 0);
        apply(cb.checked ? others.concat(list) : others);
      };
    });
    document.getElementById('sel-all').onchange = (e) => apply(e.target.checked ? all.slice() : []);
    syncBoxes();
    document.getElementById('series-full').onclick = () => this.pullSeriesPanel(true);
    const sa = document.getElementById('sample-auto');
    if (sa) sa.onchange = () => { this.sampleAuto = sa.checked; this.startSampleTimer(); };
    this.renderSeriesCharts();
    this.pullSeriesPanel(true);
  },
  renderSeriesCharts() {
    const groups = { percent: [], load: [], rate: [] };
    this.seriesSel.forEach(s => groups[this.seriesUnit(s)].push(s));
    const order = ['percent', 'load', 'rate'];
    this.seriesGroups = order.filter(u => groups[u].length).map(u => ({ unit: u, series: groups[u] }));
    order.forEach(u => {
      const w = document.getElementById('series-wrap-' + u);
      if (w) w.style.display = groups[u].length ? '' : 'none';
    });
  },
  async pullSeriesNames(names, full) {
    this.seriesCache = this.seriesCache || {};
    this.seriesLastTs = this.seriesLastTs || {};
    await Promise.all(names.map(async name => {
      const since = full ? 0 : (this.seriesLastTs[name] || 0);
      let r;
      try {
        r = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) +
          '/metrics/series?series=' + encodeURIComponent(name) + '&since_ts=' + since);
      } catch (e) { return; }
      if (r.truncated && since) { this.seriesLastTs[name] = 0; return this.pullSeriesNames([name], true); }
      if (!since) this.seriesCache[name] = [];
      this.seriesCache[name] = (this.seriesCache[name] || []).concat(r.points || []);
      while (this.seriesCache[name].length > 300) this.seriesCache[name].shift();
      this.seriesLastTs[name] = r.newest_ts || since;
    }));
  },
  startSampleTimer() {
    if (this.sampleTimer) { clearInterval(this.sampleTimer); this.sampleTimer = null; }
    if (!this.sampleAuto) return;
    this.sampleTimer = setInterval(() => this.sampleTick(), 1000);
  },
  async sampleTick() {
    if (!this.sampleAuto || document.hidden) return;
    const now = Date.now();
    const due = (this.seriesSel || []).filter(name => {
      const cfg = (this.seriesCfg || []).find(c => c.series === name);
      if (!cfg) return false;
      return now - (this.seriesLastPull[name] || 0) >= Math.max(1, Number(cfg.interval_sec) || 5) * 1000;
    });
    if (!due.length) return;
    due.forEach(n => { this.seriesLastPull[n] = now; });
    await this.pullSeriesNames(due, false);
    this.drawSeriesGroups();
  },
  async pullSeriesPanel(full) {
    if (!this.seriesSel || !this.seriesSel.length) return;
    await this.pullSeriesNames(this.seriesSel, full);
    const now = Date.now();
    this.seriesSel.forEach(n => { this.seriesLastPull[n] = now; });
    this.drawSeriesGroups();
  },
  seriesValueArray(name, points) {
    if (this.seriesUnit(name) !== 'rate') {
      return points.map(p => [Number(p.ts), Number(p.v) || 0]);
    }
    let prx = 0, ptx = 0, pts = 0;
    const rx = [], tx = [];
    points.forEach(p => {
      const t = Number(p.ts) || 0;
      const dt = pts ? Math.max(1, t - pts) / 1000 : 1;
      rx.push([t, pts ? Math.max(0, (Number(p.rx) - prx) / dt) : 0]);
      tx.push([t, pts ? Math.max(0, (Number(p.tx) - ptx) / dt) : 0]);
      prx = Number(p.rx) || 0; ptx = Number(p.tx) || 0; pts = t;
    });
    return { rx: rx, tx: tx };
  },
  drawSeriesGroups() {
    if (!this.seriesGroups) return;
    this.seriesGroups.forEach((g) => {
      const c = document.getElementById('series-chart-' + g.unit);
      if (!c || !window.Chart) return;
      try { const ex = Chart.getChart(c); if (ex) ex.destroy(); } catch (e) {}
      const tsSet = new Set();
      const dataBySeries = {};
      g.series.forEach(name => {
        const pts = this.seriesCache[name] || [];
        const v = this.seriesValueArray(name, pts);
        dataBySeries[name] = v;
        if (Array.isArray(v)) v.forEach(x => tsSet.add(x[0]));
        else { v.rx.forEach(x => tsSet.add(x[0])); v.tx.forEach(x => tsSet.add(x[0])); }
      });
      const labels = [...tsSet].sort((a, b) => a - b);
      const idx = new Map(labels.map((t, i) => [t, i]));
      const datasets = [];
      g.series.forEach((name, si) => {
        const color = this.seriesColor(si);
        const v = dataBySeries[name];
        const put = (arr) => {
          const out = new Array(labels.length).fill(null);
          arr.forEach(x => { const i = idx.get(x[0]); if (i !== undefined) out[i] = x[1]; });
          return out;
        };
        if (Array.isArray(v)) {
          datasets.push({ label: name, data: put(v), borderColor: color, backgroundColor: color + '1f',
            borderWidth: 2, pointRadius: 0, fill: true, tension: .2 });
        } else {
          datasets.push({ label: name + ' RX', data: put(v.rx), borderColor: color, borderWidth: 2, pointRadius: 0, fill: false });
          datasets.push({ label: name + ' TX', data: put(v.tx), borderColor: color, borderDash: [4, 3], borderWidth: 2, pointRadius: 0, fill: false });
        }
      });
      new Chart(c.getContext('2d'), {
        type: 'line',
        data: { labels: labels.map(t => new Date(t).toLocaleTimeString('zh-CN', { hour12: false })), datasets: datasets },
        options: { animation: false, responsive: true, maintainAspectRatio: false,
          interaction: { mode: 'index', intersect: false },
          plugins: { legend: { display: true, labels: { boxWidth: 10 } } },
          scales: { x: { display: false }, y: { beginAtZero: true, ticks: { maxTicksLimit: 4 } } } }
      });
    });
  },
  openSeriesPicker() {
    const existing = (this.seriesCfg || []).map(c => c.series);
    const cand = this.seriesCandidates();
    const mask = document.createElement('div');
    mask.className = 'tty-mask';
    mask.innerHTML = '<div class="tty-modal"><h2>添加采样指标</h2>' +
      cand.filter(x => existing.indexOf(x) < 0).map((x, i) =>
        '<label class="row"><input type="checkbox" data-cand="' + UI.esc(x) + '"> ' + UI.esc(x) + '</label>').join('') +
      (cand.filter(x => existing.indexOf(x) < 0).length ? '' : '<div class="muted">没有可添加的指标</div>') +
      '<div class="tty-btns"><button id="sp-cancel">取消</button><button id="sp-add" class="primary">添加</button></div></div>';
    document.body.appendChild(mask);
    const close = () => mask.remove();
    mask.querySelector('#sp-cancel').onclick = close;
    mask.onclick = e => { if (e.target === mask) close(); };
    mask.querySelector('#sp-add').onclick = async () => {
      const picked = [...mask.querySelectorAll('[data-cand]:checked')].map(x => x.dataset.cand);
      if (!picked.length) return close();
      const list = existing.map(s => (this.seriesCfg.find(c => c.series === s))).concat(
        picked.map(s => ({ series: s, enabled: true, interval_sec: 5, points: 720 })));
      try {
        await Api.put('/api/v1/agents/' + encodeURIComponent(this.devid) + '/metrics/series/config', list);
        close();
        await this.reloadSeries();
        this.renderSeriesPanel();
      } catch (e) { this.ttyError(e.message); }
    };
  },
  async fetchSeriesCfg() {
    const list = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) + '/metrics/series/config');
    this.seriesCfg = Array.isArray(list) ? list : [];
  },
  renderMetricCard(wrap) {
    if (!wrap) return;
    const can = canManage();
    const list = this.seriesCfg || [];
    const groups = { percent: [], load: [], rate: [] };
    list.forEach(c => groups[this.seriesUnit(c.series)].push(c));
    const syncCount = list.filter(c => c.enabled && !c.synced).length;
    const dur = c => {
      const sec = (Number(c.interval_sec) || 1) * (Number(c.points) || 0);
      if (sec >= 3600) return (sec / 3600).toFixed(sec % 3600 ? 1 : 0) + ' h';
      if (sec >= 60) return Math.round(sec / 60) + ' m';
      return sec + ' s';
    };
    const badge = c => !c.enabled ? '<span class="dot off"></span>禁用'
      : c.synced ? '<span class="dot ok"></span>已同步'
      : '<span class="dot warn"></span>未同步';
    const groupName = { percent: '百分比', load: '负载', rate: '速率' };
    let rows = '';
    ['percent', 'load', 'rate'].forEach(u => {
      if (!groups[u].length) return;
      rows += '<tr class="metric-group"><td colspan="6">' + groupName[u] + '</td></tr>';
      groups[u].forEach(c => {
        const open = this.expandMetric === c.series;
        rows += '<tr class="ui-row metric-row' + (open ? ' open' : '') + '" data-mrow="' + UI.esc(c.series) + '">' +
          '<td class="metric-name"><b>' + UI.esc(c.series) + '</b><span class="metric-type">' + groupName[u] + '</span></td>' +
          '<td class="num">' + c.interval_sec + ' s</td>' +
          '<td class="num">' + c.points + ' 点 <span class="muted">· ' + dur(c) + '</span></td>' +
          '<td>' + badge(c) + '</td>' +
          '<td class="center">' + (can ? '<label class="switch"><input type="checkbox" data-tog="' + UI.esc(c.series) + '"' +
            (c.enabled ? ' checked' : '') + '><span></span></label>' : (c.enabled ? '开' : '关')) + '</td>' +
          '<td class="chev">⌄</td></tr>';
        if (open) {
          rows += '<tr class="metric-expand"><td colspan="6">' + UI.form({
            layout: 'grid', cols: 3,
            rows: [
              { label: '间隔（秒）', control: '<input id="me-int" type="number" min="1" max="300" value="' + c.interval_sec + '">' },
              { label: '保留点数', control: '<input id="me-pts" type="number" min="60" max="86400" value="' + c.points + '">' },
              { raw: '<div class="metric-readonly"><span>预计保留</span><b>' + dur(c) + '</b></div>' },
            ],
            left: '<span class="metric-sync">' + (c.enabled
              ? (c.synced ? '已下发并确认' : '已下发，等待 agent 确认')
              : '已禁用，不参与采样') + '</span>',
            right: '<button data-mdel="' + UI.esc(c.series) + '">删除</button>' +
              '<button data-mcancel="1">取消</button>' +
              '<button class="primary" data-msave="' + UI.esc(c.series) + '">保存并下发</button>',
          }) + '</td></tr>';
        }
      });
    });
    const metricBody = '<div class="detail-table-shell"><table class="detail-table metric-table"><tr><th>指标</th><th>间隔</th><th>保留</th>' +
      '<th>同步</th><th>启用</th><th></th></tr>' +
      (rows || '<tr><td colspan="6" class="muted">暂无配置</td></tr>') + '</table></div>';
    wrap.innerHTML = UI.panel('采样配置', metricBody, { cls: 'mb16',
      left: '<span class="muted ml10">' + list.length + ' 项 · ' + syncCount + ' 项未同步</span>',
      right: (can ? '<button id="metric-add">+ 添加指标</button>' : '') });
    if (!can) return;
    const save = async (next) => {
      try {
        await Api.put('/api/v1/agents/' + encodeURIComponent(this.devid) + '/metrics/series/config',
          next.map(c => ({ series: c.series, enabled: c.enabled, interval_sec: c.interval_sec, points: c.points })));
        this.expandMetric = null;
        await this.reloadSeries();
        this.renderSeriesPanel();
      } catch (e) { this.ttyError(e.message); }
    };
    wrap.querySelectorAll('[data-mrow]').forEach(tr => {
      tr.onclick = (e) => {
        if (e.target.closest('input,button,label')) return;
        this.expandMetric = this.expandMetric === tr.dataset.mrow ? null : tr.dataset.mrow;
        this.renderMetricCard(wrap);
      };
    });
    wrap.querySelectorAll('[data-tog]').forEach(ib => {
      ib.onclick = e => e.stopPropagation();
      ib.onchange = () => {
        const next = list.map(c => c.series === ib.dataset.tog ? Object.assign({}, c, { enabled: ib.checked }) : c);
        save(next);
      };
    });
    wrap.querySelectorAll('[data-msave]').forEach(b => {
      b.onclick = () => {
        const name = b.dataset.msave;
        const next = list.map(c => c.series === name ? Object.assign({}, c, {
          interval_sec: Number(document.getElementById('me-int').value) || c.interval_sec,
          points: Number(document.getElementById('me-pts').value) || c.points,
        }) : c);
        save(next);
      };
    });
    wrap.querySelectorAll('[data-mcancel]').forEach(b => {
      b.onclick = () => { this.expandMetric = null; this.renderMetricCard(wrap); };
    });
    wrap.querySelectorAll('[data-mdel]').forEach(b => {
      b.onclick = async () => {
        if (!await UI.confirm({ title: '删除指标', message: '确定删除 ' + b.dataset.mdel + '？', danger: true })) return;
        save(list.filter(c => c.series !== b.dataset.mdel));
      };
    });
    const add = document.getElementById('metric-add');
    if (add) add.onclick = () => this.openSeriesPicker();
  },
  async reloadSeries() {
    try { await this.fetchSeriesCfg(); } catch (e) {}
    this.renderMetricCard(document.getElementById('si-metric'));
  },
  async loadTtys(force) {
    const wrap = document.getElementById('si-tty');
    if (!wrap) return;
    if (this.ttysLoaded && !force) return;
    try {
      const list = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) + '/ttys');
      this.ttys = Array.isArray(list) ? list : [];
    } catch (e) {
      wrap.innerHTML = '';
      return;
    }
    try {
      this.ttyDisc = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) + '/ttys/discovered');
    } catch (e) { this.ttyDisc = []; }
    this.ttysLoaded = true;
    this.renderTtyCard(wrap);
  },
  ttyFormHtml(it, isNew) {
    const bauds = [1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600];
    const opts = (a, cur) => a.map(v => '<option value="' + v + '"' + (v === cur ? ' selected' : '') + '>' + v + '</option>').join('');
    return UI.form({
      layout: 'grid', cols: 4,
      attrs: 'data-new="' + (isNew ? '1' : '0') + '"',
      rows: [
        { label: '别名', control: '<input id="tf-alias" value="' + UI.esc(it.alias || '') + '" placeholder="设备内唯一">' },
        { label: '路径', control: '<input id="tf-path" value="' + UI.esc(it.path || '') + '"' +
          (it.path && !isNew ? ' readonly' : '') + '>' },
        { label: '波特率', control: '<select id="tf-baud">' + opts(bauds, it.baud) + '</select>' },
        { label: '数据位', control: '<select id="tf-db">' + opts([5,6,7,8], it.data_bits) + '</select>' },
        { label: '校验', control: '<select id="tf-par">' + opts(['N','E','O'], it.parity) + '</select>' },
        { label: '停止位', control: '<select id="tf-stop">' + opts([1,2], it.stop_bits) + '</select>' },
        { raw: '<label class="tty-check"><input type="checkbox" id="tf-en"' + (it.enabled ? ' checked' : '') + '> 启用</label>' },
      ],
      left: '<div class="ui-form-left">' +
        (!isNew && roleOf() === 'admin' ? '<button data-tf-take="' + UI.esc(it.alias || it.path) + '">强制接管</button>' : '') +
        '<button data-tf-copy="' + UI.esc(it.path || '') + '">复制路径</button></div>',
      right: (!isNew && canManage() ? '<button data-tf-del="' + UI.esc(it.path) + '">删除</button>' : '') +
        '<button data-tf-cancel="1">取消</button>' +
        '<button class="primary" data-tf-save="1">' + (isNew ? '添加并下发' : '保存并下发') + '</button>',
    });
  },
  async saveTtyForm(wrap, isNew) {
    const alias = document.getElementById('tf-alias').value.trim();
    const path = document.getElementById('tf-path').value.trim();
    if (!path) { this.ttyError('路径不能为空'); return; }
    const it = (this.ttys || []).find(x => x.path === path);
    const body = {
      path: path,
      alias: alias,
      enabled: document.getElementById('tf-en').checked,
      baud: Number(document.getElementById('tf-baud').value),
      data_bits: Number(document.getElementById('tf-db').value),
      parity: document.getElementById('tf-par').value,
      stop_bits: Number(document.getElementById('tf-stop').value),
      roles: (it && it.roles) || 'operator,manager,admin',
    };
    try {
      await Api.put('/api/v1/agents/' + encodeURIComponent(this.devid) + '/ttys', body);
      this.expandTty = null;
      this.addTtyPath = null;
      await this.loadTtys(true);
    } catch (e) { this.ttyError(e.message); }
  },
  renderTtyCard(wrap) {
    const can = canManage();
    const smap = {};
    ((this.si && this.si.serial) || []).forEach(x => { smap[x.path] = x; });
    const rxTxt = p => smap[p] ? UI.bytes(smap[p].rx) : '—';
    const txTxt = p => smap[p] ? UI.bytes(smap[p].tx) : '—';
    const configured = new Set((this.ttys || []).map(t => t.path));
    const unAll = (this.ttyDisc || []).filter(d => !configured.has(d.path));
    const tab = this.ttyTab || (this.ttys && this.ttys.length ? 'config' : 'unconfig');
    this.ttyTab = tab;
    const q = (this.ttyQuery || '').trim().toLowerCase();
    let rows = '';
    if (tab === 'config') {
      rows = (this.ttys || []).map(t => {
        const st = t.enabled ? '<span class="dot ok"></span>启用' : '<span class="dot off"></span>禁用';
        const open = this.expandTty === t.path;
        let r = '<tr class="ui-row tty-row' + (open ? ' open' : '') + '" data-trow="' + UI.esc(t.path) + '">' +
          '<td class="tty-main"><b>' + UI.esc(t.alias || '—') + '</b>' +
          '<div class="tty-path" title="' + UI.esc(t.path) + '">' + UI.esc(t.path) + '</div></td>' +
          '<td>' + st + '</td>' +
          '<td class="tty-param">' + t.baud + ' · ' + t.data_bits + t.parity + t.stop_bits + '</td>' +
          '<td class="tty-stat" data-tty-rx="' + UI.esc(t.path) + '">' + rxTxt(t.path) + '</td>' +
          '<td class="tty-stat" data-tty-tx="' + UI.esc(t.path) + '">' + txTxt(t.path) + '</td>' +
          '<td class="tty-ops"><a class="btn primary" href="#/tty/' + encodeURIComponent(this.devid) + '/' +
            encodeURIComponent(t.alias || t.path) + '">调试</a><span class="chev">⌄</span></td></tr>';
        if (open) r += '<tr class="tty-expand"><td colspan="6">' + this.ttyFormHtml(t, false) + '</td></tr>';
        return r;
      }).join('');
    } else {
      rows = unAll.filter(d => !q || (d.path || '').toLowerCase().indexOf(q) >= 0).map(d => {
        const open = this.addTtyPath === d.path;
        let r = '<tr class="ui-row tty-row' + (open ? ' open' : '') + '" data-trow="' + UI.esc(d.path) + '">' +
          '<td class="tty-main"><b>未配置</b><div class="tty-path" title="' + UI.esc(d.path) + '">' +
          UI.esc(d.path) + '</div></td><td><span class="pill">未配置</span></td><td>—</td>' +
          '<td class="tty-stat" data-tty-rx="' + UI.esc(d.path) + '">' + rxTxt(d.path) + '</td>' +
          '<td class="tty-stat" data-tty-tx="' + UI.esc(d.path) + '">' + txTxt(d.path) + '</td>' +
          '<td class="tty-ops">' + (can ? '<button data-addpath="' + UI.esc(d.path) + '">添加</button>' : '') +
          '<span class="chev">⌄</span></td></tr>';
        if (open) {
          r += '<tr class="tty-expand"><td colspan="6">' + this.ttyFormHtml({ path: d.path, baud: 115200,
            data_bits: 8, parity: 'N', stop_bits: 1, enabled: true }, true) + '</td></tr>';
        }
        return r;
      }).join('');
      if (this.addTtyPath === '') {
        rows += '<tr class="tty-expand"><td colspan="6">' + this.ttyFormHtml({ path: '', baud: 115200,
          data_bits: 8, parity: 'N', stop_bits: 1, enabled: true }, true) + '</td></tr>';
      }
    }
    const openAny = (tab === 'config') ? !!this.expandTty : (this.addTtyPath !== null && this.addTtyPath !== undefined);
    const search = '<input id="tty-q" class="tty-search" placeholder="搜索别名 / 路径" value="' + UI.esc(this.ttyQuery || '') + '">';
    const ttyBody = '<div class="tty-table-wrap detail-table-shell' + (openAny ? ' open' : '') + '"><table class="detail-table tty-table"><tr>' +
      '<th>设备</th><th>状态</th><th>参数</th><th>RX</th><th>TX</th><th>操作</th></tr>' +
      (rows || '<tr><td colspan="6" class="muted">' +
        (tab === 'config' ? '暂无已配置串口' : (q ? '没有匹配的串口' : '没有未配置的串口')) + '</td></tr>') +
      '</table></div>';
    if (this._ttyTabs) { this._ttyTabs.destroy(); this._ttyTabs = null; }
    wrap.innerHTML = UI.panel('串口 TTY',
      '<div id="tty-tabs"></div><div class="ui-tabpanel">' + ttyBody + '</div>',
      { cls: 'mb16',
      right: search + '<button id="tty-refresh">刷新</button> ' +
        (can ? '<button id="tty-add">手动添加</button>' : '') });
    this._ttyTabs = UI.tabs(document.getElementById('tty-tabs'), {
      items: [
        { key: 'config', label: '已配置', count: (this.ttys || []).length },
        { key: 'unconfig', label: '未配置', count: unAll.length },
      ],
      value: tab,
      onChange: (key) => {
        this.ttyTab = key; this.expandTty = null; this.addTtyPath = null; this.renderTtyCard(wrap);
      },
    });
    const qel = document.getElementById('tty-q');
    if (qel) qel.oninput = () => {
      this.ttyQuery = qel.value;
      const v = qel.value.trim().toLowerCase();
      wrap.querySelectorAll('tbody tr').forEach(tr => {
        if (tr.classList.contains('tty-expand')) return;
        tr.style.display = !v || tr.innerText.toLowerCase().indexOf(v) >= 0 ? '' : 'none';
      });
    };
    wrap.querySelectorAll('[data-trow]').forEach(tr => {
      tr.onclick = (e) => {
        if (e.target.closest('a,button,input,select,label')) return;
        const path = tr.dataset.trow;
        if (this.ttyTab === 'config') {
          this.expandTty = this.expandTty === path ? null : path;
          this.addTtyPath = null;
        } else {
          this.addTtyPath = this.addTtyPath === path ? null : path;
          this.expandTty = null;
        }
        this.renderTtyCard(wrap);
      };
    });
    wrap.querySelectorAll('[data-addpath]').forEach(b => {
      b.onclick = (e) => {
        e.stopPropagation();
        this.addTtyPath = b.dataset.addpath;
        this.expandTty = null;
        this.renderTtyCard(wrap);
      };
    });
    wrap.querySelectorAll('[data-tf-save]').forEach(b => {
      b.onclick = () => this.saveTtyForm(wrap, b.closest('.ui-form').dataset.new === '1');
    });
    wrap.querySelectorAll('[data-tf-cancel]').forEach(b => {
      b.onclick = () => { this.expandTty = null; this.addTtyPath = null; this.renderTtyCard(wrap); };
    });
    wrap.querySelectorAll('[data-tf-del]').forEach(b => {
      b.onclick = async () => {
        if (!await UI.confirm({ title: '删除串口配置', message: '确定删除 ' + b.dataset.tfDel + '？', danger: true })) return;
        try {
          await Api.del('/api/v1/agents/' + encodeURIComponent(this.devid) + '/ttys?path=' +
            encodeURIComponent(b.dataset.tfDel));
          this.expandTty = null;
          await this.loadTtys(true);
        } catch (e) { this.ttyError(e.message); }
      };
    });
    wrap.querySelectorAll('[data-tf-copy]').forEach(b => {
      b.onclick = async () => {
        try { await navigator.clipboard.writeText(b.dataset.tfCopy); UI.toast('路径已复制'); }
        catch (e) { UI.toast('复制失败', true); }
      };
    });
    wrap.querySelectorAll('[data-tf-take]').forEach(b => {
      b.onclick = async () => {
        const name = b.dataset.tfTake;
        const v = await UI.prompt({ title: '强制接管', label: '会断开当前会话，输入别名确认：' + name });
        if (v !== name) return;
        location.hash = '#/tty/' + encodeURIComponent(this.devid) + '/' +
          encodeURIComponent(name) + '?takeover=1';
      };
    });
    const rf = document.getElementById('tty-refresh');
    if (rf) rf.onclick = () => this.loadTtys(true);
    const add = document.getElementById('tty-add');
    if (add) add.onclick = () => {
      this.ttyTab = 'unconfig';
      this.addTtyPath = '';
      this.expandTty = null;
      this.renderTtyCard(wrap);
    };
  },
  ttyError(msg) {
    UI.toast(msg);
  },
});

// ---------------------------------------------------------------- Pages.客户端列表
