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
  // 统一 Tab 组件。variant: underline(默认) / segmented / minimal / attached
  // opts: { items:[{key,label,count,disabled}], value, variant, onChange(key) }
  // 返回: { el, getValue, setValue, setCount, setItems, destroy }
  tabs(container, opts) {
    if (!container) return null;
    opts = opts || {};
    let items = (opts.items || []).slice();
    let value = opts.value != null ? opts.value : (items[0] && items[0].key);
    const variant = opts.variant || 'underline';
    const onChange = typeof opts.onChange === 'function' ? opts.onChange : function () {};
    container.classList.add('ui-tabs', 'ui-tabs-' + variant);
    container.setAttribute('role', 'tablist');

    const current = () => items.find(x => x.key === value) || items[0];
    const updateInk = () => {
      if (variant !== 'underline') return;
      const ink = container.querySelector('.ui-tab-ink');
      const active = container.querySelector('.ui-tab.is-active');
      if (!ink || !active) return;
      ink.style.width = active.offsetWidth + 'px';
      ink.style.transform = 'translateX(' + active.offsetLeft + 'px)';
      ink.style.opacity = '1';
    };
    const activate = (key, fire) => {
      if (!items.some(x => x.key === key)) return;
      value = key;
      container.querySelectorAll('.ui-tab').forEach(b => {
        const on = b.dataset.key === key;
        b.classList.toggle('is-active', on);
        b.setAttribute('aria-selected', on ? 'true' : 'false');
        b.tabIndex = on ? 0 : -1;
      });
      updateInk();
      if (fire) onChange(key);
    };
    const bind = () => {
      container.querySelectorAll('.ui-tab').forEach(b => {
        b.onclick = () => activate(b.dataset.key, true);
        b.onkeydown = (e) => {
          const idx = items.findIndex(x => x.key === b.dataset.key);
          let next = -1;
          if (e.key === 'ArrowRight') next = (idx + 1) % items.length;
          else if (e.key === 'ArrowLeft') next = (idx - 1 + items.length) % items.length;
          else if (e.key === 'Home') next = 0;
          else if (e.key === 'End') next = items.length - 1;
          if (next >= 0) {
            e.preventDefault();
            activate(items[next].key, true);
            const el = container.querySelector('.ui-tab[data-key="' + items[next].key + '"]');
            if (el) el.focus();
          }
        };
      });
    };
    const render = () => {
      container.innerHTML = items.map(x =>
        '<button class="ui-tab' + (x.key === value ? ' is-active' : '') + '" role="tab" ' +
        'data-key="' + UI.esc(x.key) + '" aria-selected="' + (x.key === value ? 'true' : 'false') + '" ' +
        'tabindex="' + (x.key === value ? '0' : '-1') + '"' + (x.disabled ? ' disabled' : '') + '>' +
        '<span class="ui-tab-label">' + UI.esc(x.label || x.key) + '</span>' +
        (x.count != null ? '<span class="ui-tab-count">' + Number(x.count) + '</span>' : '') +
        '</button>').join('') +
        (variant === 'underline' ? '<span class="ui-tab-ink"></span>' : '');
      bind();
      updateInk();
    };
    const onResize = () => updateInk();
    if (variant === 'underline') window.addEventListener('resize', onResize);
    render();
    return {
      el: container,
      getValue: () => value,
      setValue: (key, fire) => activate(key, fire !== false),
      setCount(key, count) {
        const it = items.find(x => x.key === key);
        if (!it) return;
        it.count = count;
        const b = container.querySelector('.ui-tab[data-key="' + key + '"] .ui-tab-count');
        if (b) b.textContent = Number(count);
        else render();
      },
      setItems(next) {
        items = (next || []).slice();
        if (!items.some(x => x.key === value)) value = items[0] && items[0].key;
        render();
      },
      destroy() {
        if (variant === 'underline') window.removeEventListener('resize', onResize);
        container.innerHTML = '';
      },
    };
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
      { cls: opts.cls || '', left: opts.summary ? '<span class="muted ml10">' +
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
    let closed = false;
    const close = () => {
      if (closed) return;
      closed = true;
      mask.remove();
      if (opts.onClose) opts.onClose();
    };
    mask.querySelectorAll('[data-modal-close]').forEach(b => { b.onclick = close; });
    mask.onclick = e => { if (e.target === mask) close(); };
    return { el: mask, close };
  },
  confirm(opts) {
    opts = opts || {};
    return new Promise(resolve => {
      let done = false;
      const m = UI.modal({
        title: opts.title || '确认',
        body: '<div class="ui-confirm-text">' + (opts.message || '') + '</div>',
        buttons: '<button data-modal-close>取消</button>' +
          '<button class="' + (opts.danger ? 'danger' : 'primary') + '" data-ok>确认</button>',
        onClose: () => { if (!done) { done = true; resolve(false); } },
      });
      const ok = m.el.querySelector('[data-ok]');
      if (ok) ok.onclick = () => { done = true; m.close(); resolve(true); };
    });
  },
  prompt(opts) {
    opts = opts || {};
    return new Promise(resolve => {
      let done = false;
      const m = UI.modal({
        title: opts.title || '输入',
        body: '<label>' + (opts.label || '值') + '<input id="ui-prompt-input" type="' +
          (opts.type || 'text') + '" value="' + UI.esc(opts.value || '') + '" placeholder="' +
          UI.esc(opts.placeholder || '') + '"></label>',
        buttons: '<button data-modal-close>取消</button><button class="primary" data-ok>确定</button>',
        onClose: () => { if (!done) { done = true; resolve(null); } },
      });
      const input = m.el.querySelector('#ui-prompt-input');
      const ok = m.el.querySelector('[data-ok]');
      const submit = () => { done = true; const v = input.value; m.close(); resolve(v); };
      if (ok) ok.onclick = submit;
      if (input) {
        input.focus();
        input.onkeydown = e => { if (e.key === 'Enter') submit(); };
      }
    });
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
    const cell = r => r.raw ? r.raw : '<label>' + r.label + r.control + '</label>';
    if (opts.layout === 'grid') {
      body = '<div class="ui-form-grid ' + (opts.cols ? 'cols-' + opts.cols : '') + '">' +
        rows.map(cell).join('') + '</div>';
    } else {
      body = '<div class="form-rows">' + rows.map(r =>
        '<label class="form-row"><span class="k">' + r.label + '</span>' +
        '<span class="c">' + r.control + '</span><span class="u">' + (r.unit || '') + '</span></label>').join('') + '</div>';
    }
    return '<div class="ui-form ' + (opts.cls || '') + '"' + (opts.id ? ' id="' + UI.esc(opts.id) + '"' : '') +
      (opts.attrs ? ' ' + opts.attrs : '') + '>' + body +
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
