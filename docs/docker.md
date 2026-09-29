# Running Bromelia headless (Docker)

`bromelia-daemon` is Bromelia without a window, for a server or NAS with optical drives: it watches the
drives, starts automatic rips, runs post-processing (including the background queue), sends notifications
and serves the web page. It reads the same configuration file as the apps
([configuration.md](configuration.md)), so you can set a configuration up in the Bromelia app, export it and
copy it over, or edit the JSON by hand.

## Build

MakeMKV is built from makemkv.com's sources inside the image. Its binary part is covered by MakeMKV's own
licence, so the build only runs when you accept it:

```bash
docker build -f docker/Dockerfile --build-arg ACCEPT_MAKEMKV_EULA=yes -t bromelia .
```

`--build-arg MAKEMKV_VERSION=…` picks another MakeMKV version (default 2.0.0; makemkv.com only keeps the current one).
The image also has `mkvmerge`, `ffmpeg` + `tesseract` (episode numbers from DVD menus), `abcde` (and `cyanrip` when
the distribution has it) for audio CDs, `eject` and `curl`.

## Run

```bash
docker run -d --name bromelia \
  --device /dev/sr0 --device /dev/sg0 \
  -v /run/udev:/run/udev:ro \
  -v /srv/bromelia/config:/config \
  -v /srv/media/rips:/output \
  -e BROMELIA_WEB_TOKEN=change-me \
  -p 51280:51280 \
  bromelia
```

- **Drives:** MakeMKV needs both the block device (`/dev/srN`) and the matching SCSI generic device
  (`/dev/sgN`; `lsscsi -g` shows which). Pass every drive you want to use.
- **`/run/udev`** lets Bromelia tell audio CDs and data discs apart. Without it every disc is treated as a video disc.
- **`/config`** holds Bromelia's `bromelia/config.json`, its history and job logs (`data/`), and MakeMKV's own
  settings and key (`.MakeMKV/`). On the first start a configuration is written that saves to `/output` and serves
  the web page on port 51280.
- **Web page:** the container's network isn't only this computer, so the page needs a token: set
  `BROMELIA_WEB_TOKEN` (or `webUI.token` in the configuration) and open `http://<server>:51280/?token=<token>`.
  Without a token the page stays off. There is no TLS; use a reverse proxy for access from outside your network.
- **Registration:** set `registrationKey` in the configuration, or `"autoUpdateBetaKey": true` to register
  MakeMKV's free beta key at startup and whenever it expires (a purchased key is never replaced).
- **Automatic rips:** set `automation.autoRipOnInsert` for the default drive (`defaultDrive`) or for each drive.
  Drives are polled every `pollIntervalSeconds` (keep it above 0 in a container; there are no desktop media
  events). The tray can be closed from the web page.

`docker stop` cancels running jobs (their files are kept in an `[INCOMPLETE]` folder) and exits; a second
signal exits at once.

## Without Docker

On any Linux system, build the core without GTK and run the daemon directly:

```bash
cd linux
meson setup build -Dui=false
ninja -C build
./build/src/bromelia-daemon --listen 127.0.0.1 --port 51280
```

Options: `--config PATH` (default `~/.config/bromelia/config.json`), `--listen ADDRESS`, `--port PORT` and
`--token TOKEN` (or `BROMELIA_WEB_TOKEN`) turn the web page on for this run without saving them.
