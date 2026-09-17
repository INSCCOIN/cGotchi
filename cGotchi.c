/* cGotchi — lightweight ncurses Wi-Fi companion. */
#include <ctype.h>
#include <errno.h>
#include <ncurses.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH "/home/working/ogotchi_log.txt"
#define MAX_SSID 1024
#define MAX_IP 256
#define MAX_ROW 48
#define AGE_OUT 3

enum { M_CUR, M_HAP, M_EXC, M_BOR, M_SUR, M_COO };

typedef struct {
    char name[24];
    time_t born, last;
    float age_h, bor, exc, en;
    int mood, auto_on, hide_open;
    unsigned scans, opens, wpa3, wow;
    int last_n, last_ok;
    char last_msg[40];
} Pet;

typedef struct {
    char ssid[48], bssid[24], sec[20], freq[12];
    int sig, is_new;
    unsigned seen;
} Net;

typedef struct {
    char k[80];
} Slot;

static Pet P;
static Slot Ssid[MAX_SSID], Ips[MAX_IP];
static int nSsid, nIp;
static Net rows[MAX_ROW];
static int nrows;
static char status[80];
static int dirty = 1, cur, show_log;

static int known(Slot *t, int *n, int cap, const char *k)
{
    int i;
    for (i = 0; i < *n; i++)
        if (!strcmp(t[i].k, k))
            return 1;
    if (*n >= cap)
        return 1;
    snprintf(t[*n].k, sizeof t[0].k, "%s", k);
    (*n)++;
    return 0;
}

static void save_path(char *b, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(b, n, "%s/.cgotchi", h && *h ? h : ".");
}

static void pet_default(void)
{
    memset(&P, 0, sizeof P);
    snprintf(P.name, sizeof P.name, "Observer");
    P.born = P.last = time(NULL);
    P.bor = 20;
    P.exc = 40;
    P.en = 80;
    P.mood = M_CUR;
    P.last_ok = 1;
}

static void pet_save(void)
{
    char path[256], tmp[280];
    FILE *f;
    save_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%s\n%ld %ld\n%.3f %.1f %.1f %.1f\n%d %d\n%u %u %u %u\n",
            P.name, (long)P.born, (long)P.last, P.age_h, P.bor, P.exc, P.en,
            P.mood, P.auto_on, P.scans, P.opens, P.wpa3, P.wow);
    fclose(f);
    rename(tmp, path);
}

static void pet_load(void)
{
    char path[256];
    FILE *f;
    long b, l;
    pet_default();
    save_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(P.name, sizeof P.name, f)) {
        fclose(f);
        return;
    }
    P.name[strcspn(P.name, "\n")] = 0;
    if (fscanf(f, "%ld %ld", &b, &l) == 2) {
        P.born = (time_t)b;
        P.last = (time_t)l;
    }
    if (fscanf(f, "%f %f %f %f", &P.age_h, &P.bor, &P.exc, &P.en) != 4) {
    }
    if (fscanf(f, "%d %d", &P.mood, &P.auto_on) != 2)
        P.mood = M_CUR;
    if (fscanf(f, "%u %u %u %u", &P.scans, &P.opens, &P.wpa3, &P.wow) != 4) {
    }
    fclose(f);
}

static void tick(void)
{
    time_t n = time(NULL);
    float dh = (float)(n - P.last) / 3600.f;
    if (dh < 0)
        dh = 0;
    if (dh > 12)
        dh = 12;
    P.age_h += dh;
    P.bor += dh * 8.f;
    if (P.bor > 100)
        P.bor = 100;
    P.exc -= dh * 5.f;
    if (P.exc < 0)
        P.exc = 0;
    P.en -= dh * 2.f;
    if (P.en < 10)
        P.en = 10;
    P.last = n;
}

