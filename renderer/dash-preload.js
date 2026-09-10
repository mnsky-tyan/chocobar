const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  onTokens: (cb) => ipcRenderer.on('tokens', (_e, v) => cb(v)),
  onStats: (cb) => ipcRenderer.on('stats', (_e, v) => cb(v)),
  getTheme: () => ipcRenderer.invoke('get-theme'),
  getTokens: () => ipcRenderer.invoke('get-tokens'),
  getStats: () => ipcRenderer.invoke('get-stats'),
  close: () => ipcRenderer.send('close-dash')
});
