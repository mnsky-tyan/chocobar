' Silent WizBar launcher (no console window). Used by the autostart entry.
Set sh = CreateObject("WScript.Shell")
appDir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName))
electron = appDir & "\node_modules\electron\dist\electron.exe"
If CreateObject("Scripting.FileSystemObject").FileExists(electron) Then
  sh.Run """" & electron & """ """ & appDir & """", 0, False
Else
  sh.Run "cmd /c cd /d """ & appDir & """ && npm start", 0, False
End If
