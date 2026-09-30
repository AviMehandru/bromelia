// Opens the web page in a real browser (Chromium, through Playwright) against bromelia-daemon and a stand-in for
// makemkvcon, and checks what the owner would check by hand: drives, rip / cancel / eject / close tray, opening a disc and
// ripping chosen titles, job logs, history details, settings, the token (?token=, 401), requests from another computer
// without a token (403), the archive check, HTTPS, dark mode and phone width.
//
//   node shared/web/test/check-web-page.mjs <path to bromelia-daemon> [screenshot folder]
//
// Needs `npm install playwright` and `npx playwright install --with-deps chromium`. CI runs it on Linux; the page is
// the same file on macOS and Windows.
import { chromium } from "playwright";
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import http from "node:http";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";

const daemon = path.resolve(process.argv[2] || "linux/build-headless/src/bromelia-daemon");
const shots = path.resolve(process.argv[3] || "web-page-screenshots");
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const fixtures = path.join(repo, "shared/fixtures");
const token = "s3cret-token";

const work = fs.mkdtempSync(path.join(os.tmpdir(), "bromelia-web-"));
fs.mkdirSync(shots, { recursive: true });

// makemkvcon: the drive scan and DVD listing from the fixtures, and a rip that runs until it is stopped.
const listing = path.join(work, "listing.txt");
fs.writeFileSync(listing, fs.readFileSync(path.join(fixtures, "info-dvd.txt"), "utf8").split("\n").filter(l => !l.startsWith("DRV:")).join("\n"));
const fake = path.join(work, "makemkvcon");
fs.writeFileSync(fake, `#!/bin/sh
echo "$@" >> '${work}/calls.txt'
case "$*" in *"info disc:9999"*) cat '${path.join(fixtures, "drive-scan.txt")}'; exit 0 ;; esac
while [ $# -gt 0 ]; do case "$1" in info|mkv|backup) cmd="$1"; shift; break;; esac; shift; done
case "$cmd" in
  info) cat '${listing}' ;;
  mkv) mkdir -p "$3"; echo 'PRGT:5017,0,"Saving titles to MKV files"'
       i=0; while [ $i -lt 600 ]; do echo "PRGV:$i,$i,600"; sleep 1; i=$((i+1)); done ;;
esac
exit 0
`, { mode: 0o755 });

const output = path.join(work, "output");
function writeConfig(dir, port, webToken, tls) {
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "config.json");
  fs.writeFileSync(file, JSON.stringify({
    version: 2, makemkvconPath: fake, outputRoot: output, pollIntervalSeconds: 2, preventSleep: false,
    webUI: { enabled: true, address: "127.0.0.1", port, token: webToken, tlsCertificate: tls?.cert ?? "", tlsKey: tls?.key ?? "" },
  }));
  return file;
}

const children = [];
function start(name, port, webToken, tls) {
  const home = path.join(work, name);
  const config = writeConfig(path.join(home, "config"), port, webToken, tls);
  const log = fs.openSync(path.join(shots, `${name}.log`), "w");
  const child = spawn(daemon, ["--config", config], {
    env: { ...process.env, HOME: home, XDG_CONFIG_HOME: path.join(home, "config"), XDG_DATA_HOME: path.join(home, "data") },
    stdio: ["ignore", log, log],
  });
  children.push(child);
  return `${tls ? "https" : "http"}://127.0.0.1:${port}`;
}

// A raw request, so the Host header and missing X-Bromelia header can be chosen.
function request(base, method, pathname, headers = {}) {
  return new Promise((resolve, reject) => {
    const u = new URL(pathname, base);
    const req = http.request({ host: u.hostname, port: u.port, path: u.pathname + u.search, method, headers }, res => {
      let body = "";
      res.on("data", c => (body += c));
      res.on("end", () => resolve({ status: res.statusCode, body }));
    });
    req.on("error", reject);
    req.end();
  });
}

