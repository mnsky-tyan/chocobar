const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wizbar', {
  onTheme: (cb) => ipcRenderer.on('theme', (_e, v) => cb(v)),
  action: (id) => ipcRenderer.send('menu-action', id),
  hide: () => ipcRenderer.send('menu-hide')
});
