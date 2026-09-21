const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  onSubs: (cb) => ipcRenderer.on('subs', (_e, v) => cb(v)),
  getTheme: () => ipcRenderer.invoke('get-theme'),
  getSubs: () => ipcRenderer.invoke('get-subs'),
  rescanSubs: () => ipcRenderer.invoke('rescan-subs'),
  fitHeight: (h) => ipcRenderer.send('fit-subs-height', h),
  close: () => ipcRenderer.send('close-subs')
});
