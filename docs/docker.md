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

### Check a build

```bash
docker/smoke-test.sh bromelia
```

This checks the image without a drive: every program the daemon calls is there and finds its libraries, `makemkvcon`
starts, the first start writes the configuration, the web page answers with its token (401 without it, 403 for an
action without the `X-Bromelia` header), and `docker stop` ends the daemon cleanly. CI runs it on every push. It
can't check ripping; do that once with a real drive (below).

## Run

```bash
docker run -d --name bromelia \
  --device /dev/sr0 --device /dev/sg0 \
  -v /run/udev:/run/udev:ro \
  -v /run/dbus/system_bus_socket:/run/dbus/system_bus_socket \
  -v /srv/bromelia/config:/config \
  -v /srv/media/rips:/output \
  -e BROMELIA_WEB_TOKEN=change-me \
  -p 51280:51280 \
  bromelia
```

- **Drives:** MakeMKV needs both the block device (`/dev/srN`) and the matching SCSI generic device
  (`/dev/sgN`; `lsscsi -g` shows which). Pass every drive you want to use.
- **`/run/udev`** lets Bromelia tell audio CDs and data discs apart. Without it every disc is treated as a video disc.
- **`/run/dbus/system_bus_socket`** lets the daemon keep the host awake while jobs and background steps run (it asks
  systemd-logind to block sleep and idle; `preventSleep` turns this off). Without it the log says once that it can't,
  and the host may suspend in the middle of a rip.
- **`/config`** holds Bromelia's `bromelia/config.json`, its history and job logs (`data/`), and MakeMKV's own
  settings and key (`.MakeMKV/`). On the first start a configuration is written that saves to `/output` and serves
  the web page on port 51280.
- **Web page:** the container's network isn't only this computer, so the page needs a token: set
  `BROMELIA_WEB_TOKEN` (or `webUI.token` in the configuration) and open `http://<server>:51280/?token=<token>`.
  Without a token the page stays off. There is no TLS; use a reverse proxy for access from outside your network.
- **Registration:** set `registrationKey` in the configuration, or `"autoUpdateBetaKey": true` to register
  MakeMKV's free beta key at startup and whenever it expires (a purchased key is never replaced).
- **Archive checks:** `"archiveCheck": {"intervalDays": 30}` reads everything under `/output` again every 30 days and
  compares it with the `SHA256SUMS` files, sending the result to `notifications` (a damaged folder is sent as a
  failure, so `onlyProblems` targets get it too). *Check output folder* on the web page, or `POST /api/verify`, starts
  a check now.
- **Discs archived before:** an automatic rip recognises a disc already in the history or in `/output` (its
  `bromelia.json`) and skips it (`automation.alreadyArchived`: `skip`, `ask` or `ripAgain`).
- **Automatic rips:** set `automation.autoRipOnInsert` for the default drive (`defaultDrive`) or for each drive.
  Drives are polled every `pollIntervalSeconds` (keep it above 0 in a container; there are no desktop media
  events). The tray can be closed from the web page.

`docker stop` cancels running jobs (their files are kept in an `[INCOMPLETE]` folder) and exits; a second
signal exits at once. If the container is killed instead (or the host loses power), the next start logs the jobs
that were unfinished, records them in the history and makes an interrupted job's files visible as
`INCOMPLETE - <id>`.

## Without Docker

On any Linux system, build the core without GTK and run the daemon directly:

```bash
cd linux
meson setup build -Dui=false
ninja -C build
./build/src/bromelia-daemon --listen 127.0.0.1 --port 51280
```

While jobs run it holds a systemd-logind sleep inhibitor (`systemd-inhibit --list` shows it as “Bromelia”).

Options: `--config PATH` (default `~/.config/bromelia/config.json`), `--listen ADDRESS`, `--port PORT` and
`--token TOKEN` (or `BROMELIA_WEB_TOKEN`) turn the web page on for this run without saving them.
