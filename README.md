# wallflasher
wallflasher is a wallpaper companion for [LGUG2Z/komorebi](https://github.com/LGUG2Z/komorebi) that works around Windows' slow wallpaper APIs.  
its a sledgehammer solution to a delicate problem, not subtle or particualrly clever, but it works

wallflasher looks for JPEG files with the **.jpg** extension in a folder named **wallpapers** next to the .exe  
the filename pattern is:
```
[ filename-sanitised monitor device_id ]_[ workspace name ].jpg
```
sanitization means:
- lowercase only
- all non-alphanumeric characters removed

for example, if `komorebic state` contains:  
```json
  "monitors": {
    "elements": [
      {
        "id": 65537,
        "name": "DISPLAY1",
        "device": "SAM71A8",
        "device_id": "SAM71A8-5&130eb7b4&1&UID20741",
        "serial_number_id": "H7BR901620",
        "size": {
          "left": 0,
[...]
```

`SAM71A8-5&130eb7b4&1&UID20741` => `sam71a85130eb7b41uid20741`  
switching to a workspace named `main` on that monitor would make wallflasher paint the bitmap loaded from `sam71a85130eb7b41uid20741_main.jpg` onto that monitor

## Fallbacks
wallflasher supports both monitor-specific and global fallbacks:
- if no workspace match is found, if `[monitor_id].jpg`, `sam71a85130eb7b41uid20741.jpg` for the above example, is loaded, it is painted for unknown workspaces on that monitor
- if no monitor fallback exists, if `wallpaper.jpg` is loaded, it is painted for workspaces on unknown monitors
- if nothing matches, a solid black rectangle is painted

## Image requirements

image files:
- MUST be JPEGs
- MUST use the `.jpg` extnsion
- MUST match the monitor resolution exactly

## Utility commands
for convenience `wallflasher wallpaper-info` prints the expected filenames and resolutions for all defined workspaces:
```
> wallflasher.exe wallpaper-info                                                             20:32

sam71a85130eb7b41uid20741_mainl.jpg : 1920x1080
sam71a85130eb7b41uid20741_discord.jpg : 1920x1080
sam71a85130eb7b41uid20741_three.jpg : 1920x1080
sam71a85130eb7b41uid20740_mainr.jpg : 1920x1080
sam71a85130eb7b41uid20740_secondary.jpg : 1920x1080
```

there is no UI, to unload wallflasher: `wallflasher unload`  
if you change wallpaper files, run `wallflasher reload`, this hot-reloads `wallpapers\*.jpg` and re-request workspaces from komorebic

## Notes
wallflasher loads ALL .jpg files from wallpapers\ into memory  
only keep wallpapers (or symlinks to wallpapers) that you actually use in that directory  
there is currently no deduplication of image data. if multiple symlinked files point to the same image, each one will still be loaded separately into RAM  
like i said, this thing is not elegant or clever

## Building
`MSBuild.exe" wallflasher.sln /p:Configuration=Release /p:Platform=x64`  
Works on **My** Machine™ certified.