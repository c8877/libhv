// 设备详情页（admin.js 之后加载，共享其全局工具）
// ---------------------------------------------------------------- Pages.设备详情
const AgentDetailPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },   // 由「每 5s 刷新」勾选框控制
  enter(el, params) {
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
const ClientsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/clients');
    const rows = arr.map(c => [
      '<b>' + UI.esc(c.user) + '</b>', UI.esc(c.remote_addr),
      UI.ago(c.last_seen_ms) + ' 前活跃', c.streams,
      UI.bytes(c.bytes_in) + ' / ' + UI.bytes(c.bytes_out),
      '<button class="shell" data-user="' + UI.esc(c.user) + '">Shell</button>',
    ]);
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>客户端（A 侧）</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 个</span></div>' +
      UI.table(['用户', '远端地址', '最近活跃', '会话数', '流量 ↓/↑', '操作'], rows) + '</div>';
    document.querySelectorAll('#page button.web').forEach(btn => {
      btn.onclick = (e) => {
        e.stopPropagation();
        const port = prompt('设备 Web 端口（需在 agent --allow 列表）', '80');
        if (!port) return;
        window.open('/device/' + encodeURIComponent(btn.dataset.devid) + '/' + encodeURIComponent(port) + '/', '_blank');
      };
    });
    document.querySelectorAll('#page button.shell').forEach(btn => {
      btn.onclick = async () => {
        const user = btn.dataset.user;
        try {
          // A 侧按需 agent：devid = a:<user>，relay 会通知该 client 在本机拉起 agent
          Router.go('#/terminal/' + encodeURIComponent('a:' + user));
        } catch (e) { UI.toast('打开失败：' + e.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.会话列表
const StreamsPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/streams');
    const pill = s => s === 'OPEN' ? '<span class="pill ok">OPEN</span>'
      : s === 'OPENING' ? '<span class="pill info">OPENING</span>'
      : '<span class="pill warn">HALF_CLOSED</span>';
    const rows = arr.map(s => [
      UI.esc(s.a_user) + ':' + s.a_sid,
      UI.esc(s.c_devid) + ':' + s.c_sid,
      pill(s.state), UI.ago(s.opened_at_ms),
      '<button class="kick" data-sid="' + s.a_sid + '">踢掉</button>',
    ]);
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>会话</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 条</span></div>' +
      UI.table(['A 用户:a_sid', 'C 设备:c_sid', '状态', '开启于', '操作'], rows) + '</div>';
    document.querySelectorAll('#page button.bind').forEach(btn => {
      btn.onclick = async (e) => {
        e.stopPropagation();
        const owner = prompt('绑定到账号（留空 = 解绑）', '');
        if (owner === null) return;
        try {
          await Api.put('/api/v1/devices/' + encodeURIComponent(btn.dataset.devid) + '/owner', { username: owner });
          UI.toast(owner ? ('已绑定到 ' + owner) : '已解绑');
        } catch (err) { UI.toast('绑定失败：' + err.message, true); }
      };
    });
    document.querySelectorAll('#page button.kick').forEach(btn => {
      btn.onclick = async () => {
        if (!confirm('踢掉会话 a_sid=' + btn.dataset.sid + '？（双侧 RESET）')) return;
        try {
          await Api.del('/api/v1/streams/' + btn.dataset.sid);
          UI.toast('已踢掉 ' + btn.dataset.sid);
          this.refresh();
        } catch (err) { UI.toast('操作失败：' + err.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.证书
const CertsPage = Object.assign(new Page(), {
  // 页面有输入框（SAN/CSR），自动重渲染会清空正在输入的内容
  refreshInterval() { return 0; },
  async refresh() {
    const arr = await Api.get('/api/v1/certs');
    const pill = s => s === 'valid' ? '<span class="pill ok">有效</span>'
      : s === 'expiring' ? '<span class="pill warn">即将到期</span>'
      : s === 'expired' ? '<span class="pill bad">已过期</span>'
      : '<span class="pill bad">已吊销</span>';
    const rows = arr.map(c => [
      '<b>' + UI.esc(c.san) + '</b>', UI.esc(c.role),
      '0x' + Number(c.serial).toString(16).toUpperCase(),
      c.expire_ms ? new Date(c.expire_ms).toLocaleDateString() : '—',
      pill(c.status),
      c.status === 'revoked' ? '' : '<button class="revoke" data-san="' + UI.esc(c.san) + '">吊销</button>',
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>签发新证书</h2>' +
      '<span class="spacer"></span><button id="certs-refresh">刷新列表</button></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      'SAN <input id="issue-san" placeholder="PILE-000200" size="18">' +
      '类型 <select id="issue-role"><option value="device">设备</option><option value="user">用户</option></select>' +
      '有效期(天) <input id="issue-days" type="number" value="30" min="1" max="3650" style="width:80px">' +
      '<button class="primary" id="issue-btn">签发</button>' +
      '<span id="issue-result" style="color:var(--muted)"></span></div></div>' +
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>CSR 签名（私钥不出设备）</h2></div>' +
      '<div style="padding:16px"><textarea id="csr-input" rows="6" style="width:100%;font-family:monospace" placeholder="粘贴 CSR PEM（设备侧生成，SAN 或 CN 为身份）"></textarea>' +
      '<div style="margin-top:8px;display:flex;gap:10px;align-items:center">有效期(天) <input id="csr-days" type="number" value="30" style="width:80px">' +
      '<button id="csr-btn">签名</button><span id="csr-result" style="color:var(--muted)"></span></div></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>证书台账</h2>' +
      '<span class="spacer"></span><span>共 ' + arr.length + ' 张</span></div>' +
      UI.table(['SAN(身份)', '类型', '序列号', '到期日', '状态', '操作'], rows) + '</div>';

    const cr = document.getElementById('certs-refresh');
    if (cr) cr.onclick = () => this.refresh();
    document.getElementById('issue-btn').onclick = async () => {
      const san = document.getElementById('issue-san').value.trim();
      const role = document.getElementById('issue-role').value;
      const days = parseInt(document.getElementById('issue-days').value, 10) || 30;
      if (!san) { UI.toast('请填写 SAN', true); return; }
      try {
        const r = await Api.post('/api/v1/certs/issue', { san, role, days });
        document.getElementById('issue-result').innerHTML =
          '已签发 ' + UI.esc(r.san) + '：' +
          '<a id="dl-crt" href="#">下载 crt</a> · <a id="dl-key" href="#">下载 key（仅此一次）</a>';
        const dl = (id, name, text) => {
          const url = URL.createObjectURL(new Blob([text], { type: 'application/x-pem-file' }));
          const a = document.getElementById(id);
          a.href = url; a.download = name;
        };
        dl('dl-crt', r.san + '.crt', r.cert);
        dl('dl-key', r.san + '.key', r.key);
        UI.toast('已签发 ' + r.san);
        this.refresh();
      } catch (e) { UI.toast('签发失败：' + e.message, true); }
    };
    document.querySelectorAll('#page button.revoke').forEach(btn => {
      btn.onclick = async () => {
        if (!confirm('吊销 ' + btn.dataset.san + '？立即生效：在线连接被踢、后续 AUTH 被拒')) return;
        try {
          await Api.post('/api/v1/certs/revoke', { san: btn.dataset.san });
          UI.toast('已吊销 ' + btn.dataset.san);
          this.refresh();
        } catch (e) { UI.toast('吊销失败：' + e.message, true); }
      };
    });
    document.getElementById('csr-btn').onclick = async () => {
      const csr = document.getElementById('csr-input').value;
      const days = parseInt(document.getElementById('csr-days').value, 10) || 30;
      if (!csr.trim()) { UI.toast('请粘贴 CSR', true); return; }
      try {
        const r = await Api.post('/api/v1/certs/sign', { csr, days });
        document.getElementById('csr-result').innerHTML =
          '已签发 ' + UI.esc(r.san) + '：<a id="dl-csr-crt" href="#">下载 crt</a>';
        const url = URL.createObjectURL(new Blob([r.cert], { type: 'application/x-pem-file' }));
        const a = document.getElementById('dl-csr-crt');
        a.href = url;
        a.download = r.san + '.crt';
        UI.toast('已签发 ' + r.san);
        this.refresh();
      } catch (e) { UI.toast('CSR 签名失败：' + e.message, true); }
    };
  },
});

// ---------------------------------------------------------------- Pages.ACL
const AclPage = Object.assign(new Page(), {
  // textarea 正在编辑时不能被 5s 刷新覆盖
  refreshInterval() { return 0; },
  async refresh() {
    const d = await Api.get('/api/v1/acl/file');
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>ACL 文件（' + UI.esc(d.path || '未配置') + '）</h2>' +
      '<span class="spacer"></span>' +
      '<button id="acl-reload">重新加载</button> <button class="primary" id="acl-save">保存并热加载</button></div>' +
      '<div style="padding:16px"><textarea id="acl-content" rows="12" style="width:100%;font-family:monospace" ' +
      'placeholder="user=devid[,devid|*]，每行一条，# 注释">' + UI.esc(d.content) + '</textarea>' +
      '<div style="color:var(--muted);margin-top:8px">保存 = 写文件 + 热加载，只影响新 OPEN，不动已建立会话；解析失败保留旧表。</div></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>命令行条目（只读）</h2></div>' +
      UI.table(['条目'], (d.cli || []).map(x => [UI.esc(x)])) + '</div>';
    document.getElementById('acl-save').onclick = async () => {
      try {
        const r = await Api.put('/api/v1/acl/file', { content: document.getElementById('acl-content').value });
        UI.toast('已保存，' + r.entries + ' 条授权生效');
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
    document.getElementById('acl-reload').onclick = async () => {
      try {
        await Api.post('/api/v1/acl/reload');
        UI.toast('已重新加载');
      } catch (e) { UI.toast('重载失败：' + e.message, true); }
    };
  },
});

// ---------------------------------------------------------------- Pages.审计
const AuditPage = Object.assign(new Page(), {
  async refresh() {
    const arr = await Api.get('/api/v1/audit');
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>审计日志</h2>' +
      '<span class="spacer"></span><span>最近 ' + arr.length + ' 条（新到旧）</span></div>' +
      UI.table(['时间 / 角色 / 操作 / 详情'], arr.map(x => ['<code>' + UI.esc(x) + '</code>'])) + '</div>';
  },
});

// ---------------------------------------------------------------- Pages.文件
const FilesPage = Object.assign(new Page(), {
  // 不做 5s 自动刷新：重渲染会重建 <input type=file>，用户选好的文件会丢
  refreshInterval() { return 0; },
  enter(el, params, hash) {
    const q = new URLSearchParams((hash || '').split('?')[1] || '');
    this.sort = q.get('sort') || 'name';
    this.order = q.get('order') || 'asc';
    return Page.prototype.enter.call(this, el, params, hash);
  },
  sortLink(field, label) {
    // 首次点大小/时间按降序（大的、新的在前），名称按升序
    let order = (field === 'size' || field === 'mtime') ? 'desc' : 'asc';
    if (this.sort === field) order = this.order === 'asc' ? 'desc' : 'asc';
    const cur = this.sort === field ? (this.order === 'asc' ? ' ▲' : ' ▼') : '';
    return '<a href="#/files?sort=' + field + '&order=' + order +
      '" style="color:inherit;text-decoration:none">' + label + cur + '</a>';
  },
  async refresh() {
    const arr = await Api.get('/api/v1/files?sort=' + this.sort + '&order=' + this.order);
    const rows = arr.map(f => [
      '<b>' + UI.esc(f.name) + '</b>', UI.bytes(f.size),
      '<code title="' + UI.esc(f.sha256 || '') + '">' + UI.esc((f.sha256 || '').slice(0, 8)) + '</code>',
      new Date(f.mtime_ms).toLocaleString(),
      '<button class="dl" data-name="' + UI.esc(f.name) + '">下载</button> ' +
      (canManage() ? '<button class="push" data-name="' + UI.esc(f.name) + '">分发</button> ' : '') +
      (canManage() ? '<button class="del" data-name="' + UI.esc(f.name) + '">删除</button>' : ''),
    ]);
    this.el.innerHTML =
      '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>上传文件</h2>' +
      '<span class="spacer"></span><button id="files-refresh">刷新列表</button></div>' +
      '<div style="padding:16px;display:flex;gap:10px;align-items:center;flex-wrap:wrap">' +
      '<input type="file" id="upload-input"> <button class="primary" id="upload-btn">上传</button>' +
      '<progress id="upload-progress" max="100" value="0" style="width:220px;display:none"></progress>' +
      '<span id="upload-status" style="color:var(--muted)"></span>' +
      '<span style="color:var(--muted)">单文件上限 ' + uploadLimitMb() + ' MiB</span></div></div>' +
      '<div class="panel"><div class="panel-head"><h2>文件列表</h2><span class="spacer"></span>' +
      '<span>共 ' + arr.length + ' 个</span></div>' +
      UI.table([this.sortLink('name', '名称'), this.sortLink('size', '大小'), '哈希',
                this.sortLink('mtime', '修改时间'), '操作'], rows) + '</div>';

    document.getElementById('files-refresh').onclick = () => this.refresh();
    const MAX_UPLOAD = uploadLimitMb() * 1024 * 1024;
    const CHUNK = 8 * 1024 * 1024;
    document.getElementById('upload-btn').onclick = async () => {
      const btn = document.getElementById('upload-btn');
      const fi = document.getElementById('upload-input').files[0];
      const status = document.getElementById('upload-status');
      const bar = document.getElementById('upload-progress');
      if (!fi) { UI.toast('请选择文件', true); return; }
      if (fi.size > MAX_UPLOAD) {
        status.textContent = '文件 ' + UI.bytes(fi.size) + ' 超过 ' + uploadLimitMb() + ' MiB 上限';
        UI.toast('文件太大（' + UI.bytes(fi.size) + '），上限 ' + uploadLimitMb() + ' MiB', true);
        return;
      }
      const send = (url, blob, onprog) => new Promise((res, rej) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', url);
        xhr.setRequestHeader('X-Requested-With', 'fetch');
        xhr.upload.onprogress = (e) => { if (e.lengthComputable && onprog) onprog(e.loaded); };
        xhr.onload = () => xhr.status === 200 ? res(xhr.responseText)
          : rej(new Error('HTTP ' + xhr.status + ' ' + xhr.responseText));
        xhr.onerror = () => rej(new Error('网络错误'));
        xhr.send(blob);
      });
      bar.style.display = '';
      bar.value = 0;
      status.textContent = '上传中…';
      btn.disabled = true;
      try {
        const url = '/api/v1/files/' + encodeURIComponent(fi.name);
        const uid = Math.random().toString(36).slice(2) + Date.now().toString(36);
        if (fi.size <= CHUNK) {
          await send(url, fi, (loaded) => {
            bar.value = Math.floor(loaded * 100 / fi.size);
            status.textContent = '上传中… ' + UI.bytes(loaded) + ' / ' + UI.bytes(fi.size);
          });
        } else {
          let off = 0, tries = 0;
          while (off < fi.size) {
            const chunk = fi.slice(off, off + CHUNK);
            try {
              await send(url + '?append=1&upload=' + uid + '&offset=' + off, chunk, (loaded) => {
                const cur = off + loaded;
                bar.value = Math.floor(cur * 100 / fi.size);
                status.textContent = '上传中… ' + UI.bytes(cur) + ' / ' + UI.bytes(fi.size);
              });
              off += chunk.size;
              tries = 0;
            } catch (e) {
              if (++tries > 2) throw e;
              try {
                const st = await Api.get('/api/v1/uploads/' + encodeURIComponent(fi.name) +
                  '?upload=' + uid);
                off = st.received || off;
                status.textContent = '重试中… 已传 ' + UI.bytes(off) + ' / ' + UI.bytes(fi.size);
              } catch (e2) { throw e; }
            }
          }
          await send(url + '?finalize=1&upload=' + uid, new Blob([]));
        }
        status.textContent = '已上传 ' + fi.name + '（' + UI.bytes(fi.size) + '）';
        this.refresh();
      } catch (e) {
        status.textContent = '';
        UI.toast('上传失败：' + e.message, true);
      } finally {
        btn.disabled = false;
        bar.style.display = 'none';
      }
    };
    document.querySelectorAll('#page button.push').forEach(btn => {
      btn.onclick = () => Router.go('#/distribute?file=' + encodeURIComponent(btn.dataset.name));
    });
    document.querySelectorAll('#page button.dl').forEach(btn => {
      btn.onclick = async () => {
        try {
          const headers = {};
          const r = await fetch('/api/v1/files/' + encodeURIComponent(btn.dataset.name), { headers, cache: 'no-store' });
          if (!r.ok) throw new Error('HTTP ' + r.status);
          const blob = await r.blob();
          const url = URL.createObjectURL(blob);
          const a = document.createElement('a');
          a.href = url;
          a.download = btn.dataset.name;
          a.click();
          URL.revokeObjectURL(url);
        } catch (e) { UI.toast('下载失败：' + e.message, true); }
      };
    });
    document.querySelectorAll('#page button.del').forEach(btn => {
      btn.onclick = async () => {
        if (!confirm('删除 ' + btn.dataset.name + '？')) return;
        try {
          await Api.del('/api/v1/files/' + encodeURIComponent(btn.dataset.name));
          UI.toast('已删除 ' + btn.dataset.name);
          this.refresh();
        } catch (e) { UI.toast('删除失败：' + e.message, true); }
      };
    });
  },
});

// ---------------------------------------------------------------- Pages.终端（xterm.js + WebSocket）
// 终端会话管理器：切页面不断开，支持多个 tab
const TermMgr = {
  sessions: new Map(),
  seq: 0,
  open(devid) {
    const id = devid + '#' + (++this.seq);
    const box = document.createElement('div');
    box.style.cssText = 'height:100%;width:100%';
    const term = new Terminal({ cursorBlink: true, fontSize: parseInt((Auth.profile && Auth.profile.term_font) || 14, 10) });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(proto + '://' + location.host + '/ws/shell?devid=' + encodeURIComponent(devid));
    const s = { id, devid, term, fit, ws, box, opened: false, alive: true };
    ws.onmessage = e => term.write(e.data);
    ws.onclose = () => { s.alive = false; try { term.write('\r\n[连接已关闭]\r\n'); } catch (e) {} };
    term.onData(d => { if (ws.readyState === 1) ws.send(d); });
    term.onResize(({ cols, rows }) => {
      if (ws.readyState === 1) ws.send(JSON.stringify({ type: 'resize', cols, rows }));
    });
    this.sessions.set(id, s);
    return s;
  },
  aliveOf(devid) {
    let found = null;
    for (const s of this.sessions.values()) if (s.devid === devid && s.alive) found = s;
    return found;
  },
  close(id) {
    const s = this.sessions.get(id);
    if (!s) return;
    s.alive = false;
    try { s.ws.close(); } catch (e) {}
    try { s.term.dispose(); } catch (e) {}
    s.box.remove();
    this.sessions.delete(id);
  },
};

const TerminalPage = Object.assign(new Page(), {
  enter(el, params) {
    this.el = el;
    const devid = decodeURIComponent((params[0] || '').split('?')[0]);
    if (devid) {
      let target = TermMgr.aliveOf(devid);
      if (!target) target = TermMgr.open(devid);
      this.active = target.id;
    }
    this.render();
    this._rs = () => this.fitActive();
    window.addEventListener('resize', this._rs);
  },
  fitActive() {
    const s = TermMgr.sessions.get(this.active);
    if (!s) return;
    try { s.fit.fit(); } catch (e) {}
  },
  render() {
    const ids = [...TermMgr.sessions.keys()];
    if (!ids.length) { Router.go('#/agents'); return; }
    if (!TermMgr.sessions.has(this.active)) this.active = ids[ids.length - 1];
    const cur = TermMgr.sessions.get(this.active);
    const tabs = ids.map(id => {
      const s = TermMgr.sessions.get(id);
      return '<button class="term-tab' + (id === this.active ? ' active' : '') + '" data-id="' + id + '">' +
        UI.esc(s.devid) + (s.alive ? '' : ' ✕') + '</button>';
    }).join('');
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>终端</h2>' +
      '<span class="spacer"></span><span class="term-tabs">' + tabs + '</span> ' +
      '<button id="term-new">新开</button> <button id="term-close">关闭当前</button></div>' +
      '<div id="term-host" style="height:calc(100vh - 220px);background:#000;padding:8px"></div></div>';
    const host = document.getElementById('term-host');
    if (!cur.opened) { cur.term.open(cur.box); cur.opened = true; }
    host.appendChild(cur.box);
    document.querySelectorAll('#page button.term-tab').forEach(btn => {
      btn.onclick = () => { this.active = btn.dataset.id; this.render(); };
    });
    document.getElementById('term-new').onclick = () => {
      const d = prompt('设备 devid', cur.devid);
      if (!d) return;
      this.active = TermMgr.open(d.trim()).id;
      this.render();
    };
    document.getElementById('term-close').onclick = () => {
      TermMgr.close(this.active);
      this.active = null;
      this.render();
    };
    setTimeout(() => this.fitActive(), 0);
  },
  leave() {
    if (this._rs) { window.removeEventListener('resize', this._rs); this._rs = null; }
    const s = TermMgr.sessions.get(this.active);
    if (s && s.box.parentNode) s.box.parentNode.removeChild(s.box);   // 仅摘 DOM，会话保留
  },
});


// ---------------------------------------------------------------- Pages.系统配置
const SettingsPage = Object.assign(new Page(), {
  refreshInterval() { return 0; },
  async refresh() {
    const st = await Api.get('/api/v1/settings');
    const schema = st._schema || [];
    const groups = [];
    schema.forEach(d => { if (groups.indexOf(d.group) < 0) groups.push(d.group); });

    let html = '';
    groups.forEach(g => {
      const rows = schema.filter(d => d.group === g).map(d => {
        const isText = (d.min === 0 && d.max === 0);
        const input = isText
          ? '<input type="text" data-k="' + d.key + '" value="' + UI.esc(st[d.key] || '') + '">'
          : '<input type="number" data-k="' + d.key + '" value="' + UI.esc(st[d.key] || '') +
            '" min="' + d.min + '" max="' + d.max + '">';
        return '<label class="cfg-row"><span>' + UI.esc(d.label) + '</span>' + input +
          (d.unit ? '<em>' + UI.esc(d.unit) + '</em>' : '') + '</label>';
      }).join('');
      html += '<div class="panel" style="margin-bottom:16px"><div class="panel-head"><h2>' +
        UI.esc(g) + '</h2></div><div class="cfg-body">' + rows + '</div></div>';
    });
    html += '<div class="panel"><div class="cfg-actions" style="padding-top:16px">' +
      '<button class="primary" id="cfg-save">保存</button></div></div>';
    this.el.innerHTML = html;

    document.getElementById('cfg-save').onclick = async () => {
      const body = {};
      document.querySelectorAll('#page .cfg-body [data-k]').forEach(inp => { body[inp.dataset.k] = inp.value; });
      try {
        await Api.put('/api/v1/settings', body);
        try {
          window.SETTINGS = await Api.get('/api/v1/settings');
          applySiteTitle();
        } catch (e) {}
        UI.toast('已保存，立即生效');
        this.refresh();
      } catch (e) { UI.toast('保存失败：' + e.message, true); }
    };
  },
  enter(el) { this.el = el; return Page.prototype.enter.call(this, el, []); },
});


Router.register(/^#\/agents\/([^/]+)$/, AgentDetailPage, '设备详情');
