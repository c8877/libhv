'use strict';
/* 统一渲染组件：表格 / KV / 表单 / 面板 / 格式化 */
const UI = {
  esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g,
      c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  },
  renderUser() {
    const me = (typeof Auth !== 'undefined' && Auth.me) || {};
    const name = me.display_name || me.username || '?';
    const color = me.avatar_color || avatarColor(me.username);
    const btn = document.getElementById('userbtn');
    if (btn) {
      btn.innerHTML = '<span class="avatar" style="background:' + color + '">' +
        UI.esc(String(name).slice(0, 1).toUpperCase()) + '</span><span class="uname">' +
        UI.esc(name) + '</span><span class="caret">▾</span>';
    }
    const info = document.getElementById('userinfo');
    if (info) info.textContent = name + ' · ' + (ROLE_NAMES[me.role] || me.role || '');
  },
  bytes(n) {
    n = Number(n) || 0;
    const u = ['B', 'K', 'M', 'G', 'T'];
    let i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return n.toFixed(i ? 1 : 0) + u[i];
  },
  ago(ms) {
    if (!ms) return '—';
    const s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    if (s < 86400) return Math.floor(s / 3600) + 'h';
    return Math.floor(s / 86400) + 'd';
  },
  duration(ms) {
    const s = Math.floor((Number(ms) || 0) / 1000);
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    return (d ? d + 'd ' : '') + String(h).padStart(2, '0') + ':' + String(m).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
  },
  panel(title, body, opts) {
    opts = opts || {};
    return '<div class="panel ' + (opts.cls || '') + '">' +
      '<div class="panel-head"><h2>' + title + '</h2>' +
      (opts.left || '') + '<span class="spacer"></span>' + (opts.right || '') +
      '</div>' + body + '</div>';
  },
  grid(rows) {
    return '<div class="sys-grid">' + rows.map(r =>
      '<div class="sys-item"><span class="k">' + r[0] + '</span>' +
      '<span class="v" title="' + r[1] + '">' + r[1] + '</span></div>').join('') + '</div>';
  },
  listPage(opts) {
    opts = opts || {};
    const search = opts.search
      ? '<input id="' + UI.esc(opts.search.id || '') + '" class="ui-search" placeholder="' +
        UI.esc(opts.search.placeholder || '搜索') + '">'
      : '';
    return UI.panel(opts.title, UI.detailTable(opts.columns || [], opts.rows || [], opts.tableOpts),
      { cls: opts.cls || '', left: opts.summary ? '<span class="muted" style="margin-left:10px">' +
          opts.summary + '</span>' : '', right: search + (opts.actions || '') });
  },
  bindSearch(input, scope) {
    if (!input) return;
    input.oninput = () => {
      const q = input.value.trim().toLowerCase();
      const rows = (scope || document).querySelectorAll('tbody tr');
      rows.forEach(tr => {
        tr.style.display = !q || tr.innerText.toLowerCase().indexOf(q) >= 0 ? '' : 'none';
      });
    };
  },
  pager(page, pages, total, right) {
    return '<div class="ui-pager">' +
      '<button data-pager="prev"' + (page <= 1 ? ' disabled' : '') + '>上一页</button>' +
      '<span>' + page + ' / ' + pages + (total != null ? '（共 ' + total + '）' : '') + '</span>' +
      '<button data-pager="next"' + (page >= pages ? ' disabled' : '') + '>下一页</button>' +
      '<span class="spacer"></span>' + (right || '') + '</div>';
  },
  bindPager(root, onPrev, onNext) {
    const p = root.querySelector('[data-pager="prev"]');
    const n = root.querySelector('[data-pager="next"]');
    if (p) p.onclick = onPrev;
    if (n) n.onclick = onNext;
  },
  modal(opts) {
    opts = opts || {};
    const mask = document.createElement('div');
    mask.className = 'tty-mask';
    mask.innerHTML = '<div class="tty-modal"><h2>' + (opts.title || '') + '</h2>' + (opts.body || '') +
      '<div class="tty-btns">' + (opts.buttons || '') + '</div></div>';
    document.body.appendChild(mask);
    const close = () => mask.remove();
    mask.querySelectorAll('[data-modal-close]').forEach(b => { b.onclick = close; });
    mask.onclick = e => { if (e.target === mask) close(); };
    return { el: mask, close };
  },
  formValues(root) {
    const out = {};
    (root || document).querySelectorAll('[data-field]').forEach(el => {
      out[el.dataset.field] = el.type === 'checkbox' ? el.checked : el.value;
    });
    return out;
  },
  detailTable(cols, rows, opts) {
    opts = opts || {};
    const th = c => '<th>' + (String(c).charAt(0) === '<' ? c : UI.esc(c)) + '</th>';
    let h = '<div class="detail-table-shell ' + (opts.shellCls || '') + '"><table class="detail-table ' + (opts.cls || '') + '"><thead><tr>' +
      cols.map(th).join('') + '</tr></thead><tbody>';
    if (!rows.length) h += '<tr><td class="empty" colspan="' + cols.length + '">' + UI.esc(opts.empty || '暂无数据') + '</td></tr>';
    rows.forEach((r, i) => {
      h += '<tr' + (opts.rowAttrs ? opts.rowAttrs(i) : '') + '>' + r.map(c => '<td>' + c + '</td>').join('') + '</tr>';
    });
    return h + '</tbody></table></div>';
  },
  kvTable(rows, opts) {
    opts = opts || {};
    const head = opts.header ? '<thead><tr>' + opts.header.map(h => '<th>' + h + '</th>').join('') + '</tr></thead>' : '';
    return '<div class="detail-table-shell ' + (opts.shellCls || '') + '">' +
      '<table class="detail-table detail-kv ' + (opts.cls || '') + '">' + head + '<tbody>' +
      rows.map(r => '<tr><th>' + r[0] + '</th><td>' + r[1] + '</td></tr>').join('') +
      '</tbody></table></div>';
  },
  form(opts) {
    opts = opts || {};
    const rows = opts.rows || [];
    let body;
    if (opts.layout === 'grid') {
      body = '<div class="ui-form-grid ' + (opts.cols ? 'cols-' + opts.cols : '') + '">' +
        rows.map(r => '<label>' + r.label + r.control + '</label>').join('') + '</div>';
    } else {
      body = '<div class="form-rows">' + rows.map(r =>
        '<label class="form-row"><span class="k">' + r.label + '</span>' +
        '<span class="c">' + r.control + '</span><span class="u">' + (r.unit || '') + '</span></label>').join('') + '</div>';
    }
    return '<div class="ui-form ' + (opts.cls || '') + '"' + (opts.id ? ' id="' + UI.esc(opts.id) + '"' : '') + '>' + body +
      '<div class="ui-form-foot">' + (opts.left || '') +
      '<span class="spacer"></span>' + (opts.right || '') + '</div></div>';
  },
  table(cols, rows, onrow) {
    const html = UI.detailTable(cols, rows, { rowAttrs: i => ' data-i="' + i + '"' });
    if (onrow) {
      setTimeout(() => {
        document.querySelectorAll('#page tbody tr').forEach(tr => {
          tr.onclick = () => onrow(rows[tr.dataset.i]);
        });
      });
    }
    return html;
  },
  kv(obj) {
    let h = '<dl class="kv">';
    for (const k in obj) h += '<div><dt>' + UI.esc(k) + '</dt><dd>' + obj[k] + '</dd></div>';
    return h + '</dl>';
  },
  toast(msg, err) {
    const box = document.getElementById('toast');
    const el = document.createElement('div');
    el.className = 'item' + (err ? ' err' : '');
    el.textContent = msg;
    box.appendChild(el);
    setTimeout(() => el.remove(), 4000);
  },
};
