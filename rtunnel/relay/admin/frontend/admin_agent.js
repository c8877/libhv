// 设备详情页（admin.js 之后加载，共享其全局工具）
// ---------------------------------------------------------------- Pages.设备详情
const AgentDetailPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },   // 由「每 5s 刷新」勾选框控制
  async enter(el, params) {
    this.el = el;
    el.innerHTML = '<div class="panel"><div class="empty">图表组件加载中…</div></div>';
    try { await loadScriptOnce('/vendor/chart.umd.min.js'); } catch (e) {}
    this.devid = decodeURIComponent(params[0] || '');
    this.cpuHist = [];
    this.memHist = [];
    this.charts = [];
    this.period = Number((window.SETTINGS || {})['ui.refresh_seconds'] || 0);
    this.auto = this.period > 0;
    const p = Page.prototype.enter.call(this, el, params);
    if (this.auto && !this.timer) {
      this.timer = setInterval(() => this.refresh().catch(() => {}), this.period * 1000);
    }
    return p;
  },
  leave() {
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
    if (this._retry) { clearTimeout(this._retry); this._retry = null; }
  },
  spark(id, data, color) {
    const c = document.getElementById(id);
    if (!c || !window.Chart) return;
    this.charts.push(new Chart(c.getContext('2d'), {
      type: 'line',
      data: { labels: data.map((_, i) => i),
        datasets: [{ data: data, borderColor: color, backgroundColor: color + '26',
          fill: true, pointRadius: 0, borderWidth: 2, tension: .3 }] },
      options: { animation: false, responsive: true,
        scales: { x: { display: false }, y: { display: false, min: 0, max: 100 } },
        plugins: { legend: { display: false }, tooltip: { enabled: false } } }
    }));
  },
  diskColor(p) { return p >= 85 ? cssVar('--bad', '#e5534b') : p >= 70 ? cssVar('--warn', '#f5a623') : cssVar('--accent', '#4a9eff'); },
  async refresh() {
    const d = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid));
    let si = null, siTs = 0;
    try {
      const r = await Api.get('/api/v1/agents/' + encodeURIComponent(this.devid) + '/sysinfo?refresh=1');
      si = r.sysinfo; siTs = r.ts;
    } catch (e) {}

    const rows = (d.streams || []).map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid, s.c_sid,
      s.opened ? '<span class="pill ok">OPEN</span>' : '<span class="pill info">OPENING</span>',
      UI.ago(s.opened_at_ms),
    ]);

    let html = '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>' + UI.esc(d.devid) + '</h2>' +
      '<span class="pill ok">在线</span><span class="spacer"></span>' +
      (si ? '<label class="si-auto"><input type="checkbox" id="si-auto"' + (this.auto ? ' checked' : '') +
            '> 每 5s 刷新</label> <button id="si-export">导出 JSON</button>' : '') + '</div>' + UI.kv({
        '远端地址': UI.esc(d.remote_addr),
        '接入时间': new Date(d.connected_at_ms).toLocaleString(),
        '最近活跃': UI.ago(d.last_seen_ms) + ' 前',
        '流量 ↓/↑': UI.bytes(d.bytes_in) + ' / ' + UI.bytes(d.bytes_out),
      }) + '</div>';

    if (si) {
      this.charts.forEach(c => { try { c.destroy(); } catch (e) {} });
      this.charts = [];
      const kb = n => UI.bytes((Number(n) || 0) * 1024);
      const total = Number(si.mem_total_kb) || 0;
      const avail = Number(si.mem_available_kb) || 0;
      const memPct = total > 0 ? Math.round((total - avail) * 100 / total) : 0;
      const cpu = si.cpu || { usage: 0, cores: [] };
      this.memHist.push(memPct); if (this.memHist.length > 60) this.memHist.shift();
      this.cpuHist.push(cpu.usage || 0); if (this.cpuHist.length > 60) this.cpuHist.shift();

      const mounts = si.mounts || [];
      const maxTotal = mounts.reduce((m, x) => Math.max(m, Number(x.total_kb) || 0), 1);
      const labels = mounts.map(m => m.mount);
      const usedData = mounts.map(m => (Number(m.total_kb) || 0) - (Number(m.free_kb) || 0));
      const freeData = mounts.map(m => Number(m.free_kb) || 0);
      const usedColors = mounts.map(m => {
        const t = Number(m.total_kb) || 0;
        return this.diskColor(t > 0 ? Math.round((t - (Number(m.free_kb) || 0)) * 100 / t) : 0);
      });
      const diskRows = (si.mounts || []).map(m => {
        const t = Number(m.total_kb) || 0, f = Number(m.free_kb) || 0;
        return { mount: m.mount, fstype: m.fstype || '', total: t, used: t - f, free: f,
                 pct: t > 0 ? Math.round((t - f) * 100 / t) : 0 };
      });
      this.diskRows = diskRows;
      const nics = (si.nics || []).map(n => [UI.esc(n.name), UI.esc(n.ip), UI.esc(n.netmask),
        n.up ? '<span class="pill ok">up</span>' : '<span class="pill bad">down</span>']);
      const nicPanel = '<div class="panel si-nic"><div class="panel-head"><h2>网卡</h2></div>' +
        UI.table(['接口', 'IP', '掩码', '状态'], nics) + '</div>';

      html += '<div class="si-grid">' +
        '<div class="panel si-mem"><div class="panel-head"><h2>内存</h2>' +
        '<span class="spacer"></span><span class="si-num">' + memPct + '%</span></div>' +
        '<div class="si-body"><canvas id="si-mem" height="56"></canvas>' +
        '<div class="si-legend">已用 ' + kb(total - avail) + ' / ' + kb(total) +
        ' · Swap ' + kb(si.swap_total_kb) + '</div></div></div>' +
        '<div class="panel si-disk"><div class="panel-head"><h2>存储</h2>' +
        '<span class="spacer"></span><span style="color:var(--muted);font-size:12px">条长按最大分区归一</span></div>' +
        '<div class="si-body"><canvas id="si-disk" height="' +
          Math.max(60, (si.mounts || []).length * 24 + 16) + '"></canvas></div></div>' +
        '<div class="panel si-cpu"><div class="panel-head"><h2>CPU</h2>' +
        '<span class="spacer"></span><span class="si-num">' + ((cpu.cores || []).length ? (cpu.usage || 0).toFixed(0) + '%' : '—') + '</span></div>' +
        '<div class="si-body"><canvas id="si-cpu" height="56"></canvas>' +
        '<div class="si-legend">load ' + (si.load1 || 0).toFixed(2) + ' / ' +
        (si.load5 || 0).toFixed(2) + ' / ' + (si.load15 || 0).toFixed(2) +
        ' · ' + (cpu.cores || []).length + ' 核</div></div></div>' +
        nicPanel +
        '</div>';

      const certText = si.mtls
        ? UI.kv({ '证书主体': UI.esc(si.cert_subject || ''), '签发者': UI.esc(si.cert_issuer || ''),
                  '序列号': UI.esc(String(si.cert_serial || '')), '状态': si.cert_valid ? '有效' : '已过期' })
        : '<div style="padding:12px;color:var(--muted)">未启用 mTLS</div>';
      const caps = si.caps || {};
      const chips = [];
      (caps.ports || []).forEach(p => chips.push('TCP ' + p));
      if (caps.shell) chips.push('Shell');
      if (caps.sysinfo) chips.push('SYSINFO');
      if (caps.file_dir) chips.push('文件 ' + caps.file_dir);
      chips.push(caps.mtls ? 'mTLS' : 'mTLS 未启用');
      const capHtml = chips.map(c => '<span class="chip">' + UI.esc(c) + '</span>').join(' ');
      const runtimeTable = '<table class="si-table">' +
        '<tr><th>agent 版本</th><td>' + UI.esc(si.version || '') + '</td></tr>' +
        '<tr><th>能力</th><td>' + capHtml +
        (canManage() ? ' <button class="caps-edit" id="caps-edit">编辑</button>' : '') + '</td></tr>' +
        '<tr><th>启动参数</th><td><code style="font-size:12px">' + UI.esc(si.args || '') + '</code></td></tr>' +
        '</table>';
      const sysRows = [
        ['架构', si.machine || '—'],
        ['内核', si.kernel || '—'],
        ['CPU 型号', si.cpu_model || '—'],
        ['核心数', ((cpu.cores || []).length) + ' 核'],
        ['板卡型号', si.board || '—'],
        ['采集时间', new Date(siTs).toLocaleString()],
      ];
      const sysTable = '<table class="si-table">' + sysRows.map(r =>
        '<tr><th>' + UI.esc(r[0]) + '</th><td>' + UI.esc(r[1]) + '</td></tr>').join('') + '</table>';
      html += '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>系统</h2></div>' + sysTable + '</div>' +
        '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>运行时</h2></div>' + runtimeTable + '</div>' +
        '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>证书</h2></div>' + certText + '</div>';
    }

    html += '<div class="panel"><div class="panel-head"><h2>当前会话</h2></div>' +
      UI.table(['A 用户:sid', 'c_sid', '状态', '开启于'], rows) + '</div>';
    this.el.innerHTML = html;

    if (si) {
      const self = this;
      this.spark('si-mem', this.memHist, cssVar('--accent', '#4a9eff'));
      this.spark('si-cpu', this.cpuHist, cssVar('--warn', '#f5a623'));
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
              while (nm.length > 1 && 6 + ctx2.measureText(nm).width + 6 + bw > maxRight) {
                nm = nm.slice(0, -1);
              }
              if (nm !== row.mount) nm = nm.replace(/.$/, '…');
              ctx2.fillStyle = cssVar('--chart-axis', '#8b929c');
              ctx2.fillText(nm, 6, y0);
              const nmW = ctx2.measureText(nm).width;
              const bx = 6 + nmW + 6;
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
        this.charts.push(new Chart(c.getContext('2d'), {
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
        }));
      }
      const ce = document.getElementById('caps-edit');
      if (ce) ce.onclick = () => {
        const caps = si.caps || {};
        const shellAllowed = roleOf() === 'admin';
        const ov = document.createElement('div');
        ov.className = 'modal-mask';
        ov.innerHTML = '<div class="modal"><h3>编辑能力</h3>' +
          '<label class="fld">转发端口（逗号分隔，1–65535，最多 32 个）' +
          '<input id="caps-ports" value="' + UI.esc((caps.ports || []).join(', ')) + '"></label>' +
          '<label class="fld">文件目录（绝对路径）' +
          '<input id="caps-dir" value="' + UI.esc(caps.file_dir || '/opt/data/files') + '"></label>' +
          '<label class="fld"><input type="checkbox" id="caps-shell"' + (caps.shell ? ' checked' : '') +
          (shellAllowed ? '' : ' disabled') + '> 允许 Shell' +
          (shellAllowed ? '' : '（仅 admin 可改）') + '</label>' +
          '<div class="modal-actions"><button id="caps-cancel">取消</button>' +
          '<button class="primary" id="caps-save">保存并下发</button></div>' +
          '<div id="caps-msg" class="modal-msg"></div></div>';
        document.body.appendChild(ov);
        const close = () => ov.remove();
        document.getElementById('caps-cancel').onclick = close;
        document.getElementById('caps-save').onclick = async () => {
          const msg = document.getElementById('caps-msg');
          const raw = document.getElementById('caps-ports').value;
          const ports = raw.split(',').map(x => x.trim()).filter(Boolean).map(x => Number(x));
          if (ports.some(x => !Number.isInteger(x) || x <= 0 || x > 65535)) {
            msg.textContent = '端口必须是 1–65535 的整数'; return;
          }
          const body = { ports: ports };
          const fdir = document.getElementById('caps-dir').value.trim();
          if (fdir) body.file_dir = fdir;
          if (shellAllowed) body.shell = document.getElementById('caps-shell').checked;
          msg.textContent = '下发中…';
          try {
            const r = await Api.put('/api/v1/agents/' + encodeURIComponent(this.devid) + '/caps', body);
            close();
            if (r && r.persisted === false) UI.toast('已生效，但未能写入配置文件，重启后会丢失', true);
            else UI.toast('能力已更新');
            this.refresh();
          } catch (e) { msg.textContent = '失败：' + e.message; }
        };
      };
      const cb = document.getElementById('si-auto');
      if (cb) cb.onchange = () => {
        this.auto = cb.checked;
        if (this.auto) {
          const sec = this.period > 0 ? this.period : 5;
          this.timer = setInterval(() => this.refresh().catch(() => {}), sec * 1000);
          this.refresh().catch(() => {});
        } else if (this.timer) {
          clearInterval(this.timer);
          this.timer = null;
        }
      };
      if (si && (!si.cpu || !(si.cpu.cores || []).length) && !this._retry) {
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
});

// ---------------------------------------------------------------- Pages.客户端列表
