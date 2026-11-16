// CodeMirror 6 封装：只用于 shell / json 高亮，体积优先。
// 由 vite.editor.config.js 打成 IIFE，本地 Web 与 relay-admin 共用同一份 cm-editor.js。
import { EditorState } from '@codemirror/state'
import {
  EditorView,
  keymap,
  lineNumbers,
  highlightActiveLine,
  highlightActiveLineGutter,
  drawSelection,
  dropCursor,
  rectangularSelection,
  crosshairCursor
} from '@codemirror/view'
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands'
import { search, searchKeymap, highlightSelectionMatches } from '@codemirror/search'
import {
  HighlightStyle,
  StreamLanguage,
  bracketMatching,
  foldGutter,
  foldKeymap,
  indentOnInput,
  syntaxHighlighting
} from '@codemirror/language'
import { tags } from '@lezer/highlight'
import { json } from '@codemirror/lang-json'
import { shell } from '@codemirror/legacy-modes/mode/shell'

// 与 fs-ui.js 自己的配色保持同一判断：只跟随系统深色，跟 html.dark 无关
function prefersDark() {
  return !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches)
}

const PALETTE = {
  light: {
    keyword: '#a626a4',
    string: '#50a14f',
    number: '#986801',
    comment: '#a0a1a7',
    property: '#e45649',
    operator: '#383a42'
  },
  dark: {
    keyword: '#c678dd',
    string: '#98c379',
    number: '#d19a66',
    comment: '#7f848e',
    property: '#e06c75',
    operator: '#abb2bf'
  }
}

function highlightFor(dark) {
  const c = dark ? PALETTE.dark : PALETTE.light
  return HighlightStyle.define([
    { tag: [tags.keyword, tags.controlKeyword, tags.operatorKeyword], color: c.keyword },
    { tag: [tags.tagName, tags.standard(tags.tagName)], color: c.keyword },
    { tag: [tags.string, tags.special(tags.string)], color: c.string },
    { tag: [tags.number, tags.integer, tags.float], color: c.number },
    { tag: [tags.bool, tags.null, tags.atom], color: c.number },
    { tag: [tags.comment, tags.lineComment, tags.blockComment], color: c.comment, fontStyle: 'italic' },
    { tag: [tags.propertyName, tags.attributeName], color: c.property },
    { tag: [tags.operator, tags.punctuation, tags.separator], color: c.operator },
    { tag: [tags.variableName, tags.function(tags.variableName)], color: c.operator },
    { tag: tags.invalid, color: '#ef4444' }
  ])
}

function themeFor(dark) {
  return EditorView.theme(
    {
      '&': {
        height: '100%',
        backgroundColor: 'var(--input)',
        color: 'var(--fg)',
        fontSize: '12.5px'
      },
      '&.cm-focused': { outline: 'none' },
      '.cm-scroller': {
        fontFamily: 'ui-monospace,SFMono-Regular,Menlo,monospace',
        lineHeight: '1.55'
      },
      '.cm-content': { caretColor: 'var(--fg)', padding: '8px 0' },
      '.cm-gutters': {
        backgroundColor: 'var(--panel)',
        color: 'var(--muted)',
        border: 'none',
        borderRight: '1px solid var(--line)'
      },
      '.cm-activeLine': { backgroundColor: 'var(--hover)' },
      '.cm-activeLineGutter': { backgroundColor: 'var(--hover)', color: 'var(--fg)' },
      '.cm-selectionBackground, &.cm-focused .cm-selectionBackground, ::selection': {
        backgroundColor: 'var(--sel) !important'
      },
      '.cm-cursor, .cm-dropCursor': { borderLeftColor: 'var(--fg)' },
      '.cm-foldPlaceholder': {
        backgroundColor: 'var(--badge)',
        border: '1px solid var(--line)',
        color: 'var(--muted)'
      },
      '.cm-panels': { backgroundColor: 'var(--panel)', color: 'var(--fg)' },
      '.cm-panels.cm-panels-top': { borderBottom: '1px solid var(--line)' },
      '.cm-textfield': {
        backgroundColor: 'var(--input)',
        color: 'var(--fg)',
        border: '1px solid var(--line)'
      },
      '.cm-button': {
        backgroundImage: 'none',
        backgroundColor: 'var(--btn)',
        color: 'var(--fg)',
        border: '1px solid var(--line)'
      },
      '.cm-searchMatch': { backgroundColor: 'rgba(250,204,21,.35)' },
      '.cm-searchMatch.cm-searchMatch-selected': { backgroundColor: 'rgba(250,204,21,.65)' }
    },
    { dark }
  )
}

function languageFor(lang) {
  if (lang === 'json') return json()
  if (lang === 'shell') return StreamLanguage.define(shell)
  return []
}

/**
 * 在 host 里创建编辑器。
 * opts: { value, language: 'shell'|'json'|'text', onSave(), onEscape() }
 */
export function createEditor(host, opts) {
  opts = opts || {}
  const dark = prefersDark()
  const view = new EditorView({
    parent: host,
    state: EditorState.create({
      doc: opts.value || '',
      extensions: [
        lineNumbers(),
        highlightActiveLineGutter(),
        highlightActiveLine(),
        history(),
        drawSelection(),
        dropCursor(),
        rectangularSelection(),
        crosshairCursor(),
        indentOnInput(),
        bracketMatching(),
        foldGutter(),
        highlightSelectionMatches(),
        search({ top: true }),
        syntaxHighlighting(highlightFor(dark)),
        themeFor(dark),
        EditorView.contentAttributes.of({ spellcheck: 'false', autocapitalize: 'off' }),
        languageFor(opts.language),
        keymap.of([
          {
            key: 'Mod-s',
            preventDefault: true,
            run: () => {
              if (opts.onSave) opts.onSave()
              return true
            }
          },
          {
            key: 'Escape',
            run: () => {
              if (opts.onEscape) opts.onEscape()
              return true
            }
          },
          ...defaultKeymap,
          ...searchKeymap,
          ...historyKeymap,
          ...foldKeymap,
          indentWithTab
        ])
      ]
    })
  })
  view.focus()
  return {
    getValue: () => view.state.doc.toString(),
    destroy: () => view.destroy(),
  }
}
