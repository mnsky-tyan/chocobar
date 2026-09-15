const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  onTokens: (cb) => ipcRenderer.on('tokens', (_e, v) => cb(v)),
  getTheme: () => ipcRenderer.invoke('get-theme'),
  getTokens: () => ipcRenderer.invoke('get-tokens'),
  rescanTokens: () => ipcRenderer.invoke('rescan-tokens'),
  close: () => ipcRenderer.send('close-dash')
});