async function waitUntilUp(base) {
  for (let i = 0; i < 100; i++) {
    try { await request(base, "GET", `/api/status?token=${token}`); return; } catch { await new Promise(r => setTimeout(r, 100)); }
  }
  throw new Error(`${base} did not start`);
}

let failed = false;
// nas.example is this computer under another name, like a server opened from the network.
const browser = await chromium.launch({ args: ["--host-resolver-rules=MAP nas.example 127.0.0.1"] });
try {
  const base = start("with-token", 51391, token);
  const open = start("no-token", 51392, "");
  await waitUntilUp(base);
  await waitUntilUp(open);

  // --- The API's access rules. ---
  assert.equal((await request(base, "GET", "/api/status")).status, 401, "no token → 401");
  assert.equal((await request(base, "GET", "/api/status", { Authorization: "Bearer wrong" })).status, 401, "wrong token → 401");
  assert.equal((await request(base, "GET", "/api/status", { Authorization: `Bearer ${token}` })).status, 200, "bearer token → 200");
  assert.equal((await request(base, "POST", `/api/verify?token=${token}`)).status, 403, "POST without X-Bromelia → 403");
  assert.equal((await request(open, "GET", "/api/status")).status, 200, "no token configured, localhost → 200");
  const remote = await request(open, "GET", "/api/status", { Host: "nas.example:51392" });
  assert.equal(remote.status, 403, "no token configured, another host name → 403");
  assert.match(remote.body, /token/);

  const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
  page.on("pageerror", e => { failed = true; console.error("Page error:", e.message); });
  const error = page.locator("#error");

  // --- Without the token not even the page loads; the browser shows why. ---
  const noToken = await page.goto(base + "/");
  assert.equal(noToken.status(), 401, "the page without its token → 401");
  assert.match(await page.textContent("body"), /token is required/);

  // --- Opened under another host name without a token: refused, with the reason. ---
  const remotePage = await browser.newPage();
  const refused = await remotePage.goto(open.replace("127.0.0.1", "nas.example") + "/");
  assert.equal(refused.status(), 403, "page from another host name without a token → 403");
  assert.match(await remotePage.textContent("body"), /Set a token/);
  await remotePage.close();

  // --- Drives from the scan, with the right buttons. ---
  await page.goto(`${base}/?token=${token}`);
  const drives = page.locator("#drives .card");
  await page.locator("#drives .card", { hasText: "MOVIE_DISC" }).waitFor();
  const discCard = drives.filter({ hasText: "MOVIE_DISC" });
  await discCard.getByRole("button", { name: "Rip" }).waitFor();
  await discCard.getByRole("button", { name: "Eject" }).waitFor();
  assert.ok((await drives.count()) >= 3, "every drive from the scan is listed");
  assert.ok((await page.getByRole("button", { name: "Close tray" }).count()) >= 1, "empty drives offer Close tray");
  assert.match(await page.locator("#meta").textContent(), /Bromelia/);
  await page.screenshot({ path: path.join(shots, "drives.png"), fullPage: true });

  // --- An action's error stays visible (the output folder doesn't exist yet, so the archive check can't start). ---
  await page.getByRole("button", { name: "Check output folder" }).click();
  await error.filter({ hasText: "doesn't exist" }).waitFor();
  await page.waitForTimeout(4500); // two refreshes
  assert.match(await error.textContent(), /doesn't exist/, "the error of an action outlives the refreshes after it");

  // --- Rip, see the job with its progress, cancel it, find it in Recent. ---
  await discCard.getByRole("button", { name: "Rip" }).click();
  const job = page.locator("#jobs .card").first();
  await job.getByRole("button", { name: "Cancel" }).waitFor({ timeout: 20000 });
  await page.locator("#jobs progress").waitFor({ timeout: 20000 });
  assert.equal(await discCard.getByRole("button", { name: "Rip" }).count(), 0, "a busy drive offers no Rip");
  await page.screenshot({ path: path.join(shots, "ripping.png"), fullPage: true });
  await job.getByRole("button", { name: "Cancel" }).click();
  await page.locator("#jobs .empty").waitFor({ timeout: 20000 });
  await page.locator("#history .card .state.cancelled").first().waitFor({ timeout: 20000 });
  await page.waitForTimeout(2500); // a refresh after the job reached the history
  assert.equal(await page.locator("#history .card").count(), 1, "the cancelled job is listed once");
  assert.equal(await error.textContent(), "", "a successful action clears the earlier error");

  // --- Open the disc, pick titles, rip them; the job's log; the history's details. ---
  await discCard.getByRole("button", { name: "Open" }).click();
  const titles = discCard.locator(".titles label");
  await titles.first().waitFor({ timeout: 20000 });
  const boxes = discCard.locator(".titles input[type=checkbox]");
  const count = await boxes.count();
  assert.ok(count >= 2, "the opened disc lists its titles");
  for (let i = 0; i < count; i++) {
    const want = i < 2;
    if ((await boxes.nth(i).isChecked()) !== want) await boxes.nth(i).click();
  }
  const make = discCard.getByRole("button", { name: "Make MKV (2 titles)" });
  await make.waitFor();
  await page.screenshot({ path: path.join(shots, "titles.png"), fullPage: true });
  const callsBefore = fs.readFileSync(path.join(work, "calls.txt"), "utf8").split("\n").length;
  await make.click();
  const titleJob = page.locator("#jobs .card").first();
  await titleJob.getByRole("button", { name: "Cancel" }).waitFor({ timeout: 20000 });
  await titleJob.getByRole("button", { name: "Log" }).click();
  await titleJob.locator("pre.log", { hasText: "Bromelia job" }).waitFor({ timeout: 20000 });
  await page.locator("#jobs progress").waitFor({ timeout: 20000 });
  const calls = fs.readFileSync(path.join(work, "calls.txt"), "utf8").split("\n").slice(callsBefore - 1);
  assert.ok(calls.some(l => / mkv /.test(" " + l + " ")), "the chosen titles are ripped: " + calls.join(" | "));
  await page.screenshot({ path: path.join(shots, "log.png"), fullPage: true });
  await titleJob.getByRole("button", { name: "Cancel" }).click();
  await page.locator("#jobs .empty").waitFor({ timeout: 20000 });
  await page.waitForTimeout(2500);
  assert.equal(await page.locator("#history .card").count(), 2, "both jobs are in Recent");
  await page.locator("#history .card .clickable").first().click();
  await page.locator("#history .details", { hasText: "file(s)" }).first().waitFor();
  assert.equal(await error.textContent(), "", "no error after ripping chosen titles");

  // --- Settings: switch automatic rips and the mode of the default configuration. ---
  const defaults = page.locator("#settings .card").first();
  await defaults.locator("select").selectOption("backup");
  await defaults.locator("input[type=checkbox]").check();
  for (let i = 0; i < 50; i++) {
    const st = JSON.parse((await request(base, "GET", `/api/status?token=${token}`)).body).settings.drives[0];
    if (st.mode === "backup" && st.autoRip) break;
    assert.ok(i < 49, "the settings were saved");
    await page.waitForTimeout(100);
  }
  await defaults.locator("input[type=checkbox]").uncheck();
  await defaults.locator("select").selectOption("mkv");
  await page.waitForTimeout(500);
  assert.equal(await error.textContent(), "", "no error after changing the settings");

  // --- Eject and close tray answer without an error (the devices don't exist here, as with an absent drive). ---
  await discCard.getByRole("button", { name: "Eject" }).click();
  await page.getByRole("button", { name: "Close tray" }).first().click();
  await page.waitForTimeout(500);
  assert.equal(await error.textContent(), "", "eject / close tray give no error");

  // --- Archive check: one good and one damaged folder. ---
  for (const [folder, content, listed] of [["Good Movie", "good", "good"], ["Damaged Movie", "changed", "original"]]) {
    fs.mkdirSync(path.join(output, folder), { recursive: true });
    fs.writeFileSync(path.join(output, folder, "title.mkv"), content);
    const hash = crypto.createHash("sha256").update(listed).digest("hex");
    fs.writeFileSync(path.join(output, folder, "SHA256SUMS"), `${hash}  title.mkv\n`);
  }
  await page.getByRole("button", { name: "Check output folder" }).click();
  await page.locator("#verify", { hasText: "2 folder(s), 1 damaged" }).waitFor({ timeout: 20000 });
  await page.locator("#verify", { hasText: "Damaged Movie" }).waitFor();
  assert.equal(await error.textContent(), "", "no error after the archive check");

  // --- Dark mode follows the system. ---
  await page.emulateMedia({ colorScheme: "dark" });
  assert.equal(await page.evaluate(() => getComputedStyle(document.body).backgroundColor), "rgb(22, 22, 23)", "dark background");
  await page.screenshot({ path: path.join(shots, "dark.png"), fullPage: true });
  await page.emulateMedia({ colorScheme: "light" });
  assert.equal(await page.evaluate(() => getComputedStyle(document.body).backgroundColor), "rgb(246, 246, 244)", "light background");

  // --- Phone width: nothing wider than the screen, buttons still reachable. ---
  const phone = await browser.newPage({ viewport: { width: 375, height: 812 }, isMobile: true, hasTouch: true, deviceScaleFactor: 2 });
  await phone.goto(`${base}/?token=${token}`);
  await phone.locator("#drives .card", { hasText: "MOVIE_DISC" }).waitFor();
  const widths = await phone.evaluate(() => ({ page: document.documentElement.scrollWidth, screen: window.innerWidth }));
  assert.ok(widths.page <= widths.screen, `no sideways scrolling at 375 px (page is ${widths.page} px wide)`);
  for (const b of await phone.getByRole("button").all()) {
    const box = await b.boundingBox();
    assert.ok(box && box.x >= 0 && box.x + box.width <= 375, `button “${await b.textContent()}” fits the screen`);
  }
  await phone.screenshot({ path: path.join(shots, "phone.png"), fullPage: true });
  await phone.emulateMedia({ colorScheme: "dark" });
  await phone.screenshot({ path: path.join(shots, "phone-dark.png"), fullPage: true });

  // --- HTTPS with a certificate and key (PEM). ---
  const tlsDir = path.join(work, "tls");
  fs.mkdirSync(tlsDir);
  const cert = path.join(tlsDir, "cert.pem"), key = path.join(tlsDir, "key.pem");
  execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=localhost"],
               { stdio: "ignore" });
  const secure = start("https", 51393, token, { cert, key });
  const context = await browser.newContext({ ignoreHTTPSErrors: true });
  const tlsPage = await context.newPage();
  for (let i = 0; i < 100; i++) {
    try { await tlsPage.goto(`${secure}/?token=${token}`, { timeout: 2000 }); break; } catch { await new Promise(r => setTimeout(r, 100)); }
  }
  await tlsPage.locator("#drives .card", { hasText: "MOVIE_DISC" }).waitFor({ timeout: 20000 });
  assert.equal(new URL(tlsPage.url()).protocol, "https:", "the page is served over HTTPS");
  await tlsPage.screenshot({ path: path.join(shots, "https.png"), fullPage: true });
  await context.close();

  console.log("Web page checks passed; screenshots in " + shots);
} catch (e) {
  failed = true;
  console.error(e);
} finally {
  await browser.close();
  for (const c of children) c.kill("SIGTERM");
  if (fs.existsSync(path.join(work, "calls.txt"))) fs.copyFileSync(path.join(work, "calls.txt"), path.join(shots, "makemkvcon-calls.txt"));
}
process.exit(failed ? 1 : 0);
