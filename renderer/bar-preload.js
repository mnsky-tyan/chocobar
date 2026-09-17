const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  onStats: (cb) => ipcRenderer.on('stats', (_e, v) => cb(v)),
  onTokens: (cb) => ipcRenderer.on('tokens', (_e, v) => cb(v)),
  onPet: (cb) => ipcRenderer.on('pet', (_e, v) => cb(v)),
  getTheme: () => ipcRenderer.invoke('get-theme'),
  openDash: () => ipcRenderer.send('open-dash'),
  runShortcut: () => ipcRenderer.send('run-shortcut'),
  contextMenu: () => ipcRenderer.send('bar-context'),
  togglePet: () => ipcRenderer.invoke('toggle-pet'),
  reportSize: (w) => ipcRenderer.send('bar-content-size', w)
});
