/* rtunnel 存储目录树组件（本地 Web 与 relay-admin 共用）
 *
 *   var tree = RtunnelStorage.createTree({
 *     container: el,                                     // 挂载容器
 *     fetchNodes: function (mount, path, depth) { ... }  // 返回 STORAGE 响应
 *   });
 *   tree.setMounts(sys.mounts);   // [{mount,fstype,used_kb,total_kb,avail_kb}]
 *   tree.destroy();
 *
 * 数据按需加载：分区根只取一级子项，展开目录时再按路径请求它的直接子项；
 * 右键「全展开」才按 depth=32 拉整棵子树。已取回的节点缓存 5 分钟。
 * 样式全部挂在 .rtstore 下，注入一次到 document.head，不与页面其它样式冲突。
 */
(function (global) {
    'use strict';

    var STORE_TTL = 5 * 60 * 1000;
    var MAX_KIDS = 10;
    var FETCH_DEPTH_ALL = 32;

    var CSS = ''
        + '.rtstore{position:relative;min-width:0;color:var(--rt-fg,#1f2329);font-size:12px}'
        + '.rtstore *{box-sizing:border-box}'
        + '.rtstore .store-list{display:flex;flex-direction:column}'
        + '.rtstore .store{border-bottom:1px solid var(--rt-line,#e4e8f0)}'
        + '.rtstore .store:last-child{border-bottom:0}'
        + '.rtstore .store-head{display:flex;align-items:center;gap:10px;padding:10px 6px;border-radius:6px;cursor:pointer}'
        + '.rtstore .store-head:hover{background:var(--rt-hover,#f5f7fa)}'
        + '.rtstore .store-head:focus-visible{outline:2px solid var(--rt-accent,#3b6ef5);outline-offset:-2px}'
        + '.rtstore .caret{flex:none;width:12px;font-size:11px;color:var(--rt-muted,#7b8494)}'
        + '.rtstore .store-head .mount{flex:none;min-width:88px;font-family:ui-monospace,Menlo,monospace;font-size:13px;font-weight:600}'
        + '.rtstore .fs{flex:none;font-size:11px;color:var(--rt-muted,#7b8494);background:var(--rt-fill,#f0f2f5);border-radius:4px;padding:0 6px}'
        + '.rtstore .store-head .sizes{flex:none;min-width:118px;font-family:ui-monospace,Menlo,monospace;color:var(--rt-muted,#7b8494);font-variant-numeric:tabular-nums}'
        + '.rtstore .store-head .pct{flex:none;width:44px;text-align:right;color:var(--rt-muted,#7b8494);font-variant-numeric:tabular-nums}'
        + '.rtstore .store-head .pct.warn{color:#c98a12}'
        + '.rtstore .store-head .pct.bad{color:#d9534f}'
        + '.rtstore .store-head .dirs{flex:none;min-width:58px;text-align:right;color:var(--rt-muted,#7b8494)}'
        + '.rtstore .store-bar,.rtstore .st-bar{flex:1 1 140px;min-width:50px;height:6px;border-radius:3px;background:var(--rt-fill,#f0f2f5);overflow:hidden}'
        + '.rtstore .st-bar{flex-basis:140px;height:6px}'
        + '.rtstore .store-bar i,.rtstore .st-bar i{display:block;height:100%;border-radius:3px;background:var(--rt-accent,#3b6ef5)}'
        + '.rtstore .store-bar i.warn{background:#f5a623}'
        + '.rtstore .store-bar i.bad{background:#e5534b}'
        + '.rtstore .store-body{padding:0 6px 14px}'
        + '.rtstore .store-tools{display:flex;align-items:center;gap:10px;flex-wrap:wrap;color:var(--rt-muted,#7b8494);margin-bottom:6px}'
        + '.rtstore .store-actions{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-left:auto}'
        + '.rtstore .store-loading{display:flex;flex-direction:column;gap:6px;padding:10px 0 6px;color:var(--rt-muted,#7b8494)}'
        + '.rtstore .store-loading .track{height:3px;border-radius:2px;background:var(--rt-fill,#f0f2f5);overflow:hidden}'
        + '.rtstore .store-loading .track i{display:block;height:100%;width:100%;background:var(--rt-accent,#3b6ef5);opacity:.35}'
        + '.rtstore .st-row{display:flex;align-items:center;gap:8px;height:30px;padding-right:4px;border-radius:6px}'
        + '.rtstore .st-row:hover{background:var(--rt-hover,#f5f7fa)}'
        + '.rtstore .st-row .nm{flex:0 1 240px;min-width:60px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-family:ui-monospace,Menlo,monospace}'
        + '.rtstore .st-row .sz{flex:none;width:76px;text-align:right;color:var(--rt-muted,#7b8494);font-family:ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums}'
        + '.rtstore .st-row .pc{flex:none;width:46px;text-align:right;color:var(--rt-muted,#7b8494);font-family:ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums}'
        + '.rtstore .st-row.file .nm{color:var(--rt-fg,#1f2329)}'
        + '.rtstore .st-row.small .nm,.rtstore .st-row.loading .nm{color:var(--rt-muted,#7b8494)}'
        + '.rtstore .st-row.loading .nm{font-style:italic}'
        + '.rtstore .st-row.other .nm{font-style:italic}'
        + '.rtstore .st-row.foot{height:26px}'
        + '.rtstore .st-row.foot .nm{color:var(--rt-muted,#7b8494);font-family:inherit}'
        + '.rtstore .st-bar i.free{background:var(--rt-fill-dark,#dfe3ea)}'
        + '.rtstore .st-bar i.reserved{background:#c8cdd6}'
        + '.rtstore .st-bar i.muted-fill{background:var(--rt-muted,#7b8494);opacity:.45}'
        + '.rtstore .st-foot{margin-top:8px;padding-top:6px;border-top:1px dashed var(--rt-line,#e4e8f0)}'
        + '.rtstore .st-note{margin-top:6px;font-size:11px;line-height:16px;color:var(--rt-muted,#7b8494)}'
        + '.rtstore .st-warn{margin-top:8px;padding:7px 10px;border-radius:8px;line-height:17px}'
        + '.rtstore .st-warn.warn{background:rgba(224,138,46,.12);color:#b8730f}'
        + '.rtstore .st-warn.info{background:var(--rt-hover,#f5f7fa);color:var(--rt-muted,#7b8494);word-break:break-all}'
        + '.rtstore .st-error{margin:8px 0;padding:7px 10px;border-radius:8px;background:rgba(229,83,75,.1);color:#c0392b}'
        + '.rtstore .empty{padding:16px 4px;color:var(--rt-muted,#7b8494)}'
        + '.rtstore .store-eye{display:inline-flex;align-items:center;justify-content:center;width:24px;height:24px;padding:0;border:0;border-radius:6px;background:transparent;color:var(--rt-muted,#7b8494);cursor:pointer}'
        + '.rtstore .store-eye:hover{background:var(--rt-hover,#f5f7fa);color:var(--rt-fg,#1f2329)}'
        + '.rtstore .store-eye svg{display:block}'
        + '.rtstore .link{border:0;background:transparent;color:var(--rt-accent,#3b6ef5);font:inherit;padding:0;cursor:pointer}'
        + '.rtstore .link:hover{text-decoration:underline}'
        + '.rtstore .store-menu{position:fixed;z-index:3000;min-width:120px;padding:4px;background:var(--rt-bg,#fff);border:1px solid var(--rt-line,#e4e8f0);border-radius:8px;box-shadow:0 8px 24px rgba(0,0,0,.18)}'
        + '.rtstore .store-menu button{display:block;width:100%;padding:7px 12px;border:0;border-radius:6px;background:transparent;color:var(--rt-fg,#1f2329);font:inherit;font-size:13px;text-align:left;cursor:pointer}'
        + '.rtstore .store-menu button:hover{background:var(--rt-hover,#f5f7fa)}'
        + '@media (max-width:599px){'
        + '.rtstore .store-head{flex-wrap:wrap;row-gap:5px;padding:9px 4px}'
        + '.rtstore .store-head .mount{flex:1 1 auto;min-width:0}'
        + '.rtstore .store-head .sizes{min-width:0}'
        + '.rtstore .store-head .store-bar{order:5;flex-basis:100%}'
        + '.rtstore .store-head .dirs{display:none}'
        + '.rtstore .store-tools{gap:6px}'
        + '.rtstore .st-row{gap:6px}'
        + '.rtstore .st-row .nm{flex-basis:132px}'
        + '.rtstore .st-row .sz{width:64px}'
        + '.rtstore .st-row .pc{width:40px}'
        + '}';

    var ICON_EYE = '<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor"'
        + ' stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">'
        + '<path d="M2 12s3.6-6 10-6 10 6 10 6-3.6 6-10 6-10-6-10-6z"/><circle cx="12" cy="12" r="2.6"/></svg>';
    var ICON_EYE_OFF = ICON_EYE.replace('</svg>', '<path d="M3 3l18 18"/></svg>');

    var cssInjected = false;
    function injectCss() {
        if (cssInjected) return;
        cssInjected = true;
        var st = document.createElement('style');
        st.setAttribute('data-rtunnel', 'storage');
        st.textContent = CSS;
        document.head.appendChild(st);
    }

    function num(v) { var n = Number(v); return isFinite(n) ? n : 0; }

    // 与管理台 bytes() 一致：<10 两位小数、<100 一位
    function fmtBytes(v) {
        var n = num(v);
        var units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
        var i = 0;
        while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
        var digits = i === 0 ? 0 : n >= 100 ? 0 : n >= 10 ? 1 : 2;
        return n.toFixed(digits) + ' ' + units[i];
    }

    // 磁盘用量按总量选 K/M/G，两个数值共用单位
    function kbPair(usedKb, totalKb) {
        var k = num(totalKb), unit = 'K', div = 1;
        if (k >= 100 * 1024) { unit = 'G'; div = 1024 * 1024; }
        else if (k >= 1024) { unit = 'M'; div = 1024; }
        function f(v) {
            var n = num(v) / div;
            return Math.abs(n) >= 10 ? n.toFixed(1) : n.toFixed(2);
        }
        return { used: f(usedKb), total: f(totalKb), unit: unit };
    }

    function pctOf(b, total) {
        var p = num(b) * 100 / Math.max(1, total);
        return p >= 10 ? Math.round(p) : p >= 1 ? Number(p.toFixed(1)) : p > 0 ? 0.1 : 0;
    }
    function pctText(b, total) {
        var p = num(b) * 100 / Math.max(1, total);
        if (p <= 0) return '0%';
        if (p < 1) return '<1%';
        return p >= 10 ? Math.round(p) + '%' : p.toFixed(1) + '%';
    }

    function tone(p) { return p >= 85 ? 'bad' : p >= 70 ? 'warn' : ''; }

    function esc(s) {
        return String(s === undefined || s === null ? '' : s).replace(/[&<>"']/g, function (c) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
        });
    }

    function baseName(p) {
        var i = p.lastIndexOf('/');
        return i < 0 ? p : p.slice(i + 1);
    }
    function parentOf(p) {
        var i = p.lastIndexOf('/');
        return i <= 0 ? '/' : p.slice(0, i);
    }
    function isHiddenPath(p) { return baseName(p).charAt(0) === '.'; }

    function truncText(reason) {
        return {
            entries: '达到目录项上限',
            time: '达到时间上限',
            depth: '超过统计深度',
            nodes: '目录节点过多，只保留前 5000 个',
        }[reason] || '超出扫描上限';
    }

    function Tree(opts) {
        this.opts = opts || {};
        this.container = this.opts.container;
        this.fetchNodes = this.opts.fetchNodes;
        this.mounts = [];
        this.sig = '';
        this.open = '';
        this.cache = {};
        this.expanded = {};
        this.showAll = {};
        this.showHidden = true;
        this.menu = { show: false, x: 0, y: 0, path: '' };
        this.timer = null;
        this.seq = 0;
        this.loading = false;
        this.elapsed = 0;
        this.error = '';
        this.root = null;
        this.onDocClick = this.closeMenu.bind(this);
        this.onScroll = this.closeMenu.bind(this);
        this.onResize = this.closeMenu.bind(this);
        this.onKey = this.onKeydown.bind(this);
        this._build();
    }

    Tree.prototype._build = function () {
        injectCss();
        var root = document.createElement('div');
        root.className = 'rtstore';
        root.innerHTML = '<div class="store-list"></div>'
            + '<div class="store-menu" role="menu" hidden>'
            + '<button type="button" role="menuitem" data-act="expand">全展开</button>'
            + '<button type="button" role="menuitem" data-act="collapse">全折叠</button>'
            + '</div>';
        this.container.appendChild(root);
        this.root = root;
        this.list = root.querySelector('.store-list');
        this.menuEl = root.querySelector('.store-menu');

        var self = this;
        this.list.addEventListener('click', function (e) { self.onClick(e); });
        this.list.addEventListener('contextmenu', function (e) { self.onContextMenu(e); });
        this.list.addEventListener('keydown', function (e) {
            if (e.key !== 'Enter') return;
            var head = e.target.closest('.store-head');
            if (head) { e.preventDefault(); self.toggleMount(head.getAttribute('data-mount')); }
        });
        this.menuEl.addEventListener('click', function (e) {
            var b = e.target.closest('button[data-act]');
            if (!b) return;
            e.stopPropagation();
            self.applyMenu(b.getAttribute('data-act') === 'expand');
        });
        document.addEventListener('click', this.onDocClick);
        window.addEventListener('scroll', this.onScroll, true);
        window.addEventListener('resize', this.onResize);
        document.addEventListener('keydown', this.onKey);
    };

    Tree.prototype.destroy = function () {
        clearInterval(this.timer);
        this.timer = null;
        document.removeEventListener('click', this.onDocClick);
        window.removeEventListener('scroll', this.onScroll, true);
        window.removeEventListener('resize', this.onResize);
        document.removeEventListener('keydown', this.onKey);
        if (this.root && this.root.parentNode) this.root.parentNode.removeChild(this.root);
        this.root = null;
        this.cache = {};
    };

    Tree.prototype.setMounts = function (list) {
        this.mounts = (list || []).slice().sort(function (a, b) {
            return diskPct(b) - diskPct(a);
        });
        var sig = this.mounts.map(function (m) {
            return m.mount + ':' + num(m.used_kb) + ':' + num(m.total_kb);
        }).join('|');
        if (sig === this.sig) return;
        this.sig = sig;
        // 挂载点消失时收起，避免留下空白区域
        if (this.open && !this.mounts.some(function (m) { return m.mount === this.open; }, this)) {
            this.open = '';
        }
        this.render();
    };

    function diskPct(m) {
        var total = num(m.total_kb);
        return total ? Math.round(num(m.used_kb) * 100 / total) : 0;
    }

    Tree.prototype.cur = function () {
        return this.open ? this.cache[this.open] || null : null;
    };

    Tree.prototype.render = function () {
        if (!this.root) return;
        var self = this;
        if (!this.mounts.length) {
            this.list.innerHTML = '<div class="empty">' + esc(this.opts.emptyText || '无挂载点') + '</div>';
            return;
        }
        this.list.innerHTML = this.mounts.map(function (m) {
            var p = diskPct(m), cls = tone(p), open = self.open === m.mount;
            var pair = kbPair(m.used_kb, m.total_kb);
            var c = self.cache[m.mount];
            return '<div class="store">'
                + '<div class="store-head" role="button" tabindex="0" data-mount="' + esc(m.mount) + '">'
                + '<span class="caret">' + (open ? '▾' : '▸') + '</span>'
                + '<span class="mount">' + esc(m.mount) + '</span>'
                + (m.fstype ? '<span class="fs">' + esc(m.fstype) + '</span>' : '')
                + '<span class="sizes">' + pair.used + ' / ' + pair.total + ' ' + pair.unit + '</span>'
                + '<span class="store-bar"><i class="' + cls + '" style="width:' + p + '%"></i></span>'
                + '<span class="pct ' + cls + '">' + p + '%</span>'
                + '<span class="dirs">' + (c && c.info ? num(c.info.dirs) + ' 目录' : '') + '</span>'
                + '</div>'
                + (open ? '<div class="store-body">' + self.bodyHtml(m.mount) + '</div>' : '')
                + '</div>';
        }).join('');
    };

    Tree.prototype.bodyHtml = function (mount) {
        var c = this.cache[mount];
        if (this.loading && !c) {
            return '<div class="store-loading"><span>正在扫描 ' + esc(mount) + ' … ' + this.elapsed + 's</span>'
                + '<div class="track"><i></i></div></div>';
        }
        if (!c) return this.error ? '<div class="st-error">' + esc(this.error) + '</div>' : '';
        var info = c.info;
        var total = Math.max(1, num(info.total_kb) * 1024);
        var html = '<div class="store-tools"><span>条长 = 占分区容量 · 缩进 = 层级</span>'
            + '<div class="store-actions">'
            + '<span>' + num(info.files) + ' 个文件 · 扫描 ' + (num(info.scan_ms) / 1000).toFixed(1) + 's</span>'
            + '<button class="store-eye" type="button" data-toggle-hidden="1" title="'
            + (this.showHidden ? '隐藏隐藏项' : '显示隐藏项') + '" aria-label="'
            + (this.showHidden ? '隐藏隐藏项' : '显示隐藏项') + '">'
            + (this.showHidden ? ICON_EYE : ICON_EYE_OFF) + '</button>'
            + '<button class="link" type="button" data-rescan="' + esc(mount) + '">重新扫描</button>'
            + '</div></div>';
        if (c.error) html += '<div class="st-error">' + esc(c.error) + '</div>';
        html += this.rowsHtml(c).map(function (r) { return rowHtml(r, total); }).join('');

        html += '<div class="st-foot">';
        [['已用', info.used_kb, 'used'], ['可用', info.free_kb, 'free'], ['保留', info.reserved_kb, 'reserved']]
            .forEach(function (r) {
                var p = Math.min(100, Math.max(0, Math.round(num(r[1]) * 1024 * 100 / total)));
                html += '<div class="st-row foot"><span class="nm">' + r[0] + '</span>'
                    + '<span class="sz">' + fmtBytes(num(r[1]) * 1024) + '</span>'
                    + '<span class="st-bar"><i class="' + r[2] + '" style="width:' + p + '%"></i></span>'
                    + '<span class="pc">' + p + '%</span></div>';
            });
        html += '</div>';
        html += '<div class="st-note">统计合计 ' + fmtBytes(info.bytes) + ' · df 已用 '
            + fmtBytes(num(info.used_kb) * 1024)
            + ' · 差额来自文件系统元数据、保留块、已删除未释放的文件和无权限目录</div>';
        if (info.truncated) {
            html += '<div class="st-warn warn">结果可能偏低：' + esc(truncText(info.truncated_reason)) + '</div>';
        }
        if (info.skipped && info.skipped.length) {
            html += '<div class="st-warn info">已跳过 ' + info.skipped.length + ' 个无权限目录：'
                + esc(info.skipped.slice(0, 3).join('  '))
                + (info.skipped.length > 3 ? '  等 ' + info.skipped.length + ' 个' : '') + '</div>';
        }
        return html;
    };

    function rowHtml(r, total) {
        if (r.kind === 'loading') {
            return '<div class="st-row loading" data-kind="loading" style="padding-left:'
                + (6 + r.depth * 14) + 'px"><span class="caret"></span>'
                + '<span class="nm">' + esc(r.name) + '</span></div>';
        }
        var cls = 'st-row';
        if (r.other) cls += ' other';
        if (r.kind === 'file') cls += ' file';
        if (r.kind === 'small') cls += ' small';
        return '<div class="' + cls + '" data-path="' + esc(r.path) + '" data-kind="' + r.kind + '"'
            + (r.other ? ' data-other="1"' : '')
            + (r.toggle && !r.other ? ' data-kids="1"' : '')
            + ' style="padding-left:' + (6 + r.depth * 14) + 'px">'
            + '<span class="caret">' + (r.toggle ? (r.open ? '▾' : '▸') : '') + '</span>'
            + '<span class="nm" title="' + esc(r.title || r.path) + '">' + esc(r.name) + '</span>'
            + '<span class="sz">' + fmtBytes(r.b) + '</span>'
            + '<span class="st-bar"><i class="' + (r.other ? 'muted-fill' : '')
            + '" style="width:' + pctOf(r.b, total) + '%"></i></span>'
            + '<span class="pc">' + pctText(r.b, total) + '</span></div>';
    }

    // 子项分「目录 / 文件」两组，各自最多 10 项，其余合并成「其他…」。
    // 返回 null 表示该目录的子项还没取过。
    Tree.prototype.kids = function (c, path) {
        var node = c.map[path];
        if (!node) return [];
        if (!node.kidsLoaded) return null;
        var list = node.children;
        if (!this.showHidden) list = list.filter(function (k) { return !isHiddenPath(k.p); });
        var groups = { dir: [], file: [] };
        list.forEach(function (k) {
            (k.t === 'f' || k.t === 'o' ? groups.file : groups.dir).push(k);
        });
        var out = [];
        var showAll = this.showAll[path];
        ['dir', 'file'].forEach(function (kind) {
            var arr = groups[kind];
            if (!arr.length) return;
            arr.sort(function (a, b) { return b.b - a.b; });
            if (showAll) { out = out.concat(arr); return; }
            var threshold = Math.max(node.b * 0.01, 1), keep = [], rest = [];
            arr.forEach(function (k, i) {
                if (i < MAX_KIDS && (k.b >= threshold || i < 3)) keep.push(k);
                else rest.push(k);
            });
            out = out.concat(keep);
            if (rest.length) {
                out.push({
                    other: true, group: kind, count: rest.length,
                    b: rest.reduce(function (a, x) { return a + num(x.b); }, 0),
                });
            }
        });
        return out;
    };

    Tree.prototype.rowsHtml = function (c) {
        var self = this;
        var rows = [];

        function base(node, depth) { return { b: node.b, depth: depth }; }

        function leaf(node, depth) {
            var isSmall = node.t === 'o';
            var r = {
                path: node.p,
                kind: isSmall ? 'small' : 'file',
                name: isSmall ? '小文件 ' + num(node.n) + ' 项' : baseName(node.p),
                title: isSmall ? parentOf(node.p) : node.p,
                toggle: false, open: false,
            };
            return Object.assign(r, base(node, depth));
        }

        function other(parent, kid, depth) {
            var r = {
                path: parent, kind: 'other', other: true,
                name: (kid.group === 'file' ? '其他文件 ' : '其他目录 ') + kid.count + ' 项',
                toggle: true, open: false,
            };
            return Object.assign(r, base(kid, depth));
        }

        function emit(kids, depth, parent) {
            kids.forEach(function (kid) {
                if (kid.other) rows.push(other(parent, kid, depth));
                else if (kid.t === 'f' || kid.t === 'o') rows.push(leaf(kid, depth));
                else walk(kid.p, depth);
            });
        }

        function walk(path, depth) {
            var node = c.map[path];
            if (!node) return;
            var kids = self.kids(c, path);
            var open = !!self.expanded[path];
            var hasKids = kids === null ? !!node.c : kids.length > 0;
            rows.push(Object.assign({
                path: path, kind: 'dir', title: path,
                name: path === c.info.mount ? path : baseName(path),
                toggle: hasKids, open: open,
            }, base(node, depth)));
            if (!open) return;
            if (kids === null) {
                rows.push({ path: '', kind: 'loading', name: '加载中…', depth: depth + 1, toggle: false, open: false, b: 0 });
                return;
            }
            emit(kids, depth + 1, path);
        }

        // 分区本身已由头部展示，正文从一级子项开始
        var rootKids = this.kids(c, c.info.mount);
        if (rootKids) emit(rootKids, 0, c.info.mount);
        return rows;
    };

    // 把一次响应合并进缓存；rootPath 的子项以本次结果为准
    Tree.prototype.merge = function (c, r, rootPath, depth) {
        var nodes = (r && r.nodes) || [];
        nodes.forEach(function (n) {
            if (!c.map[n.p]) c.map[n.p] = { p: n.p, b: n.b, n: n.n, d: n.d, m: n.m, t: n.t, c: n.c, children: [], kidsLoaded: false };
            else {
                var cur = c.map[n.p];
                cur.b = n.b; cur.n = n.n; cur.d = n.d; cur.m = n.m; cur.t = n.t; cur.c = n.c;
                if (!cur.children) cur.children = [];
            }
        });
        if (c.map[rootPath]) {
            c.map[rootPath].children = [];
            c.map[rootPath].kidsLoaded = true;
        }
        var touched = {};
        nodes.forEach(function (n) {
            if (n.p === rootPath) return;
            var parent = parentOf(n.p);
            if (!c.map[parent]) return;
            if (depth > 1 && !touched[parent]) {
                touched[parent] = true;
                c.map[parent].children = [];
            }
            c.map[parent].children.push(c.map[n.p]);
        });
        // 整棵子树一次取回时中间目录的子项也齐了；被截断时只认根
        if (depth > 1 && r && !r.truncated) {
            nodes.forEach(function (n) {
                if (n.t === 'd' && n.p !== rootPath && c.map[n.p]) c.map[n.p].kidsLoaded = true;
            });
        }
    };

    Tree.prototype.toggleMount = function (mount) {
        if (this.open === mount) {
            this.open = '';
            this.render();
            return;
        }
        this.open = mount;
        this.error = '';
        var c = this.cache[mount];
        if (c && Date.now() - c.ts < STORE_TTL) {
            this.render();
            return;
        }
        this.render();
        this.loadRoot(mount, false);
    };

    Tree.prototype.loadRoot = function (mount, force) {
        var self = this;
        var c = this.cache[mount];
        if (!force && c && Date.now() - c.ts < STORE_TTL) return;
        var seq = ++this.seq;
        this.loading = true;
        this.error = '';
        this.elapsed = 0;
        var t0 = Date.now();
        clearInterval(this.timer);
        this.timer = setInterval(function () {
            self.elapsed = Math.round((Date.now() - t0) / 1000);
            self.render();
        }, 250);
        this.render();
        Promise.resolve(this.fetchNodes(mount, '', 1))
            .then(function (r) {
                if (seq !== self.seq) return;
                if (!r || !r.ok) throw new Error((r && r.error) || 'scan failed');
                var cur = { info: r, map: {}, ts: Date.now(), loading: {}, error: '' };
                self.merge(cur, r, mount, 1);
                self.cache[mount] = cur;
            })
            .catch(function (e) {
                if (seq === self.seq) self.error = '扫描失败：' + (e && e.message ? e.message : e);
            })
            .then(function () {
                if (seq !== self.seq) return;
                clearInterval(self.timer);
                self.timer = null;
                self.loading = false;
                self.render();
            });
    };

    // 取某个目录的直接子项（depth=1）或整棵子树（depth>1）
    Tree.prototype.loadKids = function (mount, path, depth) {
        var self = this;
        var c = this.cache[mount];
        if (!c) return Promise.resolve(false);
        if (c.loading[path]) return Promise.resolve(false);
        c.loading[path] = true;
        return Promise.resolve(this.fetchNodes(mount, path, depth))
            .then(function (r) {
                if (!r || !r.ok) throw new Error((r && r.error) || 'scan failed');
                self.merge(c, r, path, depth);
                c.error = '';
                return true;
            })
            .catch(function (e) {
                delete self.expanded[path];
                c.error = '加载 ' + path + ' 失败：' + (e && e.message ? e.message : e);
                return false;
            })
            .then(function (ok) {
                c.loading[path] = false;
                self.render();
                return ok;
            });
    };

    Tree.prototype.onClick = function (e) {
        var rescan = e.target.closest('[data-rescan]');
        if (rescan) { this.loadRoot(rescan.getAttribute('data-rescan'), true); return; }
        if (e.target.closest('[data-toggle-hidden]')) {
            this.showHidden = !this.showHidden;
            this.render();
            return;
        }
        var head = e.target.closest('.store-head');
        if (head) { this.toggleMount(head.getAttribute('data-mount')); return; }

        var row = e.target.closest('.st-row');
        if (!row) return;
        var kind = row.getAttribute('data-kind');
        if (kind === 'loading') return;
        var path = row.getAttribute('data-path');
        if (row.getAttribute('data-other')) {
            this.showAll[path] = true;
            this.render();
            return;
        }
        if (!row.getAttribute('data-kids')) return;
        if (this.expanded[path]) {
            delete this.expanded[path];
            this.render();
            return;
        }
        this.expanded[path] = true;
        var c = this.cache[this.open];
        var node = c && c.map[path];
        this.render();
        if (node && !node.kidsLoaded) this.loadKids(this.open, path, 1);
    };

    Tree.prototype.onContextMenu = function (e) {
        var row = e.target.closest('.st-row');
        if (!row) return;
        var kind = row.getAttribute('data-kind');
        if (kind === 'file' || kind === 'small' || kind === 'loading') return;
        var path = row.getAttribute('data-path');
        var c = this.cache[this.open];
        if (!path || !c || !c.map[path]) return;
        e.preventDefault();
        var w = 132, h = 84;
        this.menu.path = path;
        this.menu.x = Math.max(8, Math.min(e.clientX, window.innerWidth - w - 8));
        this.menu.y = Math.max(8, Math.min(e.clientY, window.innerHeight - h - 8));
        this.menu.show = true;
        this.menuEl.hidden = false;
        this.menuEl.style.left = this.menu.x + 'px';
        this.menuEl.style.top = this.menu.y + 'px';
    };

    Tree.prototype.closeMenu = function () {
        if (!this.menu.show) return;
        this.menu.show = false;
        this.menu.path = '';
        if (this.menuEl) this.menuEl.hidden = true;
    };

    Tree.prototype.onKeydown = function (e) {
        if (e.key === 'Escape') this.closeMenu();
    };

    // 递归展开整棵子树：先按需把子树取回来，再逐层标记
    Tree.prototype.applyMenu = function (open) {
        var self = this;
        var path = this.menu.path;
        this.closeMenu();
        var c = this.cache[this.open];
        if (!path || !c) return;

        function mark() {
            (function walk(p) {
                var node = c.map[p];
                if (!node) return;
                if (open) {
                    // 子树被截断时有些后代还没取过，不能标成展开（否则一直显示加载中）
                    if (node.kidsLoaded) {
                        self.expanded[p] = true;
                        self.showAll[p] = true;
                    }
                } else {
                    delete self.expanded[p];
                    delete self.showAll[p];
                }
                node.children.forEach(function (k) { walk(k.p); });
            })(path);
            self.render();
        }

        if (!open) {
            mark();
            return;
        }
        // 全展开需要整棵子树：即使当前层已加载，后代也可能没取过
        this.expanded[path] = true;
        this.render();
        this.loadKids(this.open, path, FETCH_DEPTH_ALL).then(function (ok) {
            if (ok) mark();
        });
    };

    global.RtunnelStorage = {
        createTree: function (opts) { return new Tree(opts); },
    };
})(window);