static void log_line(const char *kind, const char *val, const char *ex)
{
    FILE *f;
    char ts[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    mkdir("/home/working", 0755);
    f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", tm);
    fprintf(f, "%s %s  %s  %s\n", ts, kind, val, ex ? ex : "");
    fclose(f);
}

static void log_preload(void)
{
    FILE *f;
    char line[256], kind[16], val[80];
    f = fopen(LOG_PATH, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%*s %15s %79s", kind, val) < 2)
            continue;
        if (!strcmp(kind, "SSID"))
            known(Ssid, &nSsid, MAX_SSID, val);
        else if (!strcmp(kind, "IP"))
            known(Ips, &nIp, MAX_IP, val);
    }
    fclose(f);
}

static void capture_ips(void)
{
    FILE *p = popen("ip -4 -o addr show 2>/dev/null", "r");
    char line[256];
    if (!p)
        return;
    while (fgets(line, sizeof line, p)) {
        char iface[32], ip[64], extra[80], *sl;
        if (sscanf(line, "%*d: %31s inet %63s", iface, ip) < 2)
            continue;
        sl = strchr(ip, '/');
        if (sl)
            *sl = 0;
        if (!known(Ips, &nIp, MAX_IP, ip)) {
            snprintf(extra, sizeof extra, "%s", iface);
            log_line("IP", ip, extra);
        }
    }
    pclose(p);
}

static int split_g(char *line, char **out, int max)
{
    int n = 0;
    char *r = line, *w = line;
    out[0] = line;
    while (*r && n < max) {
        if (*r == '\\' && r[1]) {
            *w++ = r[1];
            r += 2;
            continue;
        }
        if (*r == ':') {
            *w++ = 0;
            r++;
            if (++n < max)
                out[n] = w;
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
    return n + (*out[n] || n == 0 ? 1 : 0);
}

static int cmp_sig(const void *a, const void *b)
{
    const Net *x = a, *y = b;
    return y->sig - x->sig;
}

static Net *find_bssid(const char *b)
{
    int i;
    for (i = 0; i < nrows; i++)
        if (!strcmp(rows[i].bssid, b))
            return &rows[i];
    return NULL;
}

static void age_rows(void)
{
    int i, o = 0;
    for (i = 0; i < nrows; i++) {
        if (P.scans - rows[i].seen <= AGE_OUT)
            rows[o++] = rows[i];
    }
    nrows = o;
}

static int scan_wifi(int rescan)
{
    FILE *p;
    char line[512];
    int n = 0, neu = 0, op = 0, w3 = 0;
    P.last_ok = 1;
    P.last_msg[0] = 0;
    if (access("/usr/bin/nmcli", X_OK) && access("/bin/nmcli", X_OK)) {
        P.last_ok = 0;
        snprintf(P.last_msg, sizeof P.last_msg, "no nmcli");
        return 0;
    }
    if (rescan)
        (void)!system("nmcli -w 8 device wifi rescan >/dev/null 2>&1");
    p = popen("nmcli -g SSID,BSSID,SIGNAL,SECURITY,FREQ device wifi list 2>/dev/null", "r");
    if (!p) {
        P.last_ok = 0;
        snprintf(P.last_msg, sizeof P.last_msg, "wifi quiet");
        return 0;
    }
    while (fgets(line, sizeof line, p)) {
        char *f[6], key[40], extra[96];
        int nf, hidden, is_open, is_wpa3, is_new;
        Net *r, tmp;
        line[strcspn(line, "\n")] = 0;
        if (!line[0] || !strncmp(line, "Error", 5))
            continue;
        nf = split_g(line, f, 5);
        if (nf < 3)
            continue;
        memset(&tmp, 0, sizeof tmp);
        hidden = !f[0][0];
        snprintf(tmp.ssid, sizeof tmp.ssid, "%s", hidden ? "<Hidden>" : f[0]);
        snprintf(tmp.bssid, sizeof tmp.bssid, "%s", f[1]);
        tmp.sig = atoi(f[2]);
        snprintf(tmp.sec, sizeof tmp.sec, "%s", nf >= 4 && f[3][0] ? f[3] : "Open");
        snprintf(tmp.freq, sizeof tmp.freq, "%s", nf >= 5 ? f[4] : "");
        tmp.seen = P.scans;
        snprintf(key, sizeof key, "%s", tmp.bssid[0] ? tmp.bssid : tmp.ssid);
        is_open = !tmp.sec[0] || !strcmp(tmp.sec, "--") || !strcasecmp(tmp.sec, "open");
        is_wpa3 = !!strstr(tmp.sec, "WPA3") || !!strstr(tmp.sec, "wpa3");
        is_new = !known(Ssid, &nSsid, MAX_SSID, key);
        tmp.is_new = is_new;
        if (is_open)
            op++;
        if (is_wpa3)
            w3++;
        if (is_new) {
            neu++;
            snprintf(extra, sizeof extra, "%d %s %s %s", tmp.sig, tmp.sec, tmp.freq, tmp.bssid);
            log_line("SSID", hidden ? tmp.bssid : tmp.ssid, extra);
        }
        r = tmp.bssid[0] ? find_bssid(tmp.bssid) : NULL;
        if (r)
            *r = tmp;
        else if (nrows < MAX_ROW)
            rows[nrows++] = tmp;
        n++;
    }
    pclose(p);
    age_rows();
    qsort(rows, (size_t)nrows, sizeof(Net), cmp_sig);
    if (cur >= nrows)
        cur = nrows ? nrows - 1 : 0;
    P.last_n = n;
    P.opens += (unsigned)op;
    P.wpa3 += (unsigned)w3;
    if (!P.last_ok || n == 0) {
        P.mood = M_BOR;
        P.bor += 10;
        if (P.bor > 100)
            P.bor = 100;
        P.exc -= 6;
        if (P.exc < 0)
            P.exc = 0;
    } else if (neu > 3) {
        P.mood = M_EXC;
        P.exc += 20;
        if (P.exc > 100)
            P.exc = 100;
        P.bor -= 18;
        if (P.bor < 0)
            P.bor = 0;
        P.wow++;
    } else if (op > 0) {
        P.mood = M_SUR;
        P.exc += 12;
        if (P.exc > 100)
            P.exc = 100;
    } else if (neu > 0) {
        P.mood = M_CUR;
        P.bor -= 8;
        if (P.bor < 0)
            P.bor = 0;
    } else if (P.bor > 60)
        P.mood = M_BOR;
    else
        P.mood = (n % 2) ? M_HAP : M_COO;
    snprintf(status, sizeof status, "scan %d  new %d  open %d", n, neu, op);
    return n;
}

static void observe(int rescan)
{
    tick();
    P.scans++;
    scan_wifi(rescan);
    capture_ips();
    pet_save();
    dirty = 1;
}

static const char *face(void)
{
    if (P.en < 20)
        return "(-_-)z";
    if (P.exc > 75)
        return "(^_^)";
    if (P.bor > 70)
        return "(-_-)";
    if (P.mood == M_SUR)
        return "(O_O)";
    if (P.mood == M_EXC || P.mood == M_HAP)
        return "(^_^)";
    if (P.mood == M_COO)
        return "(._.)";
    return "(o_o)";
}

static const char *moodn(void)
{
    static const char *n[] = {"curious", "happy", "excited", "bored", "surprised", "cool"};
    return n[P.mood > 5 ? 0 : P.mood];
}

static const char *speak(void)
{
    if (!P.last_ok)
        return "Radio quiet. Sitting still.";
    if (P.mood == M_EXC)
        return "New signals on this block.";
    if (P.mood == M_BOR)
        return "Same nets. Walk somewhere.";
    if (P.mood == M_SUR)
        return "Open AP. Noted.";
    if (P.mood == M_HAP)
        return "Airwaves feel friendly.";
    return "Spectrum has secrets.";
}

static void put(int y, int x, const char *s, int n, int attr)
{
    int h, w, room;
    getmaxyx(stdscr, h, w);
    if (y < 0 || x < 0 || y >= h || x >= w || n <= 0)
        return;
    room = w - x - (y == h - 1 ? 1 : 0);
    if (n > room)
        n = room;
    if (n <= 0)
        return;
    attron(attr);
    mvaddnstr(y, x, s, n);
    attroff(attr);
}

static void fill(int y, int x, int n, int attr)
{
    int i;
    for (i = 0; i < n; i++)
        put(y, x + i, " ", 1, attr);
}

static void bar_at(int y, int x, int n, float p, int attr)
{
    char b[16];
    int i, k;
    if (n > 12)
        n = 12;
    k = (int)(p / 100.f * n + 0.5f);
    if (k < 0)
        k = 0;
    if (k > n)
        k = n;
    for (i = 0; i < n; i++)
        b[i] = (char)(i < k ? '=' : '.');
    b[n] = 0;
    put(y, x, b, n, attr);
}

static int prompt(const char *title, char *out, size_t n)
{
    int h, w;
    getmaxyx(stdscr, h, w);
    echo();
    curs_set(1);
    fill(h / 2, 0, w, COLOR_PAIR(2));
    put(h / 2, 1, title, 8, COLOR_PAIR(2));
    wmove(stdscr, h / 2, 10);
    out[0] = 0;
    wgetnstr(stdscr, out, (int)n - 1);
    noecho();
    curs_set(0);
    return out[0] != 0;
}

static void draw(void)
{
    int h, w, mid, lw, rw, list0, list_h, i, y;
    char buf[64], linebuf[160];
    if (!dirty)
        return;
    getmaxyx(stdscr, h, w);
    if (w < 20)
        w = 20;
    mid = w / 2;
    if (mid < 16)
        mid = 16;
    if (mid > w - 10)
        mid = w / 2;
    lw = mid;
    rw = w - mid - 1;
    if (rw < 8)
        rw = 8;
    list0 = 6;
    list_h = h - list0 - 3;
    if (list_h < 1)
        list_h = 1;

    erase();
    fill(0, 0, w, COLOR_PAIR(2));
    snprintf(buf, sizeof buf, "cGotchi %s", face());
    put(0, 1, buf, lw - 2, COLOR_PAIR(2));
    snprintf(buf, sizeof buf, "%s %s", moodn(), P.auto_on ? "AUTO" : "");
    put(0, mid + 1, buf, rw, COLOR_PAIR(2));

    snprintf(buf, sizeof buf, "%s  %.1fd", P.name, P.age_h / 24.f);
    put(1, 1, buf, w - 2, COLOR_PAIR(1));
    put(2, 1, "bor", 3, COLOR_PAIR(1));
    bar_at(2, 5, 8, P.bor, COLOR_PAIR(1));
    put(3, 1, "exc", 3, COLOR_PAIR(1));
    bar_at(3, 5, 8, P.exc, COLOR_PAIR(1));
    put(4, 1, "en ", 3, COLOR_PAIR(1));
    bar_at(4, 5, 8, P.en, COLOR_PAIR(1));
    snprintf(buf, sizeof buf, "n%d i%d s%u", nSsid, nIp, P.scans);
    put(2, mid + 1, buf, rw, COLOR_PAIR(1));
    put(3, mid + 1, speak(), rw, COLOR_PAIR(1));
    put(5, 0, "AP", 2, COLOR_PAIR(2));
    fill(5, 0, lw, COLOR_PAIR(2));
    put(5, 1, "AP", 2, COLOR_PAIR(2));
    fill(5, mid + 1, rw, COLOR_PAIR(2));
    put(5, mid + 2, show_log ? "LOG" : "IP", 3, COLOR_PAIR(2));

    y = list0;
    for (i = 0; i < nrows && y < list0 + list_h; i++) {
        int attr = (i == cur) ? COLOR_PAIR(2) : (rows[i].is_new ? COLOR_PAIR(3) : COLOR_PAIR(1));
        if (P.hide_open && (!rows[i].sec[0] || !strcmp(rows[i].sec, "Open")))
            continue;
        snprintf(buf, sizeof buf, "%c%-10.10s %3d", rows[i].is_new ? '*' : ' ',
                 rows[i].ssid, rows[i].sig);
        put(y++, 0, buf, lw - 1, attr);
    }
    y = list0;
    if (show_log) {
        FILE *lf = fopen(LOG_PATH, "r");
        char ring[6][160];
        int rn = 0, k;
        memset(ring, 0, sizeof ring);
        if (lf) {
            while (fgets(linebuf, sizeof linebuf, lf)) {
                snprintf(ring[rn % 6], sizeof ring[0], "%s", linebuf);
                rn++;
            }
            fclose(lf);
        }
        k = rn > 6 ? rn - 6 : 0;
        for (; k < rn && y < list0 + list_h; k++) {
            char *s = ring[k % 6];
            s[strcspn(s, "\n")] = 0;
            put(y++, mid + 1, s, rw, COLOR_PAIR(1));
        }
    } else {
        for (i = 0; i < nIp && y < list0 + list_h; i++)
            put(y++, mid + 1, Ips[i].k, rw, COLOR_PAIR(1));
    }
    for (y = list0; y < list0 + list_h; y++)
        mvaddch(y, mid, ACS_VLINE | COLOR_PAIR(1));

    fill(h - 2, 0, w, COLOR_PAIR(1));
    if (nrows && cur >= 0 && cur < nrows) {
        snprintf(buf, sizeof buf, "%s  %s  %s", rows[cur].bssid, rows[cur].sec, rows[cur].freq);
        put(h - 2, 1, buf, w - 3, COLOR_PAIR(1));
    } else
        put(h - 2, 1, status[0] ? status : LOG_PATH, w - 3, COLOR_PAIR(1));
    fill(h - 1, 0, w, COLOR_PAIR(2));
    put(h - 1, 1, "up/dn  s R  a  l log  n  o  q", w - 3, COLOR_PAIR(2));
    refresh();
    dirty = 0;
}

int main(void)
{
    struct pollfd pfd = {.fd = 0, .events = POLLIN};
    pet_load();
    log_preload();
    capture_ips();
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_BLACK, COLOR_GREEN);
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);
        bkgd(COLOR_PAIR(1));
    }
    snprintf(status, sizeof status, "s look  R radio");
    dirty = 1;
    draw();
    for (;;) {
        int timeout = P.auto_on ? 25000 : 500;
        if (poll(&pfd, 1, timeout) > 0) {
            int k = getch();
            if (k == 'q' || k == 'Q')
                break;
            if (k == KEY_UP) {
                if (cur > 0)
                    cur--;
                dirty = 1;
            } else if (k == KEY_DOWN) {
                if (cur + 1 < nrows)
                    cur++;
                dirty = 1;
            } else if (k == 's')
                observe(0);
            else if (k == 'R')
                observe(1);
            else if (k == 'l' || k == 'L') {
                show_log ^= 1;
                dirty = 1;
            }
            else if (k == 'a' || k == 'A') {
                P.auto_on ^= 1;
                pet_save();
                snprintf(status, sizeof status, P.auto_on ? "auto on" : "auto off");
                dirty = 1;
            } else if (k == 'n' || k == 'N') {
                char nm[24];
                if (prompt("name", nm, sizeof nm)) {
                    snprintf(P.name, sizeof P.name, "%s", nm);
                    pet_save();
                }
                dirty = 1;
            } else if (k == 'o' || k == 'O') {
                P.hide_open ^= 1;
                snprintf(status, sizeof status, P.hide_open ? "hide open" : "show open");
                dirty = 1;
            } else if (k == 'i' || k == 'I') {
                capture_ips();
                snprintf(status, sizeof status, "ips %d", nIp);
                dirty = 1;
            } else if (k == KEY_RESIZE)
                dirty = 1;
        } else if (P.auto_on)
            observe(0);
        draw();
    }
    endwin();
    pet_save();
    return 0;
}
