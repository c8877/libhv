// 服务端排序：设备进程、网络（TCP/UNIX/IPC）、任务列表、OTA 任务表。
// 这些表分页在服务端，排序必须由接口完成，不是只排当前页。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b)
  await h.login(p)

  // ---- 进程 ----
  await h.openDevice(p, '进程')
  await p.waitForSelector('.el-table__body tbody tr', { timeout: 25000 })
  await p.waitForTimeout(800)

  async function clickProcSort(label, dir) {
    const [resp] = await Promise.all([
      p.waitForResponse((r) => /\/procs\?/.test(r.url()) && r.url().includes('dir=' + dir), { timeout: 25000 }),
      p.locator('.el-table__header th', { hasText: label }).click({ position: { x: 6, y: 8 } }),
    ])
    await p.waitForTimeout(300)
    return resp.json()
  }
  const arrow = (label) => p.locator('.el-table__header th', { hasText: label }).getAttribute('class')

  const base = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV + '/procs?sort=cpu&dir=0')).json()
  h.check('默认 CPU 降序', h.sorted(base.entries.map((x) => x.cpu_pct), -1),
    base.entries.slice(0, 4).map((x) => x.cpu_pct))
  h.check('默认表头箭头指向 CPU', ((await arrow('CPU')) || '').indexOf('descending') >= 0, await arrow('CPU'))

  let j = await clickProcSort('CPU', 1)
  h.check('点 CPU 升序请求生效', h.sorted(j.entries.map((x) => x.cpu_pct), 1))
  h.check('CPU 箭头变升序', ((await arrow('CPU')) || '').indexOf('ascending') >= 0, await arrow('CPU'))
  j = await clickProcSort('CPU', -1)
  h.check('再点 CPU 降序', h.sorted(j.entries.map((x) => x.cpu_pct), -1))

  j = await clickProcSort('PID', 1)
  h.check('切到 PID 从升序开始', h.sorted(j.entries.map((x) => x.pid), 1), j.entries.slice(0, 4).map((x) => x.pid))
  j = await clickProcSort('PID', -1)
  h.check('再点 PID 降序', h.sorted(j.entries.map((x) => x.pid), -1))

  j = await clickProcSort('内存', -1)
  h.check('内存默认降序', h.sorted(j.entries.map((x) => x.rss_kb), -1))
  j = await clickProcSort('内存', 1)
  h.check('再点内存升序', h.sorted(j.entries.map((x) => x.rss_kb), 1))

  j = await clickProcSort('线程', -1)
  h.check('线程可排序', h.sorted(j.entries.map((x) => x.threads), -1))
  j = await clickProcSort('用户', 1)
  h.check('用户可排序', h.sorted(j.entries.map((x) => String(x.user).toLowerCase()), 1))
  j = await clickProcSort('命令', 1)
  h.check('命令可排序', h.sorted(j.entries.map((x) => String(x.comm).toLowerCase()), 1))
  j = await clickProcSort('启动时间', -1)
  h.check('启动时间可排序', h.sorted(j.entries.map((x) => x.start_ms), -1))

  // ---- 网络 ----
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '网络' }).click()
  await p.waitForSelector('.net-tabs', { timeout: 25000 })

  async function sortNet(label, dir) {
    const [resp] = await Promise.all([
      p.waitForResponse((r) => /\/net\?/.test(r.url()) && r.url().includes('dir=' + dir), { timeout: 30000 }),
      p.locator('.net-table .el-table__header th', { hasText: label }).click({ position: { x: 6, y: 8 } }),
    ])
    await p.waitForTimeout(300)
    return resp.json()
  }

  await p.locator('.net-tabs .el-tabs__item', { hasText: 'TCP' }).click()
  await p.waitForSelector('.net-table .el-table__body tbody tr', { timeout: 30000 })
  let j2 = await sortNet('本地端口', -1)
  let v = (j2.tcp.entries || []).map((x) => x.local_port)
  h.check('TCP 本地端口默认降序', v.length > 1 && h.sorted(v, -1), v.slice(0, 6))
  j2 = await sortNet('本地端口', 1)
  v = (j2.tcp.entries || []).map((x) => x.local_port)
  h.check('再点本地端口升序', h.sorted(v, 1), v.slice(0, 6))

  await p.locator('.net-tabs .el-tabs__item', { hasText: 'UNIX' }).click()
  await p.waitForSelector('.net-table .el-table__body tbody tr', { timeout: 30000 })
  j2 = await sortNet('路径', 1)
  v = (j2.unix.entries || []).map((x) => x.path || '')
  h.check('UNIX 路径升序', v.length > 1 && h.sorted(v, 1), v.slice(0, 4))

  await p.locator('.net-tabs .el-tabs__item', { hasText: 'IPC' }).click()
  await p.waitForSelector('.net-table .el-table__body tbody tr', { timeout: 45000 })
  j2 = await sortNet('ID', -1)
  v = (j2.ipc.shm || []).map((x) => Number(x.id))
  h.check('IPC ID 默认降序', v.length > 1 && h.sorted(v, -1), v.slice(0, 6))

  // ---- 任务列表 ----
  await p.goto(h.A + '/#/tasks')
  await p.waitForSelector('.el-table__body tbody tr', { timeout: 30000 })
  async function sortTasks(label, dir, field) {
    const [resp] = await Promise.all([
      p.waitForResponse((r) => /\/distributions\?/.test(r.url())
        && r.url().includes('sort=' + field) && r.url().includes('dir=' + dir), { timeout: 30000 }),
      p.locator('.el-table__header th', { hasText: label }).click({ position: { x: 6, y: 8 } }),
    ])
    await p.waitForTimeout(300)
    return resp.json()
  }
  let t = await sortTasks('文件', 1, 'file_name')
  v = t.items.map((x) => x.file_name)
  h.check('任务文件名升序', v.length > 1 && h.sorted(v, 1), v)
  t = await sortTasks('进度 / 大小', -1, 'file_size')
  v = t.items.map((x) => x.file_size)
  h.check('任务大小降序', v.length > 1 && h.sorted(v, -1), v)
  t = await sortTasks('状态', 1, 'status')
  v = t.items.map((x) => x.status)
  h.check('任务状态升序', h.sorted(v, 1), v)

  // ---- OTA 任务表 ----
  await p.goto(h.A + '/#/ota')
  await p.waitForSelector('.el-table__body tbody tr', { timeout: 30000 })
  const [otaResp] = await Promise.all([
    p.waitForResponse((r) => /\/distributions\?/.test(r.url())
      && r.url().includes('kind=ota') && r.url().includes('sort=target_version'), { timeout: 30000 }),
    p.locator('.el-table__header th', { hasText: '目标版本' }).click({ position: { x: 6, y: 8 } }),
  ])
  await p.waitForTimeout(300)
  const ota = await otaResp.json()
  const versions = ota.items.map((x) => x.target_version)
  h.check('OTA 目标版本升序', versions.length > 1 && h.sorted(versions, 1), versions)

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await b.close()
  h.finish()
})()
