const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  onStats: (cb) => ipcRenderer.on('stats', (_e, v) => cb(v)),
  onTokens: (cb) => ipcRenderer.on('tokens', (_e, v) => cb(v)),
  onPet: (cb) => ipcRenderer.on('pet', (_e, v) => cb(v)),
  onSubs: (cb) => ipcRenderer.on('subs', (_e, v) => cb(v)),
  getTheme: () => ipcRenderer.invoke('get-theme'),
  openDash: () => ipcRenderer.send('open-dash'),
  openSubs: () => ipcRenderer.send('open-subs'),
  runShortcut: () => ipcRenderer.send('run-shortcut'),
  runCustom: (id) => ipcRenderer.send('run-custom', id),
  onCustom: (cb) => ipcRenderer.on('custom', (_e, v) => cb(v)),
  contextMenu: (x, y) => ipcRenderer.send('bar-context', { x, y }),
  raiseTerminal: () => ipcRenderer.send('raise-terminal'),
  togglePet: () => ipcRenderer.invoke('toggle-pet'),
  reportSize: (w) => ipcRenderer.send('bar-content-size', w)
});
