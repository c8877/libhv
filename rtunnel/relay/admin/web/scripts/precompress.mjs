// 为静态产物生成 .gz/.br 预压缩副本，relay 按 Accept-Encoding 直接返回
import { copyFile, readdir, readFile, stat, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { brotliCompressSync, constants, gzipSync } from 'node:zlib'

const root = process.argv[2] || '.output/public'
const EXT = new Set(['.js', '.css', '.html', '.json', '.svg', '.txt'])
const files = []

// relay 以 admin.html 作为入口；dist 里没有时从 index.html 复制一份
try {
  await stat(join(root, 'index.html'))
  try {
    await stat(join(root, 'admin.html'))
  } catch {
    await copyFile(join(root, 'index.html'), join(root, 'admin.html'))
  }
} catch { /* 非 Vite 产物布局，跳过 */ }

async function walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const p = join(dir, entry.name)
    if (entry.isDirectory()) await walk(p)
    else if (EXT.has(p.slice(p.lastIndexOf('.')))) files.push(p)
  }
}

await walk(root)

let saved = 0
for (const file of files) {
  const src = await readFile(file)
  const gz = gzipSync(src, { level: 9 })
  if (gz.length < src.length) {
    await writeFile(file + '.gz', gz)
    saved += src.length - gz.length
  }
  const br = brotliCompressSync(src, {
    params: { [constants.BROTLI_PARAM_QUALITY]: 11 },
  })
  if (br.length < src.length) {
    await writeFile(file + '.br', br)
    saved += src.length - br.length
  }
}

const total = (await Promise.all(files.map((f) => stat(f)))).reduce((n, s) => n + s.size, 0)
console.log(`precompress: ${files.length} files, ${(total / 1024).toFixed(0)} KB -> saved ${(saved / 1024).toFixed(0)} KB`)
