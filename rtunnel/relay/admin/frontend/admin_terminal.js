// 设备终端：xterm.js + WebSocket，切页面不断开，支持多 tab
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
  async enter(el, params) {
    this.el = el;
    el.innerHTML = '<div class="panel"><div class="empty">终端组件加载中…</div></div>';
    try {
      await loadCssOnce('/vendor/xterm.css');
      await loadScriptOnce('/vendor/xterm.js');
      await loadScriptOnce('/vendor/xterm-addon-fit.js');
    } catch (e) {
      el.innerHTML = '<div class="panel"><div class="empty">终端组件加载失败</div></div>';
      return;
    }
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
    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    this.el.innerHTML = '<div class="panel"><div class="panel-head"><h2>终端</h2>' +
      '<span class="spacer"></span>' +
      '<button id="term-new">新开</button> <button id="term-close">关闭当前</button></div>' +
      '<div id="term-tabs" class="term-tabbar"></div>' +
      '<div id="term-host" class="ui-term"></div></div>';
    this._tabs = UI.tabs(document.getElementById('term-tabs'), {
      items: ids.map(id => {
        const s = TermMgr.sessions.get(id);
        return { key: id, label: s.devid + (s.alive ? '' : ' ✕') };
      }),
      value: this.active,
      onChange: key => { this.active = key; this.render(); },
    });
    const host = document.getElementById('term-host');
    if (!cur.opened) { cur.term.open(cur.box); cur.opened = true; }
    host.appendChild(cur.box);
    document.getElementById('term-new').onclick = async () => {
      const d = await UI.prompt({ title: '新建终端', label: '设备 devid', value: cur.devid });
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
    if (this._tabs) { this._tabs.destroy(); this._tabs = null; }
    const s = TermMgr.sessions.get(this.active);
    if (s && s.box.parentNode) s.box.parentNode.removeChild(s.box);   // 仅摘 DOM，会话保留
  },
});

Router.register(/^#\/terminal\/([^/]+)$/, TerminalPage, '终端');
