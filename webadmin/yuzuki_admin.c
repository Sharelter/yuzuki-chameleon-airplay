#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LISTEN_PORT 8080
#define SETTINGS_PATH "/etc/yuzuki-admin.conf"
#define SETTINGS_TMP "/etc/yuzuki-admin.conf.tmp"
#define ASOUND_PATH "/etc/asound.conf"
#define ASOUND_TMP "/etc/asound.conf.tmp"

typedef struct {
  char audio[16];
  char display[8];
} settings_t;

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) { (void)sig; running = 0; }

static int valid_audio(const char *value) {
  return !strcmp(value, "lineout") || !strcmp(value, "hdmi");
}

static int valid_display(const char *value) {
  return !strcmp(value, "on") || !strcmp(value, "off");
}

static void load_settings(settings_t *settings) {
  strcpy(settings->audio, "lineout");
  strcpy(settings->display, "on");
  FILE *file = fopen(SETTINGS_PATH, "r");
  if (!file) return;
  char line[64];
  while (fgets(line, sizeof(line), file)) {
    char *equals = strchr(line, '=');
    if (!equals) continue;
    *equals++ = 0;
    equals[strcspn(equals, "\r\n")] = 0;
    if (!strcmp(line, "audio") && valid_audio(equals))
      snprintf(settings->audio, sizeof(settings->audio), "%s", equals);
    else if (!strcmp(line, "display") && valid_display(equals))
      snprintf(settings->display, sizeof(settings->display), "%s", equals);
  }
  fclose(file);
}

static int save_settings(const settings_t *settings) {
  FILE *file = fopen(SETTINGS_TMP, "w");
  if (!file) return -1;
  fprintf(file, "audio=%s\ndisplay=%s\n", settings->audio, settings->display);
  if (fclose(file) != 0) return -1;
  return rename(SETTINGS_TMP, SETTINGS_PATH);
}

static int write_asound(const char *audio) {
  int card = !strcmp(audio, "hdmi") ? 2 : 0;
  FILE *file = fopen(ASOUND_TMP, "w");
  if (!file) return -1;
  fprintf(file,
          "# Managed by Yuzuki admin. Do not edit while the service is running.\n"
          "pcm.airplay {\n"
          "    type plug\n"
          "    slave {\n"
          "        pcm \"hw:%d,0\"\n"
          "        format S16_LE\n"
          "        channels 2\n"
          "        rate 48000\n"
          "    }\n"
          "}\n\n"
          "ctl.airplay {\n"
          "    type hw\n"
          "    card %d\n"
          "}\n", card, card);
  if (fclose(file) != 0) return -1;
  return rename(ASOUND_TMP, ASOUND_PATH);
}

static void run_service_command(const char *command) {
  if (system(command) != 0)
    fprintf(stderr, "Service command failed: %s\n", command);
}

static void apply_settings(const settings_t *settings) {
  if (write_asound(settings->audio) == 0) {
    if (!strcmp(settings->audio, "lineout")) {
      run_service_command("amixer -q -c 2 set I2S1OUT off");
      run_service_command("amixer -q -c 0 set LINEOUT on");
      run_service_command("amixer -q -c 0 set 'Left Output Mixer DACL' on");
      run_service_command("amixer -q -c 0 set 'Right Output Mixer DACR' on");
      run_service_command("amixer -q -c 0 set 'LINEOUT volume' 31");
    } else {
      run_service_command("amixer -q -c 2 set 'I2S1 Src Select' 'APBIF_TXDIF0'");
      run_service_command("amixer -q -c 2 set I2S1OUT on");
    }
    /* The watchdog in airplay_boot restarts the receiver within five seconds. */
    run_service_command("killall shairport-sync >/dev/null 2>&1");
  }
  if (!strcmp(settings->display, "on"))
    run_service_command("/bin/ash /etc/rc.common /etc/init.d/nowplaying start >/dev/null 2>&1");
  else
    run_service_command("/bin/ash /etc/rc.common /etc/init.d/nowplaying stop >/dev/null 2>&1");
}

static void get_ip(char *out, size_t out_size) {
  snprintf(out, out_size, "unavailable");
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return;
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
  if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
    struct sockaddr_in *address = (struct sockaddr_in *)&ifr.ifr_addr;
    snprintf(out, out_size, "%s", inet_ntoa(address->sin_addr));
  }
  close(sock);
}

static void url_decode(char *text) {
  char *in = text, *out = text;
  while (*in) {
    if (*in == '+' ) { *out++ = ' '; in++; }
    else if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
      char hex[3] = { in[1], in[2], 0 };
      *out++ = (char)strtol(hex, NULL, 16);
      in += 3;
    } else *out++ = *in++;
  }
  *out = 0;
}

static void parse_query(char *query, settings_t *settings) {
  for (char *part = strtok(query, "&"); part; part = strtok(NULL, "&")) {
    char *equals = strchr(part, '=');
    if (!equals) continue;
    *equals++ = 0;
    url_decode(part);
    url_decode(equals);
    if (!strcmp(part, "audio") && valid_audio(equals))
      snprintf(settings->audio, sizeof(settings->audio), "%s", equals);
    else if (!strcmp(part, "display") && valid_display(equals))
      snprintf(settings->display, sizeof(settings->display), "%s", equals);
  }
}

