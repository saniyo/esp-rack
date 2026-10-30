// esprack-loader: the HTTP server.
//   GET  /        the page (status, upload, back to the app)
//   GET  /status  JSON for the page
//   POST /upload  the image (or a signed .bin) as the raw body
//   POST /back    boot the old app again (when it is still there)
// The POSTs need the admin password in "X-Password" (URI-encoded UTF-8),
// checked against the handoff's adminHash (hex SHA-256). No hash (no
// handoff, or the app had none): open, and the page says so.
// Handlers run on the server's task; an upload streams into the
// ImageWriter there, under Session::writer.

#include <stdio.h>
#include <string.h>

#include <esp_http_server.h>
#include <esp_log.h>

#include "image_rules.h"
#include "image_writer.h"
#include "loader.h"
#include "sig_verify.h"

namespace loader {

namespace {

const char* const TAG = "loader";
httpd_handle_t s_server = nullptr;
uint8_t s_rx[2048];  // the upload's (one at a time: Session::writer)

const char kPage[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>esp-rack loader</title>"
    "<style>body{font:15px system-ui,sans-serif;max-width:34em;margin:1em auto;padding:0 1em}"
    "td{padding:2px 8px 2px 0}progress{width:100%}.e{color:#b00}.w{color:#a60}"
    "button,input{font:inherit;margin:4px 0}</style></head><body>"
    "<h2>esp-rack loader</h2><table>"
    "<tr><td>State<td id=ph>"
    "<tr><td>App<td id=app>"
    "<tr><td>Wanted<td id=to>"
    "<tr><td>Progress<td id=pb></table>"
    "<progress id=pg value=0 max=1></progress>"
    "<p class=e id=er></p><p class=w id=wr></p>"
    "<p><input type=file id=f accept='.bin'><br>"
    "<span id=pwr><input type=password id=pw placeholder='admin password'><br></span>"
    "<button onclick=up()>Upload and install</button> "
    "<button id=bk onclick=back() hidden>Back to the app</button></p><p id=msg></p>"
    "<script>"
    "const $=i=>document.getElementById(i);"
    "function st(){fetch('/status').then(r=>r.json()).then(s=>{"
    "$('ph').textContent=s.phase+' ('+s.mode+')';$('app').textContent=s.app||'none';"
    "$('to').textContent=s.target||'-';const t=s.total||0;$('pg').max=t||1;$('pg').value=s.bytes;"
    "$('pb').textContent=s.bytes+(t?' / '+t:'')+' B, attempt '+s.attempts+"
    "(s.mode=='pull'&&s.attempts>=s.allowed?' (stuck: pulls go on every 5 min, an upload works too)':'');"
    "$('er').textContent=s.error;$('wr').textContent=s.warn.join(' | ');"
    "$('bk').hidden=!s.appValid;$('pwr').hidden=!s.pw}).catch(()=>{})"
    ".finally(()=>setTimeout(st,2000))}"
    "function send(u,b){const x=new XMLHttpRequest();x.open('POST',u);"
    "x.setRequestHeader('X-Password',encodeURIComponent($('pw').value));"
    "x.upload.onprogress=e=>{$('pg').max=e.total;$('pg').value=e.loaded};"
    "x.onload=()=>$('msg').textContent=x.responseText;"
    "x.onerror=()=>$('msg').textContent='connection lost';x.send(b)}"
    "function up(){const f=$('f').files[0];if(!f)return;$('msg').textContent='uploading...';"
    "send('/upload',f)}"
    "function back(){send('/back',null)}"
    "st()</script></body></html>";

esp_err_t reply(httpd_req_t* req, const char* status, const char* text) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, text);
}

const char* adminHash() { return g.haveHandoff ? g.handoff.adminHash : ""; }

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// %XX in place.
void uriDecode(char* s) {
  char* o = s;
  for (const char* p = s; *p; ++p) {
    if (*p == '%' && hexVal(p[1]) >= 0 && hexVal(p[2]) >= 0) {
      *o++ = char(hexVal(p[1]) << 4 | hexVal(p[2]));
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
}

bool authorized(httpd_req_t* req) {
  const char* want = adminHash();
  if (!want[0]) return true;  // none known: open (the page warns)
  char pw[200];
  if (httpd_req_get_hdr_value_str(req, "X-Password", pw, sizeof(pw)) != ESP_OK) return false;
  uriDecode(pw);
  uint8_t sha[32];
  char hex[65];
  sha256(pw, strlen(pw), sha);
  ota::toHex(sha, sizeof(sha), hex);
  memset(pw, 0, sizeof(pw));
  return hexEqual(hex, want);
}

// A JSON string body: quotes, backslashes and control characters dropped.
void jsonText(char* out, size_t cap, const char* s) {
  size_t n = 0;
  for (; *s && n + 1 < cap; ++s) {
    if (*s == '"' || *s == '\\' || uint8_t(*s) < 0x20) continue;
    out[n++] = *s;
  }
  out[n] = 0;
}

esp_err_t onPage(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}

esp_err_t onStatus(httpd_req_t* req) {
  char err[64], warn[96], e[64], w[96], app[32], to[32];
  copyTexts(err, sizeof(err), warn, sizeof(warn));
  jsonText(e, sizeof(e), err);
  jsonText(w, sizeof(w), warn);
  jsonText(app, sizeof(app), g.appValid ? g.appVersion : "");
  jsonText(to, sizeof(to), g.result.to);
  const uint8_t mode = g.haveHandoff ? g.handoff.mode : 0;
  char warns[256];
  int n = snprintf(warns, sizeof(warns), "\"%s\"", w);
  if (!g.prodKey)
    n += snprintf(warns + n, sizeof(warns) - size_t(n), ",\"dev signing key %s: images are not strictly checked\"",
                  keyId());
  if (!adminHash()[0]) n += snprintf(warns + n, sizeof(warns) - size_t(n), ",\"no admin password: this page is open\"");
  if (net::apOn()) snprintf(warns + n, sizeof(warns) - size_t(n), ",\"open Wi-Fi AP %s\"", net::apName());
  char out[640];
  snprintf(out, sizeof(out),
           "{\"phase\":\"%s\",\"mode\":\"%s\",\"app\":\"%s\",\"appValid\":%s,\"target\":\"%s\",\"bytes\":%u,"
           "\"total\":%u,\"attempts\":%u,\"allowed\":%u,\"error\":\"%s\",\"warn\":[%s],\"pw\":%s}",
           phaseName(phase()), ota::modeName(mode), app, g.appValid ? "true" : "false", to, unsigned(g.bytes.load()),
           unsigned(g.total.load()), unsigned(g.result.attempts),
           unsigned(attemptsAllowed(g.haveHandoff ? g.handoff.attemptsAllowed : 0)), e, warns,
           adminHash()[0] ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, out);
}

esp_err_t onUpload(httpd_req_t* req) {
  if (!authorized(req)) return reply(req, "401 Unauthorized", "wrong admin password");
  const size_t len = req->content_len;
  if (len < kHeadNeeded) return reply(req, "400 Bad Request", "no image");
  if (len > g.slot->size + ota::kTrailerSize) return reply(req, "413 Payload Too Large", "image larger than the app slot");
  // An image in (being activated, or about to be): never written over.
  if (g.imageIn.load() || phase() == Phase::Activating)
    return reply(req, "409 Conflict", "an image is in already: restarting");
  if (xSemaphoreTake(g.writer, 0) != pdTRUE) return reply(req, "409 Conflict", "a download is under way");
  if (g.imageIn.load()) {  // one went in just before
    xSemaphoreGive(g.writer);
    return reply(req, "409 Conflict", "an image is in already: restarting");
  }
  xEventGroupSetBits(g.events, kEvUploadStart);
  setPhase(Phase::Uploading);
  ++g.result.attempts;
  g.bytes.store(0);
  g.total.store(uint32_t(len));
  ESP_LOGI(TAG, "upload: %u B", unsigned(len));
  ImageWriter& w = imageWriter();
  w.begin(g.slot, ImageWriter::Expect{});
  size_t left = len;
  int timeouts = 0;
  bool ok = true;
  while (left) {
    const int n = httpd_req_recv(req, reinterpret_cast<char*>(s_rx), left < sizeof(s_rx) ? left : sizeof(s_rx));
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) {
      setError("upload cut off at %u B", unsigned(len - left));
      ok = false;
      break;
    }
    timeouts = 0;
    left -= size_t(n);
    g.bytes.store(uint32_t(len - left));
    if (!w.feed(s_rx, size_t(n))) {
      setError("%s", w.error());
      ok = false;
      break;
    }
  }
  if (ok && !w.finish()) {
    setError("%s", w.error());
    ok = false;
  }
  if (!ok) {
    if (w.touched()) g.appValid = false;  // ota_0 was written to: no going back
    w.abandon();
    xSemaphoreGive(g.writer);
    xEventGroupSetBits(g.events, kEvUploadEnd);
    char err[64], warn[8];
    copyTexts(err, sizeof(err), warn, sizeof(warn));
    httpd_resp_set_hdr(req, "Connection", "close");
    return reply(req, "400 Bad Request", err);
  }
  g.appValid = false;  // ota_0 now holds the new image
  ota::setText(g.newVersion, w.version());
  if (w.warning()[0]) setWarning("%s", w.warning());
  char msg[96];
  snprintf(msg, sizeof(msg), "%s installed%s%s: restarting", g.newVersion, w.warning()[0] ? ", " : "", w.warning());
  // In before the writer goes: a pull that takes it next finds the image
  // in. The session activates it (kEvDone); its restart stops this server
  // only once this answer is out (httpd_stop waits for the handler).
  g.imageIn.store(true);
  xEventGroupSetBits(g.events, kEvDone);
  xSemaphoreGive(g.writer);
  return reply(req, "200 OK", msg);
}

esp_err_t onBack(httpd_req_t* req) {
  if (!authorized(req)) return reply(req, "401 Unauthorized", "wrong admin password");
  if (!g.appValid) return reply(req, "409 Conflict", "no app to go back to");
  reply(req, "200 OK", "restarting into the app");
  xEventGroupSetBits(g.events, kEvBack);
  return ESP_OK;
}

}  // namespace

bool webStart() {
  if (s_server) return true;
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 8192;  // SHA-256 and the ECDSA check run here for an upload
  c.recv_wait_timeout = 15;
  c.send_wait_timeout = 15;
  c.lru_purge_enable = true;
  c.max_uri_handlers = 4;
  if (httpd_start(&s_server, &c) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server did not start");
    s_server = nullptr;
    return false;
  }
  static const httpd_uri_t kUris[] = {
      {"/", HTTP_GET, onPage, nullptr},
      {"/status", HTTP_GET, onStatus, nullptr},
      {"/upload", HTTP_POST, onUpload, nullptr},
      {"/back", HTTP_POST, onBack, nullptr},
  };
  for (const httpd_uri_t& u : kUris) httpd_register_uri_handler(s_server, &u);
  return true;
}

void webStop() {
  if (s_server) httpd_stop(s_server);
  s_server = nullptr;
}

}  // namespace loader
