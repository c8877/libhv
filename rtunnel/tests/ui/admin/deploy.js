// 运维 → 部署：主机登记、连接测试、提交作业、实时日志
// 后端全部用 route 拦截，不依赖真实的 SSH 目标机。
const h = require('../lib/harness')

const HOSTS = [
  {
    id: 1, name: '车间-01', host: '10.0.0.21', port: 22, username: 'root', auth: 'password',
    sudo: false, note: '', secret_set: true, last_status: 'ok', last_ms: Date.now(),
  },
  {
    id: 2, name: '待补凭据', host: '10.0.0.22', port: 22, username: 'ops', auth: 'key',
    sudo: true, note: '测试机', secret_set: false, last_status: '', last_ms: 0,
  },
]
const PKGS = [
  { name: 'rtunnel-agent-web-1.4.151-x86_64.tar.gz', path: 'packages/agent/1.4.151/x86_64/rtunnel-agent-web-1.4.151-x86_64.tar.gz', component: 'agent', version: '1.4.151', arch: 'x86_64', variant: 'web', size: 4212345, mtime_ms: Date.now() },
  { name: 'rtunnel-client-1.4.151-x86_64.tar.gz', path: 'packages/client/1.4.151/x86_64/rtunnel-client-1.4.151-x86_64.tar.gz', component: 'client', version: '1.4.151', arch: 'x86_64', variant: '', size: 3212345, mtime_ms: Date.now() - 1000 },
  { name: 'rtunnel-agent-web-1.4.151-aarch64.tar.gz', path: 'packages/agent/1.4.151/aarch64/rtunnel-agent-web-1.4.151-aarch64.tar.gz', component: 'agent', version: '1.4.151', arch: 'aarch64', variant: 'web', size: 4112345, mtime_ms: Date.now() - 2000 },
]
const JOBS = [
  {
    id: 7, host_id: 1, host: '10.0.0.21', username: 'root', component: 'agent',
    version: '1.4.151', arch: 'x86_64', variant: 'web', prefix: '/opt/rtunnel/agent',
    pkg: PKGS[0].name, status: 'success', step: '完成', error: '',
    actor: 'admin', created_ms: Date.now() - 60000, started_ms: Date.now() - 59000,
    finished_ms: Date.now() - 50000, log: '[x] [预检] 架构 x86_64\n[x] [安装] INSTALLED=/opt/rtunnel/agent/bin/rtunnel-agent-1.4.151\n',
  },
]

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  let posted = null
  let probed = 0
  let created = false
  await p.route(/\/api\/v1\/deploy\/hosts\/\d+\/probe$/, (route) => {
    probed++
    route.fulfill({
      status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, arch: 'x86_64', uid: 0, systemd: true, free_kb: 8123456, installed: 'none' }),
    })
  })
  let scanBody = null
  await p.route(/\/api\/v1\/deploy\/scan$/, (route) => {
    scanBody = JSON.parse(route.request().postData() || '{}')
    route.fulfill({
      status: 200, contentType: 'application/json',
      body: JSON.stringify({
        engine: 'nmap', cidr: scanBody.cidr, port: scanBody.port, scanned: 256, duration_ms: 4310,
        items: [
          { host: '10.0.0.21', port: 22, service: 'ssh', known: true, host_id: 1 },
          { host: '10.0.0.33', port: 22, service: 'ssh', known: false },
        ],
      }),
    })
  })
  await p.route(/\/api\/v1\/deploy\/hosts(\?|$)/, (route) => {
    if (route.request().method() === 'GET') {
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ items: HOSTS }) })
    }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ id: 9 }) })
  })
  await p.route(/\/api\/v1\/deploy\/packages(\?|$)/, (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ items: PKGS }) }))
  await p.route(/\/api\/v1\/deploy\/jobs(\?|$)/, (route) => {
    if (route.request().method() === 'GET') {
      // 提交后列表里要立刻能看到这条，页面才会自动打开它的抽屉
      const items = created
        ? [{ ...JOBS[0], id: 8, status: 'running', step: '安装', finished_ms: 0 }, ...JOBS]
        : JOBS
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ items }) })
    }
    posted = JSON.parse(route.request().postData() || '{}')
    created = true
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ id: 8 }) })
  })
  await p.route(/\/api\/v1\/deploy\/jobs\/8$/, (route) =>
    route.fulfill({
      status: 200, contentType: 'application/json',
      body: JSON.stringify({
        id: 8, host_id: 1, host: '10.0.0.21', username: 'root', component: 'agent',
        version: '1.4.151', arch: 'x86_64', variant: 'web', prefix: '/opt/rtunnel/agent',
        pkg: PKGS[0].name, status: 'running', step: '安装', error: '', actor: 'admin',
        created_ms: Date.now(), started_ms: Date.now(),
        log: '[x] [预检] 架构 x86_64\n[x] [安装] 执行包内 install.sh\n',
      }),
    }))

  await p.goto(h.A + '/#/deploy')
  await p.waitForSelector('.dep-form', { timeout: 20000 })
  await p.waitForTimeout(600)

  h.check('部署页可打开', await p.locator('.panel-head h2').first().innerText() === '部署')
  const tabs = await p.locator('.el-tabs__item').allInnerTexts()
  h.check('安装包来自文件仓库', (await p.locator('.panel').innerText()).includes('文件仓库'))
  h.check('有三个页签', tabs.join('|').includes('部署') && tabs.join('|').includes('主机') &&
    tabs.join('|').includes('记录'), tabs)

  h.check('没选主机和包时不能提交',
    await p.locator('button', { hasText: '开始部署' }).isDisabled())

  // 组件默认 agent，显示设备 ID 与 relay 地址
  h.check('默认装 agent', (await p.locator('.el-radio-button.is-active').first().innerText()).includes('agent'))
  h.check('agent 需要设备 ID', (await p.locator('.dep-form').innerText()).includes('设备 ID'))

  // 选择主机后自动探测，并把探测结果展示出来
  await p.locator('.dep-form .el-select').first().click()
  await p.locator('.el-select-dropdown__item').first().click()
  await p.waitForTimeout(800)
  const devidVal = await p.locator('.el-form-item', { hasText: '设备 ID' }).locator('input').first().inputValue()
  h.check('选中主机后自动填设备 ID', (devidVal || '').length > 0, devidVal)
  h.check('探测到目标架构', (await p.locator('.probe').innerText()).includes('x86_64'))
  h.check('探测到 systemd', (await p.locator('.probe').innerText()).includes('systemd'))
  h.check('提交前有目标摘要', (await p.locator('.dep-actions .summary').innerText()).includes('→'))

  // 选到架构不符的包要提前提示，而不是等预检失败
  await p.locator('.dep-form .el-select').nth(1).click()
  await p.locator('.el-select-dropdown__item:visible', { hasText: 'aarch64' }).first().click()
  await p.waitForTimeout(400)
  h.check('架构不符有提示', (await p.locator('.dep-form').innerText()).includes('预检会拒绝'))

  // 网段扫描：默认用已选主机的 /24，结果里区分已登记/待登记
  await p.locator('.panel-head button', { hasText: '扫描网段' }).click()
  await p.waitForTimeout(500)
  const scanDlg = p.locator('.el-dialog:visible').filter({ hasText: '扫描网段里的 SSH 主机' })
  h.check('打开发扫描对话框', await scanDlg.count() === 1)
  const cidrVal = await scanDlg.locator('input').first().inputValue()
  h.check('默认网段取自已选主机', cidrVal === '10.0.0.0/24', cidrVal)
  await scanDlg.locator('button', { hasText: '开始扫描' }).click()
  await p.waitForTimeout(800)
  h.check('扫描请求带上网段与端口', scanBody && scanBody.cidr === '10.0.0.0/24' && scanBody.port === 22, scanBody)
  const scanText = await scanDlg.locator('.el-table__body').innerText()
  h.check('结果里标出已登记主机', scanText.includes('10.0.0.21') && scanText.includes('已登记'), scanText)
  h.check('结果里列出新主机', scanText.includes('10.0.0.33') && scanText.includes('待登记'), scanText)
  h.check('显示扫描引擎与耗时', (await scanDlg.innerText()).includes('nmap'), await scanDlg.innerText())
  // 登记扫到的主机：地址与端口直接带进表单
  await scanDlg.locator('.el-table__body tr', { hasText: '10.0.0.33' })
    .locator('button', { hasText: '登记' }).click()
  await p.waitForTimeout(600)
  const hostDlg = p.locator('.el-dialog:visible').filter({ hasText: '登记主机' })
  h.check('扫描后直接打开登记表单', await hostDlg.count() === 1)
  h.check('扫到的地址已填入', await hostDlg.locator('input').nth(1).inputValue() === '10.0.0.33',
    await hostDlg.locator('input').nth(1).inputValue())
  await hostDlg.locator('button', { hasText: '取消' }).click()
  await p.waitForTimeout(400)

  // 切到 relay：只留管理端口/初始令牌
  await p.locator('.el-radio-button', { hasText: 'relay（中继节点）' }).first().click()
  await p.waitForTimeout(300)
  const relayText = await p.locator('.dep-form').innerText()
  h.check('relay 要管理端口', relayText.includes('管理端口'))
  h.check('relay 不要设备 ID', !relayText.includes('设备 ID'))
  h.check('切换组件后清空安装包',
    (await p.locator('.el-form-item', { hasText: '安装包' }).locator('.el-select').first().innerText()).includes('选择安装包'))

  // 切回 agent，选包并提交
  await p.locator('.el-radio-button', { hasText: 'agent（设备接入）' }).first().click()
  await p.waitForTimeout(400)
  await p.keyboard.press('Escape')
  await p.locator('.el-form-item', { hasText: '安装包' }).locator('.el-select').first().click()
  await p.locator('.el-select-dropdown__item:visible').first().click()
  await p.waitForTimeout(400)
  h.check('选好主机和包后可以提交',
    !(await p.locator('button', { hasText: '开始部署' }).isDisabled()))

  await p.locator('button', { hasText: '开始部署' }).click()
  await p.waitForTimeout(1200)
  h.check('提交了部署作业', posted && posted.component === 'agent' &&
    posted.package === PKGS[0].path && posted.host_id === 1, posted)
  h.check('作业参数带上 devid/server',
    posted && posted.options && posted.options.devid && posted.options.server, posted && posted.options)
  h.check('打开作业日志抽屉', (await p.locator('.el-drawer:visible').count()) >= 1 &&
    (await p.locator('.el-drawer:visible').first().innerText()).includes('部署 #8'))
  await p.waitForTimeout(1200)
  const logtxt = await p.locator('.el-drawer:visible').first().innerText()
  h.check('日志显示步骤', logtxt.includes('预检') && logtxt.includes('安装'), logtxt.slice(0, 200))
  h.check('运行中可以取消', (await p.locator('.el-drawer:visible').first().innerText()).includes('取消'))
  await p.locator('.el-drawer:visible .el-drawer__close-btn').first().click().catch(() => {})
  await p.waitForTimeout(400)

  // 主机页签：缺凭据要有提示
  await p.locator('.el-tabs__item', { hasText: '主机' }).click()
  await p.waitForTimeout(500)
  const rows = await p.locator('.el-tab-pane:visible .el-table__body tr').count()
  h.check('主机列表有数据', rows >= 2, rows)
  h.check('缺凭据有提示',
    (await p.locator('.el-tab-pane:visible .el-table__body').first().innerText()).includes('缺凭据'))
  h.check('不回显密码/私钥',
    !(await p.locator('.el-tab-pane:visible .el-table__body').first().innerText()).match(/password|PRIVATE KEY/i))

  // 主机行的“更多”收敛了测试/编辑/删除
  await p.locator('.el-tab-pane:visible .el-table__body tr').first()
    .locator('button', { hasText: '更多' }).click()
  await p.waitForTimeout(400)
  const menuText = await p.locator('.el-dropdown-menu:visible').innerText()
  h.check('主机更多操作齐全',
    ['测试连接', '编辑', '删除'].every((x) => menuText.includes(x)), menuText)
  await p.keyboard.press('Escape')
  await p.waitForTimeout(300)

  // 记录页签：能看到历史作业
  await p.locator('.el-tabs__item', { hasText: '记录' }).click()
  await p.waitForTimeout(500)
  h.check('记录页有作业',
    (await p.locator('.el-tab-pane:visible .el-table__body').first().innerText()).includes('agent'))
  h.check('记录页显示版本',
    (await p.locator('.el-tab-pane:visible .el-table__body').first().innerText()).includes('1.4.151'))
  await p.locator('.el-tab-pane:visible .bar .el-radio-button', { hasText: '进行中' }).click()
  await p.waitForTimeout(400)
  const runRows = await p.locator('.el-tab-pane:visible .el-table__body tr').count()
  h.check('按状态筛选进行中', runRows === 1, runRows)
  await p.locator('.el-tab-pane:visible .bar .el-radio-button', { hasText: '全部' }).click()
  await p.waitForTimeout(300)
  h.check('筛选能回到全部',
    (await p.locator('.el-tab-pane:visible .el-table__body tr').count()) === 2)
  h.check('探测确实调用过', probed >= 1, probed)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