static void send_response(int client, const char *type, const char *body) {
  dprintf(client,
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: %s; charset=utf-8\r\n"
          "Cache-Control: no-store\r\n"
          "Connection: close\r\n\r\n%s", type, body);
}

static void send_page(int client, const settings_t *settings, const char *notice) {
  char ip[64];
  get_ip(ip, sizeof(ip));
  char page[8192];
  snprintf(page, sizeof(page),
    "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Yuzuki Chameleon</title><style>"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 80%% 0,#3e1842,#101018 55%%);color:#f7f5fa;font:16px -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;display:grid;place-items:center;padding:22px}"
    ".card{width:min(540px,100%%);background:#1b1923dd;border:1px solid #ffffff22;border-radius:24px;padding:28px;box-shadow:0 24px 70px #0007}.brand{font-size:13px;letter-spacing:.13em;text-transform:uppercase;color:#c7b9cf}.title{font-size:31px;margin:6px 0 3px}.sub{color:#c8c1ce;margin:0 0 25px}.row{padding:18px 0;border-top:1px solid #ffffff18}.label{font-weight:650;display:block;margin-bottom:10px}.hint{font-size:13px;color:#b5aebc;line-height:1.45;margin:8px 0 0}.options{display:flex;gap:10px}.option{flex:1}.option input{position:absolute;opacity:0}.option span{display:block;text-align:center;padding:12px 8px;border:1px solid #ffffff2b;border-radius:13px;color:#cec7d2}.option input:checked+span{background:#fa3d62;border-color:#ff7a92;color:white;box-shadow:0 7px 20px #f23d5a55}.toggle{display:flex;gap:10px}.toggle .option{max-width:130px}button{margin-top:22px;width:100%%;border:0;border-radius:14px;padding:14px;background:linear-gradient(135deg,#ff607b,#fa244d);color:white;font-size:16px;font-weight:700}.notice{margin:0 0 18px;padding:11px 13px;border-radius:12px;background:#2b523c;color:#b7f0ca}.footer{margin-top:22px;font-size:13px;color:#aaa2b0;text-align:center}</style></head><body><main class=\"card\">"
    "<div class=\"brand\">Yuzuki Chameleon</div><div class=\"title\">AirPlay control</div>"
    "<p class=\"sub\">Connected at %s &middot; settings are saved across reboot.</p>%s"
    "<form action=\"/apply\" method=\"get\"><section class=\"row\"><span class=\"label\">Audio output</span><div class=\"options\">"
    "<label class=\"option\"><input type=\"radio\" name=\"audio\" value=\"lineout\" %s><span>3.5 mm jack</span></label>"
    "<label class=\"option\"><input type=\"radio\" name=\"audio\" value=\"hdmi\" %s><span>HDMI monitor</span></label></div>"
    "<p class=\"hint\">Changing the route briefly restarts AirPlay, then it becomes available again.</p></section>"
    "<section class=\"row\"><span class=\"label\">Now-playing display</span><div class=\"toggle\">"
    "<label class=\"option\"><input type=\"radio\" name=\"display\" value=\"on\" %s><span>On</span></label>"
    "<label class=\"option\"><input type=\"radio\" name=\"display\" value=\"off\" %s><span>Off</span></label></div>"
    "<p class=\"hint\">Turning this off releases the HDMI framebuffer while audio can continue through HDMI.</p></section>"
    "<button type=\"submit\">Apply settings</button></form><div class=\"footer\">LAN-only control &middot; http://%s:%d</div></main></body></html>",
    ip, notice ? notice : "", !strcmp(settings->audio, "lineout") ? "checked" : "",
    !strcmp(settings->audio, "hdmi") ? "checked" : "", !strcmp(settings->display, "on") ? "checked" : "",
    !strcmp(settings->display, "off") ? "checked" : "", ip, LISTEN_PORT);
  send_response(client, "text/html", page);
}

static void serve_client(int client) {
  char request[8192];
  ssize_t received = read(client, request, sizeof(request) - 1);
  if (received <= 0) return;
  request[received] = 0;
  char method[8], target[2048];
  if (sscanf(request, "%7s %2047s", method, target) != 2) return;

  settings_t settings;
  load_settings(&settings);
  if (!strncmp(target, "/apply?", 7)) {
    char query[2048];
    snprintf(query, sizeof(query), "%s", target + 7);
    parse_query(query, &settings);
    const char *notice = save_settings(&settings) == 0 ?
      "Settings applied. AirPlay may take a few seconds to reappear." :
      "Unable to save settings.";
    if (!strncmp(notice, "Settings", 8)) apply_settings(&settings);
    send_page(client, &settings, notice);
  } else if (!strcmp(target, "/health")) {
    send_response(client, "text/plain", "ok\n");
  } else {
    send_page(client, &settings, NULL);
  }
}

int main(void) {
  signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
  int server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) return 1;
  int enabled = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(LISTEN_PORT);
  if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 8) < 0)
    return 2;
  while (running) {
    int client = accept(server, NULL, NULL);
    if (client < 0) { if (errno == EINTR) continue; break; }
    serve_client(client);
    close(client);
  }
  close(server);
  return 0;
}
