// 设备视图 UI 测试的公共工具。
//
// run-ui.sh 负责启动 relay / agent，并通过环境变量传入地址：
//   A   管理台地址（relay admin，例如 http://127.0.0.1:18080）
//   L   设备本地页地址（agent 内置单页，例如 http://127.0.0.1:18081）
//   DEV / DEV2  两台测试设备的 devid（默认 uitest / uitest2）
//   T   临时目录（仓库文件、分发任务等都在里面）
//   UI_ARTIFACTS  截图输出目录
const { chromium } = require('playwright')

const A = process.env.A || ''
const L = process.env.L || ''
const T = process.env.T || ''
const DEV = process.env.DEV || 'uitest'
const DEV2 = process.env.DEV2 || 'uitest2'
const USER = process.env.UI_USER || 'admin'
const PASS = process.env.UI_PASS || 'pass12345'
const ART = process.env.UI_ARTIFACTS || '/tmp'

let failed = 0
let passed = 0
let skipped = 0

function check(name, ok, extra) {
  if (ok) {
    passed++
    console.log('ok   ' + name)
  } else {
    failed++
    console.log('FAIL ' + name + (extra !== undefined ? '  ' + JSON.stringify(extra) : ''))
  }
}

function skip(name, why) {
  skipped++
  console.log('skip ' + name + (why ? '  (' + why + ')' : ''))
}

// 忽略浏览器对静态资源的常规 404（例如 favicon），只把真正的脚本错误算进去
function isNoise(text) {
  return /favicon|net::ERR_ABORTED|Failed to load resource/.test(text)
}

async function openPage(browser, opts) {
  const ctx = await browser.newContext(Object.assign({ viewport: { width: 1440, height: 1000 } }, opts || {}))
  const page = await ctx.newPage()
  const errs = []
  page.on('pageerror', (e) => errs.push(String(e).slice(0, 300)))
  page.on('console', (m) => {
    if (m.type() !== 'error') return
    const t = m.text()
    if (!isNoise(t)) errs.push('console: ' + t.slice(0, 200))
  })
  return { ctx, page, errs }
}

function baseIsAdmin(base) {
  return base === A
}

async function login(page, base) {
  await page.goto((base || A) + '/')
  await page.waitForSelector('#af-user', { timeout: 20000 })
  await page.fill('#af-user', USER)
  await page.fill('#af-pass', PASS)
  await page.click('#af-btn')
  await page.waitForTimeout(1200)
}

// 打开设备详情页并切到指定一级 tab；tab 为空时停留在概览
async function openDevice(page, tab, devid) {
  await page.goto(A + '/#/agents/' + encodeURIComponent(devid || DEV))
  await page.waitForSelector('.dev-tabs', { timeout: 30000 })
  if (tab) {
    await page.locator('.dev-tabs .el-tabs__item', { hasText: tab }).click()
    await page.waitForTimeout(1200)
  }
}

async function shot(page, name) {
  try {
    await page.screenshot({ path: ART + '/' + name + '.png' })
  } catch (e) {
    // 截图失败不影响判定
  }
}

function sorted(arr, dir) {
  return arr.every((x, i) => i === 0 || (dir > 0 ? arr[i - 1] <= x : arr[i - 1] >= x))
}

async function launch() {
  return chromium.launch()
}

function finish() {
  const tail = 'passed=' + passed + ' failed=' + failed + (skipped ? ' skipped=' + skipped : '')
  console.log(tail)
  if (failed) {
    console.log('UI FAILED')
    process.exit(1)
  }
  console.log('UI PASS')
}

module.exports = {
  A, L, T, DEV, DEV2, USER, PASS, ART,
  check, skip, launch, openPage, login, openDevice, shot, sorted, finish,
  chromium,
}
