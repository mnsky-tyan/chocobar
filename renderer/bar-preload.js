const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  onStats: (cb) => ipcRenderer.on('stats', (_e, v) => cb(v)),
  onTokens: (cb) => ipcRenderer.on('tokens', (_e, v) => cb(v)),
  onRemielle: (cb) => ipcRenderer.on('remielle', (_e, v) => cb(v)),
  getTheme: () => ipcRenderer.invoke('get-theme'),
  openDash: () => ipcRenderer.send('open-dash'),
  contextMenu: () => ipcRenderer.send('bar-context'),
  toggleRemielle: () => ipcRenderer.invoke('toggle-remielle'),
  reportSize: (w) => ipcRenderer.send('bar-content-size', w)
});
