/* rtunnel 服务管理组件（本地 Web 与 relay-admin 共用）
 *
 *   const sm = RtunnelSvc.createManager({ wsUrl, token, title });
 *   sm.open();
 *
 * 协议：文本帧 JSON，op = hello/welcome/list/logs/action/error。
 */
(function (global) {
    'use strict';

    var FILE_STATE = {
        enabled: '已启用', 'enabled-runtime': '已启用(临时)',
        disabled: '已禁用', static: '静态', masked: '已屏蔽', 'masked-runtime': '已屏蔽(临时)',
        indirect: '间接', generated: '生成', alias: '别名', linked: '链接',
        transient: '临时', bad: '异常'
    };

    function h(tag, cls, text) {
        var e = document.createElement(tag);
        if (cls) e.className = cls;
        if (text !== undefined && text !== null) e.textContent = text;
        return e;
    }

    function fmtMem(n) {
        if (n === null || n === undefined || n < 0) return '—';
        if (n < 1024) return n + ' B';
        var u = ['KB', 'MB', 'GB', 'TB'], v = n / 1024, i = 0;
        while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
        return (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)) + ' ' + u[i];
    }

    function fmtDur(ms) {
        if (ms === null || ms === undefined || ms < 0) return '';
        var s = Math.floor(ms / 1000);
        if (s < 60) return s + ' 秒';
        var m = Math.floor(s / 60), hr = Math.floor(m / 60), d = Math.floor(hr / 24);
        if (d > 0) return d + ' 天 ' + (hr % 24) + ' 小时';
        if (hr > 0) return hr + ' 小时 ' + (m % 60) + ' 分';
        return m + ' 分';
    }

    // 窄屏隐藏「运行时长」列，改用状态格里的紧凑写法（54天18时）
    function fmtDurShort(ms) {
        if (ms === null || ms === undefined || ms < 0) return '';
        var s = Math.floor(ms / 1000);
        if (s < 60) return s + '秒';
        var m = Math.floor(s / 60), hr = Math.floor(m / 60), d = Math.floor(hr / 24);
        if (d > 0) return d + '天' + (hr % 24) + '时';
        if (hr > 0) return hr + '时' + (m % 60) + '分';
        return m + '分';
    }

    function statusOf(s) {
        var active = s.active || '';
        if (active === 'failed') {
            var why = s.result && s.result !== 'success' ? s.result : '';
            if (s.exit) why += (why ? ' ' : '') + 'exit ' + s.exit;
            return { text: '失败', cls: 'bad', detail: why || '进程异常退出' };
        }
        if (active === 'active') {
            return {
                text: '运行中', cls: 'ok',
                detail: s.pid ? 'PID ' + s.pid : '',
                up: s.active_ms >= 0 ? fmtDur(s.active_ms) : ''
            };
        }
        if (active === 'activating') return { text: '启动中', cls: 'warn', detail: '' };
        if (active === 'deactivating') return { text: '停止中', cls: 'warn', detail: '' };
        if (active === 'reloading') return { text: '重载中', cls: 'warn', detail: '' };
        if (s.file_state === 'masked') return { text: '已屏蔽', cls: 'warn', detail: '解除屏蔽后才能启动' };
        return { text: '已停止', cls: 'idle', detail: s.file_state === 'static' ? 'static' : '' };
    }

    function canAct(s, verb, writable) {
        if (!writable) return false;
        var active = s.active || '', fs = s.file_state || '';
        if (verb === 'start') return active !== 'active' && fs !== 'masked';
        if (verb === 'stop') return active === 'active' || active === 'activating';
        if (verb === 'restart') return active === 'active';
        if (verb === 'enable') return fs !== 'enabled' && fs !== 'static' && fs !== 'masked';
        if (verb === 'disable') return fs === 'enabled' || fs === 'indirect';
        if (verb === 'mask') return fs !== 'masked';
        if (verb === 'unmask') return fs === 'masked';
        return false;
    }

    var CSS = [
        ':host{all:initial;display:block;',
        '--panel:#fff;--fg:#1f2633;--muted:#7b8494;--line:#e4e8f0;--soft:#f4f6fa;--brand:#3b6ef5;',
        '--ok:#2e9e63;--warn:#c98a18;--bad:#d9534f;--idle:#9aa4b2;',
        '--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}',
        ':host([hidden]),.win[hidden]{display:none}',
        '@media (prefers-color-scheme:dark){:host{--panel:#171c26;--fg:#e6ebf5;--muted:#8f99ab;',
        '--line:#252c3a;--soft:#222938;--idle:#6b7484}}',
        '*{box-sizing:border-box}',
        '.head[hidden],.tag[hidden],.pnums[hidden]{display:none}',
        '.win{position:relative;background:var(--panel);color:var(--fg);border:1px solid var(--line);border-radius:12px;',
        'font:13px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;overflow:hidden}',
        '.head{display:flex;align-items:center;gap:10px;padding:13px 15px;border-bottom:1px solid var(--line)}',
        '.head .t{font-weight:650;font-size:14px}',
        '.head .sum{color:var(--muted);font-size:12px;margin-left:auto}',
        '.x{border:0;background:transparent;color:var(--muted);font-size:20px;cursor:pointer;padding:0 4px;border-radius:6px}',
        '.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:11px 15px;border-bottom:1px solid var(--line);background:var(--soft)}',
        '.bar input{height:30px;min-width:160px;flex:1;max-width:300px;border:1px solid var(--line);border-radius:8px;',
        'background:var(--panel);color:var(--fg);padding:0 10px;font:inherit;outline:none}',
        '.bar input:focus{border-color:var(--brand)}',
        '.tabs{display:flex;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:var(--panel)}',
        '.tabs button{border:0;background:transparent;color:var(--muted);padding:6px 10px;font:inherit;cursor:pointer}',
        '.tabs button+button{border-left:1px solid var(--line)}',
        '.tabs button.on{background:rgba(59,110,245,.1);color:var(--brand);font-weight:600}',
        '.bar .sp{flex:1}',
        '.bar .tabs,.bar .tag,.bar .btn{flex:none}',
        '.tag{font-size:11px;padding:3px 8px;border-radius:999px;background:var(--soft);color:var(--muted);white-space:nowrap}',
        '.tag.rw{background:rgba(46,158,99,.12);color:var(--ok)}',
        '.btn{height:30px;border:1px solid var(--line);background:var(--panel);color:var(--fg);border-radius:8px;',
        'padding:0 10px;font:inherit;cursor:pointer;white-space:nowrap}',
        '.btn:hover:not(:disabled){border-color:var(--brand);color:var(--brand)}',
        '.btn:disabled{opacity:.45;cursor:default}',
        '.btn.pri{background:var(--brand);border-color:var(--brand);color:#fff}',
        '.btn.pri:hover:not(:disabled){color:#fff;filter:brightness(1.05)}',
        '.btn.danger{color:var(--bad);border-color:rgba(217,83,79,.4)}',
        '.err{margin:10px 15px 0;padding:8px 10px;border-radius:8px;background:rgba(217,83,79,.1);color:var(--bad);font-size:12px}',
        '.tw{padding:0 15px 12px;overflow-x:auto}',
        'table{width:100%;min-width:300px;border-collapse:collapse;table-layout:fixed}',
        'th{color:var(--muted);font-weight:500;font-size:11px;text-align:left;padding:9px 8px;border-bottom:1px solid var(--line);white-space:nowrap}',
        'td{padding:9px 8px;border-bottom:1px solid var(--line);vertical-align:middle}',
        'tr.row:last-child td{border-bottom:0}',
        'th.op,td.op{text-align:right;width:224px}',
        'th.c-start,td.c-start{width:88px}',
        'th.c-mem,td.c-mem{width:92px;text-align:right;font-variant-numeric:tabular-nums}',
        'th.c-up,td.c-up{width:104px;white-space:nowrap}',
        'th.st,td.st{width:206px}',
        'th.sortable{cursor:pointer;user-select:none}',
        'th.sortable:hover{color:var(--fg)}',
        '.arw{font-size:9px;color:var(--brand);margin-left:3px}',
        '.name{display:flex;align-items:center;gap:7px;font-family:var(--mono);font-size:12.5px;overflow-wrap:anywhere}',
        '.name span{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
        '.desc{color:var(--muted);font-size:11.5px;margin-top:2px;padding-left:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
        '.led{width:7px;height:7px;border-radius:50%;background:var(--idle);flex:none}',
        '.led.ok{background:var(--ok)}.led.bad{background:var(--bad)}.led.warn{background:var(--warn)}',
        '.badge{display:inline-flex;align-items:center;gap:5px;font-size:11.5px;padding:2px 8px;border-radius:999px;background:var(--soft);color:var(--muted);white-space:nowrap}',
        '.badge::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}',
        '.badge.ok{background:rgba(46,158,99,.12);color:var(--ok)}',
        '.badge.bad{background:rgba(217,83,79,.12);color:var(--bad)}',
        '.badge.warn{background:rgba(201,138,24,.13);color:var(--warn)}',
        '.detail{color:var(--muted);font-size:11px;margin-top:3px;padding-left:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}',
        '.up-inline{display:none}',
        '.acts{display:flex;gap:6px;justify-content:flex-end}',
        '.more{width:30px;padding:0;font-size:16px;line-height:1}',
        '.meta{color:var(--muted);font-size:11.5px;display:flex;gap:14px;flex-wrap:wrap;margin-bottom:8px}',
        '.meta b{color:var(--fg);font-weight:500}',
        'pre.logs{margin:0;max-height:260px;overflow:auto;background:#11151c;color:#d6dde8;border-radius:8px;',
        'padding:10px 12px;font:11.5px/1.55 var(--mono);white-space:pre-wrap;overflow-wrap:anywhere}',
        'pre.logs .e{color:#ff8a80}',
        '.confirm{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:10px;padding:9px 11px;',
        'border:1px solid rgba(217,83,79,.28);background:rgba(217,83,79,.07);border-radius:8px;font-size:12px}',
        '.confirm .sp{flex:1}',
        '.expand>td{padding:0;background:var(--soft)}',
        '.xbody{padding:12px 14px;border-bottom:1px solid var(--line)}',
        '.empty{color:var(--muted);text-align:center;padding:26px 10px;font-size:13px}',
        '.foot{display:flex;gap:12px;align-items:center;padding:8px 15px;border-top:1px solid var(--line);color:var(--muted);font-size:11.5px}',
        '.foot .sp{flex:1}',
        '.pager{display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:9px 15px;border-top:1px solid var(--line);color:var(--muted);font-size:11.5px}',
        '.pager .sp{flex:1}',
        '.pager select{height:26px;border:1px solid var(--line);border-radius:7px;background:var(--panel);color:var(--fg);font:inherit;padding:0 4px}',
        '.pager .btn{height:26px;padding:0 9px;font-size:11.5px}',
        '.pnums{display:inline-flex;gap:4px}',
        '.pnum{min-width:26px;height:26px;padding:0 6px;border:1px solid var(--line);background:var(--panel);color:var(--fg);border-radius:7px;font:inherit;font-size:11.5px;cursor:pointer}',
        '.pnum.on{background:var(--brand);border-color:var(--brand);color:#fff}',
        '.pnum:disabled{opacity:.4;cursor:default}',
        '.dots{padding:0 2px}',
        '.pcur{display:none}',
        '.menu{position:fixed;z-index:60;min-width:132px;background:var(--panel);border:1px solid var(--line);',
        'border-radius:8px;box-shadow:0 8px 24px rgba(16,24,40,.16);padding:4px;display:none}',
        '.menu.on{display:block}',
        '.menu button{display:block;width:100%;text-align:left;border:0;background:transparent;color:var(--fg);',
        'font:inherit;padding:6px 10px;border-radius:6px;cursor:pointer;white-space:nowrap}',
        '.menu button:hover:not(:disabled){background:var(--soft)}',
        '.menu button:disabled{opacity:.4;cursor:default}',
        '.menu button.danger{color:var(--bad)}',
        '.win.narrow th.c-start,.win.narrow td.c-start,.win.narrow th.c-mem,.win.narrow td.c-mem{display:none}',
        '.win.narrow th.st,.win.narrow td.st{width:170px}',
        '.win.narrow th.op,.win.narrow td.op{width:180px}',
        '.win.small th.c-start,.win.small td.c-start,.win.small th.c-mem,.win.small td.c-mem,',
        '.win.small th.c-up,.win.small td.c-up{display:none}',
        '.win.small .up-inline{display:block}',
        '.win.small .pnums{display:none}.win.small .pcur{display:inline}',
        '.win.small th.st,.win.small td.st{width:92px}',
        '.win.small th.op,.win.small td.op{width:100px}',
        '.win.small th,.win.small td{padding:8px 5px}',
        '.win.small .bar .sp{display:none}',
        '.win.small .tabs button{padding:6px 7px;font-size:12px}',
        '.win.small .acts{gap:4px}',
        '.win.small .acts .btn:not(.pri):not(.more){display:none}',
        '.win.small .st .detail:not(.up-inline){display:none}',
        '.win.small .st .up-inline{white-space:normal;overflow:visible;text-overflow:clip;line-height:1.45}',
        '.win.small .name{font-size:11.5px}',
        '.win.small .desc{display:none}',
        '.win.small .bar input{max-width:none;min-width:120px}'
    ].join('');

    function Manager(opts) {
        this.opts = opts || {};
        this.container = this.opts.container;
        this.inline = !!this.opts.inline;
        this.title = this.opts.title || '服务管理';
        this.wsUrl = this.opts.wsUrl;
        this.token = this.opts.token || global.sessionStorage.getItem('rtsvc.token:' + this.wsUrl) || '';
        this.onClose = this.opts.onClose || null;

        this.ws = null;
        this.connected = false;
        this.authed = false;
        this.writable = false;
        this.systemd = true;
        this.services = [];
        this.total = 0;
        this.running = 0;
        this.failed = 0;
        this.stopped = 0;
        this.matched = 0;
        this.pages = 1;
        this.filter = '';
        this.tab = 'all';
        this.page = 1;
        this.pageSize = 10;
        this.sortKey = '';
        this.sortDir = 1;
        this.expanded = '';
        this.logs = '';
        this.logErr = '';
        this.logNote = '';
        this.confirm = '';
        this.err = '';
        this.busy = false;
        this.timer = null;
        this.filterTimer = null;
        this.logTimer = null;
        this.visible = false;
        this.seq = 0;
        this.askedToken = false;

        this.host = h('div');
        this.sh = this.host.attachShadow({ mode: 'open' });
        var st = document.createElement('style');
        st.textContent = CSS;
        this.sh.appendChild(st);

        this.win = h('div', 'win');
        this.sh.appendChild(this.win);
        this.build();
        if (this.container) this.container.appendChild(this.host);
        this.fit();
        if (global.ResizeObserver) {
            var self = this;
            this._ro = new global.ResizeObserver(function () { self.fit(); });
            this._ro.observe(this.win);
        }
    }

    // 按组件自身宽度切换列，避免页面很宽但卡片很窄时列被挤扁
    Manager.prototype.fit = function () {
        var w = this.win.clientWidth;
        if (!w) return;
        this.win.classList.toggle('narrow', w < 900);
        this.win.classList.toggle('small', w < 760);
    };

    Manager.prototype.build = function () {
        var self = this;
        var head = h('div', 'head');
        head.appendChild(h('div', 't', this.title));
        this.q = {};
        this.q.sum = h('div', 'sum', '');
        head.appendChild(this.q.sum);
        var x = h('button', 'x', '×');
        x.title = '关闭';
        x.addEventListener('click', function () { self.close(); });
        head.appendChild(x);
        if (this.inline) head.hidden = true;
        this.win.appendChild(head);

        var bar = h('div', 'bar');
        this.q.q = h('input');
        this.q.q.type = 'search';
        this.q.q.placeholder = '搜索服务名或描述…';
        this.q.q.addEventListener('input', function () {
            self.filter = this.value.trim().toLowerCase();
            self.page = 1;
            if (self.filterTimer) clearTimeout(self.filterTimer);
            self.filterTimer = setTimeout(function () { self.load(); }, 300);
        });
        bar.appendChild(this.q.q);

        var tabs = h('div', 'tabs');
        this.q.tabs = {};
        [['all', '全部'], ['running', '运行中'], ['failed', '失败'], ['stopped', '已停止']].forEach(function (t) {
            var b = h('button', '', t[1]);
            b.dataset.tab = t[0];
            b.addEventListener('click', function () { self.tab = t[0]; self.page = 1; self.load(); });
            tabs.appendChild(b);
            self.q.tabs[t[0]] = b;
        });
        bar.appendChild(tabs);
        bar.appendChild(h('div', 'sp'));
        this.q.mode = h('span', 'tag', '');
        if (this.inline) this.q.mode.hidden = true;
        bar.appendChild(this.q.mode);
        this.q.refresh = h('button', 'btn', '刷新');
        this.q.refresh.addEventListener('click', function () { self.load(true); });
        bar.appendChild(this.q.refresh);
        this.win.appendChild(bar);

        this.q.err = h('div', 'err', '');
        this.q.err.hidden = true;
        this.win.appendChild(this.q.err);

        var tw = h('div', 'tw');
        var table = h('table');
        var thead = h('thead');
        var tr = h('tr');
        this.q.arw = {};
        [
            ['服务', 'name', ''],
            ['状态', 'status', 'st'],
            ['自启', 'start', 'c-start'],
            ['内存', 'mem', 'c-mem'],
            ['运行时长', 'uptime', 'c-up'],
            ['操作', '', 'op']
        ].forEach(function (c) {
            var th = h('th', c[2], c[0]);
            if (c[1]) {
                th.classList.add('sortable');
                th.title = '点击排序';
                var arw = h('span', 'arw', '');
                th.appendChild(arw);
                self.q.arw[c[1]] = arw;
                th.addEventListener('click', function () { self.sortBy(c[1]); });
            }
            tr.appendChild(th);
        });
        thead.appendChild(tr);
        table.appendChild(thead);
        this.q.tbody = h('tbody');
        table.appendChild(this.q.tbody);
        tw.appendChild(table);
        this.win.appendChild(tw);

        this.q.empty = h('div', 'empty', '正在加载服务列表…');
        this.win.appendChild(this.q.empty);

        var pager = h('div', 'pager');
        this.q.pcount = h('span', 'pcount', '');
        pager.appendChild(this.q.pcount);
        var psz = h('span', '', '每页 ');
        this.q.psize = document.createElement('select');
        [5, 10, 20, 50].forEach(function (n) {
            var o = document.createElement('option');
            o.value = String(n);
            o.textContent = String(n);
            self.q.psize.appendChild(o);
        });
        this.q.psize.value = String(this.pageSize);
        this.q.psize.addEventListener('change', function () {
            self.pageSize = Number(this.value) || 10;
            self.page = 1;
            self.load();
        });
        psz.appendChild(this.q.psize);
        psz.appendChild(document.createTextNode(' 条'));
        pager.appendChild(psz);
        pager.appendChild(h('span', 'sp'));
        this.q.prev = h('button', 'btn', '‹');
        this.q.prev.title = '上一页';
        this.q.prev.addEventListener('click', function () {
            if (self.page > 1) { self.page--; self.load(); }
        });
        pager.appendChild(this.q.prev);
        this.q.pnums = h('span', 'pnums');
        this.q.pnums.addEventListener('click', function (e) {
            var b = e.target && e.target.closest ? e.target.closest('button[data-page]') : null;
            if (!b) return;
            self.page = Number(b.dataset.page) || 1;
            self.load();
        });
        pager.appendChild(this.q.pnums);
        this.q.pcur = h('span', 'pcur', '');
        pager.appendChild(this.q.pcur);
        this.q.next = h('button', 'btn', '›');
        this.q.next.title = '下一页';
        this.q.next.addEventListener('click', function () {
            if (self.page < self.pages) { self.page++; self.load(); }
        });
        pager.appendChild(this.q.next);
        this.win.appendChild(pager);

        var foot = h('div', 'foot');
        this.q.foot = h('span', '', '');
        foot.appendChild(this.q.foot);
        foot.appendChild(h('span', 'sp'));
        this.q.updated = h('span', '', '');
        foot.appendChild(this.q.updated);
        this.win.appendChild(foot);

        this.q.menu = h('div', 'menu');
        this.win.appendChild(this.q.menu);

        this.sh.addEventListener('click', function (e) { self.onClick(e); });
        this._docClick = function (e) {
            if (!self.q.menu.classList.contains('on')) return;
            var path = e.composedPath ? e.composedPath() : [];
            if (path.indexOf(self.q.menu) < 0) self.closeMenu();
        };
        document.addEventListener('click', this._docClick);
        this._vis = function () {
            if (document.hidden) return;
            if (self.visible) self.load();
        };
        document.addEventListener('visibilitychange', this._vis);
        this._menuAway = function () {
            // 打开菜单的这次点击不会滚动页面；250ms 内忽略，避免刚打开就被滚动事件关掉
            if (Date.now() - (self._menuAt || 0) > 250) self.closeMenu();
        };
        global.addEventListener('scroll', this._menuAway, true);
        global.addEventListener('resize', this._menuAway);
    };

    Manager.prototype.open = function () {
        this.visible = true;
        this.host.hidden = false;
        this.win.hidden = false;
        if (!this.connected) this.connect();
        this.load();
        this.startTimers();
    };

    Manager.prototype.close = function () {
        this.visible = false;
        this.stopTimers();
        this.win.hidden = true;
        this.host.hidden = true;
        this.closeMenu();
        if (!this.inline) this.teardown();
        if (this.onClose) this.onClose();
    };

    Manager.prototype.destroy = function () {
        this.stopTimers();
        this.teardown();
        if (this._ro) { this._ro.disconnect(); this._ro = null; }
        document.removeEventListener('click', this._docClick);
        document.removeEventListener('visibilitychange', this._vis);
        global.removeEventListener('scroll', this._menuAway, true);
        global.removeEventListener('resize', this._menuAway);
        if (this.host.parentNode) this.host.parentNode.removeChild(this.host);
    };

    Manager.prototype.teardown = function () {
        this.connected = false;
        this.authed = false;
        if (this.ws) {
            try { this.ws.close(); } catch (e) {}
            this.ws = null;
        }
    };

    Manager.prototype.startTimers = function () {
        var self = this;
        this.stopTimers();
        this.timer = setInterval(function () {
            if (self.visible && !document.hidden) self.load();
        }, 5000);
    };

    Manager.prototype.stopTimers = function () {
        if (this.timer) { clearInterval(this.timer); this.timer = null; }
        if (this.logTimer) { clearInterval(this.logTimer); this.logTimer = null; }
    };

    Manager.prototype.connect = function () {
        var self = this;
        if (this.ws) return;
        try {
            this.ws = new WebSocket(this.wsUrl);
        } catch (e) {
            this.setError('无法连接设备：' + e.message);
            return;
        }
        this.ws.onopen = function () {
            self.connected = true;
            self.hello();
        };
        this.ws.onmessage = function (ev) {
            var msg;
            try { msg = JSON.parse(ev.data); } catch (e) { return; }
            self.onMessage(msg);
        };
        this.ws.onerror = function () { self.setError('服务管理连接失败'); };
        this.ws.onclose = function () {
            self.connected = false;
            self.authed = false;
            if (self.visible) self.setError('连接已断开，正在重试…');
        };
    };

    Manager.prototype.hello = function () {
        var msg = { op: 'hello', id: ++this.seq };
        if (this.token) msg.token = this.token;
        this.send(msg);
    };

    Manager.prototype.send = function (obj) {
        if (!this.ws || this.ws.readyState !== 1) return;
        this.ws.send(JSON.stringify(obj));
    };

    Manager.prototype.onMessage = function (msg) {
        if (msg.op === 'welcome') {
            this.authed = true;
            this.writable = !!msg.writable;
            this.systemd = msg.systemd !== false;
            this.setError('');
            this.load();
            return;
        }
        if (msg.op === 'error') {
            if (msg.code === 2 && !this.askedToken) {
                this.askedToken = true;
                var t = global.prompt('该设备已启用服务管理 Token，请输入：', this.token || '');
                if (t) {
                    this.token = t;
                    global.sessionStorage.setItem('rtsvc.token:' + this.wsUrl, t);
                    this.teardown();
                    this.connect();
                }
                return;
            }
            this.setError(msg.message || '操作失败');
            this.busy = false;
            this.q.refresh.disabled = false;
            this.render();
            return;
        }
        if (msg.op === 'list') {
            this.q.refresh.disabled = false;
            if (!msg.ok) { this.setError(msg.error || '读取服务列表失败'); return; }
            this.services = msg.services || [];
            this.total = msg.total || 0;
            this.running = msg.running || 0;
            this.failed = msg.failed || 0;
            this.stopped = msg.stopped || 0;
            this.matched = msg.matched == null ? this.services.length : msg.matched;
            this.pages = msg.pages || 1;
            this.page = msg.page || 1;
            this.setError('');
            this.render();
            var d = new Date();
            function p(x) { return x < 10 ? '0' + x : '' + x; }
            this.q.updated.textContent = '更新于 ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
            return;
        }
        if (msg.op === 'logs') {
            if (this.expanded !== msg.unit) return;
            this.logs = msg.ok ? (msg.text || '') : '';
            this.logNote = msg.ok ? (msg.note || '') : '';
            this.logErr = msg.ok ? '' : (msg.error || '读取日志失败');
            this.renderExpand();
            return;
        }
        if (msg.op === 'action') {
            this.busy = false;
            this.q.refresh.disabled = false;
            if (!msg.ok) {
                this.setError((msg.unit || '') + '：' + (msg.error || '操作失败'));
                this.render();
                return;
            }
            this.confirm = '';
            this.setError('');
            var self = this;
            this.load(true);
            setTimeout(function () { self.load(); }, 1200);
        }
    };

    Manager.prototype.request = function (op, extra) {
        var msg = { op: op, id: ++this.seq };
        for (var k in extra) msg[k] = extra[k];
        this.send(msg);
    };

    Manager.prototype.load = function (manual) {
        if (manual) this.q.refresh.disabled = true;
        if (!this.connected) this.connect();
        this.request('list', {
            page: this.page,
            page_size: this.pageSize,
            sort: this.sortKey,
            dir: this.sortDir,
            q: this.filter,
            tab: this.tab
        });
    };

    Manager.prototype.setError = function (text) {
        this.err = text || '';
        this.q.err.textContent = this.err;
        this.q.err.hidden = !this.err;
    };

    Manager.prototype.sortBy = function (key) {
        if (this.sortKey === key) this.sortDir = -this.sortDir;
        else { this.sortKey = key; this.sortDir = 1; }
        this.page = 1;
        this.load();
    };

    Manager.prototype.render = function () {
        var self = this;
        this.q.tabs.all.textContent = '全部 ' + this.total;
        this.q.tabs.running.textContent = '运行中 ' + this.running;
        this.q.tabs.failed.textContent = '失败 ' + this.failed;
        this.q.tabs.stopped.textContent = '已停止 ' + this.stopped;
        for (var k in this.q.tabs) this.q.tabs[k].classList.toggle('on', k === this.tab);
        this.q.sum.textContent = 'systemd · ' + this.total + ' 个服务';
        this.q.mode.textContent = this.writable ? '可控制' : '只读';
        this.q.mode.className = 'tag' + (this.writable ? ' rw' : '');
        this.q.foot.textContent = this.writable ? '' : '只读（agent 加 --svc-rw 后可控制）';
        this.q.empty.textContent = this.connected
            ? (this.services.length ? '没有匹配的服务' : '没有可管理的服务')
            : '正在连接设备…';

        for (var k2 in this.q.arw) {
            this.q.arw[k2].textContent = this.sortKey === k2 ? (this.sortDir > 0 ? '▲' : '▼') : '';
        }

        var rows = this.services;
        this.q.empty.hidden = rows.length > 0;

        var pages = this.pages || 1;
        if (this.page > pages) this.page = pages;
        if (this.page < 1) this.page = 1;

        var frag = document.createDocumentFragment();
        var still = false;
        rows.forEach(function (s) {
            frag.appendChild(self.row(s));
            if (s.name === self.expanded) {
                still = true;
                frag.appendChild(self.expandRow(s));
            }
        });
        if (!still) {
            this.expanded = '';
            this.stopLog();
        }
        this.q.tbody.textContent = '';
        this.q.tbody.appendChild(frag);

        this.q.pcount.textContent = '共 ' + this.matched + ' 个';
        this.q.prev.disabled = this.page <= 1;
        this.q.next.disabled = this.page >= pages;
        this.q.pcur.textContent = this.page + ' / ' + pages;
        this.q.pnums.textContent = '';
        this.q.pnums.hidden = pages <= 1;
        var from = Math.max(1, this.page - 2), to = Math.min(pages, from + 4);
        from = Math.max(1, to - 4);
        function pageBtn(n) {
            var b = h('button', 'pnum' + (n === self.page ? ' on' : ''), String(n));
            b.dataset.page = String(n);
            return b;
        }
        if (from > 1) {
            this.q.pnums.appendChild(pageBtn(1));
            if (from > 2) this.q.pnums.appendChild(h('span', 'dots', '…'));
        }
        for (var p = from; p <= to; p++) this.q.pnums.appendChild(pageBtn(p));
        if (to < pages) {
            if (to < pages - 1) this.q.pnums.appendChild(h('span', 'dots', '…'));
            this.q.pnums.appendChild(pageBtn(pages));
        }
    };

    Manager.prototype.row = function (s) {
        var self = this;
        var tr = h('tr', 'row');
        tr.dataset.name = s.name;

        var tdSvc = h('td');
        var name = h('div', 'name');
        var st = statusOf(s);
        name.appendChild(h('i', 'led ' + st.cls));
        name.appendChild(h('span', '', s.name));
        tdSvc.appendChild(name);
        if (s.description) tdSvc.appendChild(h('div', 'desc', s.description));
        tr.appendChild(tdSvc);

        var tdSt = h('td', 'st');
        tdSt.appendChild(h('span', 'badge ' + st.cls, st.text));
        if (st.detail) tdSt.appendChild(h('div', 'detail', st.detail));
        if (st.up) {
            var upInline = h('div', 'detail up-inline', fmtDurShort(s.active_ms));
            upInline.title = '已运行 ' + st.up;
            tdSt.appendChild(upInline);
        }
        tr.appendChild(tdSt);

        tr.appendChild(h('td', 'c-start', FILE_STATE[s.file_state] || s.file_state || '—'));
        tr.appendChild(h('td', 'c-mem', fmtMem(s.mem)));
        tr.appendChild(h('td', 'c-up', st.up || '—'));

        var tdOp = h('td', 'op');
        var acts = h('div', 'acts');
        var primary = this.primary(s);
        if (primary) {
            var pb = h('button', 'btn pri', primary.label);
            pb.dataset.act = primary.verb;
            pb.dataset.name = s.name;
            pb.disabled = !this.writable;
            acts.appendChild(pb);
        }
        var lb = h('button', 'btn', '日志');
        lb.dataset.act = '@logs';
        lb.dataset.name = s.name;
        acts.appendChild(lb);
        var mb = h('button', 'btn more', '⋯');
        mb.dataset.act = '@menu';
        mb.dataset.name = s.name;
        acts.appendChild(mb);
        tdOp.appendChild(acts);
        tr.appendChild(tdOp);
        return tr;
    };

    Manager.prototype.primary = function (s) {
        if (s.file_state === 'masked') return { verb: 'unmask', label: '取消屏蔽' };
        if (s.active === 'active') return { verb: 'stop', label: '停止' };
        if (s.active === 'activating' || s.active === 'deactivating') return { verb: 'stop', label: '停止' };
        return { verb: 'start', label: '启动' };
    };

    Manager.prototype.expandRow = function (s) {
        var tr = h('tr', 'expand');
        var td = h('td');
        td.colSpan = 6;
        var body = h('div', 'xbody');
        var meta = h('div', 'meta');
        function kv(k, v) {
            var sp = h('span', '', k + ' ');
            sp.appendChild(h('b', '', v || '—'));
            meta.appendChild(sp);
        }
        kv('Unit', s.name);
        kv('Load', s.load);
        kv('Active', (s.active || '') + (s.sub ? ' (' + s.sub + ')' : ''));
        kv('Main PID', s.pid ? String(s.pid) : '—');
        if (s.active === 'active' && s.active_ms >= 0) kv('已运行', fmtDur(s.active_ms));
        body.appendChild(meta);

        if (this.logErr) {
            body.appendChild(h('div', 'detail', this.logErr));
        }
        var pre = h('pre', 'logs');
        var text = this.logs || this.logNote || (this.logTimer ? '正在读取日志…' : '');
        if (text) {
            // 只对明显的错误行着色，避免把日志当 HTML 解析
            text.split('\n').forEach(function (line, i, arr) {
                var span = document.createElement('span');
                if (/error|failed|fail|fatal|panic|denied|not found/i.test(line)) span.className = 'e';
                span.textContent = line + (i < arr.length - 1 ? '\n' : '');
                pre.appendChild(span);
            });
        }
        body.appendChild(pre);

        if (this.confirm === s.name) {
            var verb = this.confirmVerb;
            var label = { stop: '停止', restart: '重启', disable: '禁用', mask: '屏蔽' }[verb] || verb;
            var box = h('div', 'confirm');
            box.appendChild(h('span', '', '确定' + label + ' ' + s.name + '？' + (verb === 'stop' ? '服务会立即中断。' : '')));
            box.appendChild(h('span', 'sp'));
            var cancel = h('button', 'btn', '取消');
            cancel.dataset.act = '@cancel';
            cancel.dataset.name = s.name;
            box.appendChild(cancel);
            var ok = h('button', 'btn danger', label);
            ok.dataset.act = '@do';
            ok.dataset.name = s.name;
            ok.dataset.verb = verb;
            box.appendChild(ok);
            body.appendChild(box);
        }
        td.appendChild(body);
        tr.appendChild(td);
        return tr;
    };

    Manager.prototype.renderExpand = function () {
        if (!this.expanded) return;
        var row = this.q.tbody.querySelector('tr.expand');
        if (!row) return;
        var name = this.expanded;
        var s = null;
        for (var i = 0; i < this.services.length; i++) {
            if (this.services[i].name === name) { s = this.services[i]; break; }
        }
        if (!s) return;
        var pre = row.querySelector('pre.logs');
        if (!pre) return;
        var atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 40;
        pre.textContent = '';
        var text = this.logErr ? '' : (this.logs || this.logNote || '');
        if (this.logErr) {
            pre.textContent = this.logErr;
        }
        else if (text) {
            text.split('\n').forEach(function (line, i, arr) {
                var span = document.createElement('span');
                if (/error|failed|fail|fatal|panic|denied|not found/i.test(line)) span.className = 'e';
                span.textContent = line + (i < arr.length - 1 ? '\n' : '');
                pre.appendChild(span);
            });
        }
        if (atBottom) pre.scrollTop = pre.scrollHeight;
    };

    Manager.prototype.toggleExpand = function (name) {
        if (this.expanded === name) {
            this.expanded = '';
            this.confirm = '';
            this.stopLog();
            this.render();
            return;
        }
        this.expanded = name;
        this.confirm = '';
        this.logs = '';
        this.logErr = '';
        this.logNote = '';
        this.render();
        this.startLog(name);
    };

    Manager.prototype.startLog = function (name) {
        var self = this;
        this.stopLog();
        this.loadLog(name);
        this.logTimer = setInterval(function () {
            if (self.visible && !document.hidden && self.expanded === name) self.loadLog(name);
        }, 2000);
    };

    Manager.prototype.stopLog = function () {
        if (this.logTimer) { clearInterval(this.logTimer); this.logTimer = null; }
        this.logs = '';
        this.logErr = '';
        this.logNote = '';
    };

    Manager.prototype.loadLog = function (name) {
        this.request('logs', { unit: name, lines: 200 });
    };

    Manager.prototype.onClick = function (e) {
        var b = e.target.closest ? e.target.closest('[data-act]') : null;
        if (!b) {
            var tr = e.target.closest ? e.target.closest('tr.row') : null;
            if (tr && e.target.closest('td') && !e.target.closest('button')) this.toggleExpand(tr.dataset.name);
            return;
        }
        var act = b.dataset.act, name = b.dataset.name;
        if (act === '@menu') {
            // 不要让这次点击冒泡到 document 的关闭逻辑，否则菜单刚打开就被关掉
            e.stopPropagation();
            this.showMenu(b, name);
            return;
        }
        if (act === '@logs') { this.toggleExpand(name); return; }
        if (act === '@cancel') {
            this.confirm = '';
            this.render();
            return;
        }
        if (act === '@do') {
            this.doAction(name, b.dataset.verb);
            return;
        }
        this.askAction(name, act);
    };

    Manager.prototype.askAction = function (name, verb) {
        if (!this.writable) return;
        if (verb === 'stop' || verb === 'restart' || verb === 'disable' || verb === 'mask') {
            if (this.expanded !== name) {
                this.expanded = name;
                this.logs = '';
                this.logErr = '';
                this.logNote = '';
                this.render();
                this.startLog(name);
            }
            this.confirm = name;
            this.confirmVerb = verb;
            this.render();
            var row = this.q.tbody.querySelector('tr.expand');
            if (row) row.scrollIntoView({ block: 'nearest' });
            return;
        }
        this.doAction(name, verb);
    };

    Manager.prototype.doAction = function (name, verb) {
        if (!this.writable || this.busy) return;
        this.busy = true;
        this.q.refresh.disabled = true;
        this.request('action', { unit: name, verb: verb });
    };

    Manager.prototype.showMenu = function (btn, name) {
        var self = this;
        var s = null;
        for (var i = 0; i < this.services.length; i++) {
            if (this.services[i].name === name) { s = this.services[i]; break; }
        }
        if (!s) return;
        var menu = this.q.menu;
        menu.textContent = '';
        var items = [
            ['start', '启动'], ['stop', '停止'], ['restart', '重启'],
            ['enable', '启用'], ['disable', '禁用'], ['mask', '屏蔽'], ['unmask', '取消屏蔽']
        ];
        items.forEach(function (it) {
            var b = h('button', it[0] === 'stop' || it[0] === 'mask' ? 'danger' : '', it[1]);
            b.disabled = !canAct(s, it[0], self.writable);
            b.addEventListener('click', function () {
                self.closeMenu();
                self.askAction(name, it[0]);
            });
            menu.appendChild(b);
        });
        var lg = h('button', '', '日志');
        lg.addEventListener('click', function () { self.closeMenu(); self.toggleExpand(name); });
        menu.appendChild(lg);

        // 菜单用 fixed 定位，避免被 .win / .tw 的 overflow 裁掉
        menu.style.left = '0px';
        menu.style.top = '0px';
        menu.classList.add('on');
        var br = btn.getBoundingClientRect();
        var mw = menu.offsetWidth, mh = menu.offsetHeight;
        var left = Math.min(Math.max(8, br.right - mw), global.innerWidth - mw - 8);
        var top = br.bottom + 6;
        if (top + mh > global.innerHeight - 8) top = Math.max(8, br.top - mh - 6);
        menu.style.left = left + 'px';
        menu.style.top = top + 'px';
        this._menuAt = Date.now();
    };

    Manager.prototype.closeMenu = function () {
        this.q.menu.classList.remove('on');
    };

    global.RtunnelSvc = {
        createManager: function (opts) { return new Manager(opts); }
    };
})(window);
