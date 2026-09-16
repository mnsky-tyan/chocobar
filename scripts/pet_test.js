// E2E test of the pet toggle path: spawn exactly like togglePet(),
// check detection with tasklist, then kill with the same taskkill and confirm
// it's gone. (The live bar poll no longer uses tasklist: pollPet() detects
// the pet in-process via native.findProcessIdByName, a Toolhelp32 snapshot.)
// The bar's poll (3s) should flip the chip to "on" in between.
const { spawn, execFile } = require('child_process');
const path = require('path');
const fs = require('fs');

const exe = ''; // point at your own pet executable (modules.pet.exePath)
const img = path.basename(exe);

const probe = () => new Promise((res) => {
  execFile('tasklist', ['/FI', 'IMAGENAME eq ' + img, '/FO', 'CSV', '/NH'], { windowsHide: true },
    (err, stdout) => res(!err && /","/.test(stdout || '')));
});

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  console.log('exists:', fs.existsSync(exe));
  console.log('running before:', await probe());

  console.log('-- launch (detached, like the chip click)');
  const child = spawn(exe, [], { cwd: path.dirname(exe), detached: true, stdio: 'ignore' });
  child.unref();
  await wait(6000);
  const running = await probe();
  console.log('running after launch:', running);

  console.log('-- waiting for the bar poll to notice (chip should read "on")');
  await wait(4000);

  console.log('-- kill (taskkill /T /F, like the second click)');
  execFile('taskkill', ['/IM', img, '/F', '/T'], { windowsHide: true }, async () => {
    await wait(1500);
    console.log('running after kill:', await probe());
  });
})();
