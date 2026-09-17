/* cGotchi — passive Wi-Fi companion for SharkDeck. libc only. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define COLS 48
#define MAX_SSID 1024
#define MAX_IP 256
#define LOG_PATH "/home/working/ogotchi_log.txt"
#define SAVE_PATH_HOME ".cgotchi"

static const char *FACES[] = {
    "(._.)", "(o_o)", "(^_^)", "(-_-)", "(O_O)", "(u_u)",
};

enum { M_CURIOUS, M_HAPPY, M_EXCITED, M_BORED, M_SURPRISED, M_COOL, M_N };

typedef struct {
    char name[24];
    time_t born, last;
    float age_h, boredom, excitement, energy;
    int mood;
    unsigned scans, seen, opens, wpa3, wow;
    int last_n, last_ok;
    char last_msg[40];
} Pet;

typedef struct {
    char key[80];
} Slot;

static Pet pet;
static Slot ssids[MAX_SSID];
static int nssid;
static Slot ips[MAX_IP];
static int nip;
static int autoscan;
static struct termios oldt;
static int raw_on;

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int seen_key(Slot *tab, int *n, int cap, const char *key)
{
    int i;
    for (i = 0; i < *n; i++)
        if (!strcmp(tab[i].key, key))
            return 1;
    if (*n >= cap)
        return 1;
    snprintf(tab[*n].key, sizeof tab[0].key, "%s", key);
    (*n)++;
    (void)hash_str;
    return 0;
}

static void home_save(char *out, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/%s", h && *h ? h : ".", SAVE_PATH_HOME);
}

static void pet_default(void)
{
    memset(&pet, 0, sizeof pet);
    snprintf(pet.name, sizeof pet.name, "Observer");
    pet.born = pet.last = time(NULL);
    pet.boredom = 20;
    pet.excitement = 40;
    pet.energy = 80;
    pet.mood = M_CURIOUS;
    pet.last_ok = 1;
}

static void pet_save(void)
{
    char path[256], tmp[280];
    FILE *f;
    home_save(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%s\n%ld %ld\n%.4f %.2f %.2f %.2f\n%d\n%u %u %u %u %u\n",
            pet.name, (long)pet.born, (long)pet.last, pet.age_h,
            pet.boredom, pet.excitement, pet.energy, pet.mood,
            pet.scans, pet.seen, pet.opens, pet.wpa3, pet.wow);
    fclose(f);
    rename(tmp, path);
}

static void pet_load(void)
{
    char path[256], line[64];
    FILE *f;
    long b, l;
    pet_default();
    home_save(path, sizeof path);
    f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(pet.name, sizeof pet.name, f)) {
        fclose(f);
        return;
    }
    pet.name[strcspn(pet.name, "\n")] = 0;
    if (fscanf(f, "%ld %ld", &b, &l) == 2) {
        pet.born = (time_t)b;
        pet.last = (time_t)l;
    }
    fscanf(f, "%f %f %f %f", &pet.age_h, &pet.boredom, &pet.excitement, &pet.energy);
    fscanf(f, "%d", &pet.mood);
    fscanf(f, "%u %u %u %u %u", &pet.scans, &pet.seen, &pet.opens, &pet.wpa3, &pet.wow);
    fclose(f);
    (void)line;
}

static void tick_time(void)
{
    time_t n = time(NULL);
    float dh = (float)(n - pet.last) / 3600.f;
    if (dh < 0)
        dh = 0;
    if (dh > 24)
        dh = 24;
    pet.age_h += dh;
    pet.boredom = pet.boredom + dh * 8.f;
    if (pet.boredom > 100)
        pet.boredom = 100;
    pet.excitement -= dh * 5.f;
    if (pet.excitement < 0)
        pet.excitement = 0;
    pet.energy -= dh * 2.f;
    if (pet.energy < 10)
        pet.energy = 10;
    pet.last = n;
}

static void log_init_dir(void)
{
    mkdir("/home/working", 0755);
}

static void log_append(const char *kind, const char *value, const char *extra)
{
    FILE *f;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    log_init_dir();
    f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", tm);
    fprintf(f, "%s %s  %s  %s\n", ts, kind, value, extra ? extra : "");
    fclose(f);
}

static void log_preload(void)
{
    FILE *f;
    char line[256], kind[16], val[80], extra[80];
    f = fopen(LOG_PATH, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        extra[0] = 0;
        if (sscanf(line, "%*s %15s %79s %79[^\n]", kind, val, extra) < 2)
            continue;
        if (!strcmp(kind, "SSID"))
            seen_key(ssids, &nssid, MAX_SSID, val);
        else if (!strcmp(kind, "IP"))
            seen_key(ips, &nip, MAX_IP, val);
    }
    fclose(f);
}

static int have_nmcli(void)
{
    return access("/usr/bin/nmcli", X_OK) == 0 || access("/bin/nmcli", X_OK) == 0;
}

static void capture_ips(void)
{
    FILE *p;
    char line[256];
    p = popen("ip -4 -o addr show 2>/dev/null", "r");
    if (!p)
        return;
    while (fgets(line, sizeof line, p)) {
        char iface[32] = "", ip[64] = "", extra[80];
        char *slash;
        if (sscanf(line, "%*d: %31s inet %63s", iface, ip) < 2)
            continue;
        slash = strchr(ip, '/');
        if (slash)
            *slash = 0;
        if (!seen_key(ips, &nip, MAX_IP, ip)) {
            snprintf(extra, sizeof extra, "%s", iface);
            log_append("IP", ip, extra);
        }
    }
    pclose(p);
}

static int scan_wifi(int *new_ssid, int *opens, int *wpa3)
{
    FILE *p;
    char line[512];
    int n = 0;
    *new_ssid = *opens = *wpa3 = 0;
    pet.last_ok = 1;
    pet.last_msg[0] = 0;
    if (!have_nmcli()) {
        pet.last_ok = 0;
        snprintf(pet.last_msg, sizeof pet.last_msg, "no nmcli");
        return 0;
    }
    p = popen("nmcli -t -f SSID,BSSID,SIGNAL,SECURITY device wifi list 2>/dev/null", "r");
    if (!p) {
        pet.last_ok = 0;
        snprintf(pet.last_msg, sizeof pet.last_msg, "wifi quiet");
        return 0;
    }
    while (fgets(line, sizeof line, p)) {
        char ssid[64] = "", bssid[32] = "", sec[40] = "", extra[96], key[80];
        char *s, *parts[6];
        int np = 0, sig = 0, is_open, is_wpa3;
        line[strcspn(line, "\n")] = 0;
        if (!line[0] || !strncmp(line, "Error", 5))
            continue;
        s = line;
        while (np < 5) {
            parts[np++] = s;
            s = strchr(s, ':');
            if (!s)
                break;
            *s++ = 0;
        }
        if (np < 3)
            continue;
        snprintf(ssid, sizeof ssid, "%s", parts[0][0] ? parts[0] : "<Hidden>");
        snprintf(bssid, sizeof bssid, "%s", parts[1]);
        sig = atoi(parts[2]);
        if (np >= 4)
            snprintf(sec, sizeof sec, "%s", parts[3][0] ? parts[3] : "Open");
        else
            snprintf(sec, sizeof sec, "Open");
        snprintf(key, sizeof key, "%s|%s", ssid, bssid);
        is_open = (!sec[0] || !strcmp(sec, "--") || !strcasecmp(sec, "open") || !strcasecmp(sec, "none"));
        is_wpa3 = strstr(sec, "WPA3") || strstr(sec, "wpa3");
        if (is_open)
            (*opens)++;
        if (is_wpa3)
            (*wpa3)++;
        if (!seen_key(ssids, &nssid, MAX_SSID, key)) {
            (*new_ssid)++;
            snprintf(extra, sizeof extra, "%d %s %s", sig, sec, bssid);
            log_append("SSID", ssid, extra);
        }
        n++;
        if (n > 64)
            break;
    }
    pclose(p);
    /* best-effort rescan for next time */
    if (n == 0)
        (void)system("nmcli device wifi rescan >/dev/null 2>&1");
    return n;
}

static void observe(void)
{
    int neu, op, w3, n;
    tick_time();
    pet.scans++;
    n = scan_wifi(&neu, &op, &w3);
    capture_ips();
    pet.last_n = n;
    pet.seen += (unsigned)n;
    pet.opens += (unsigned)op;
    pet.wpa3 += (unsigned)w3;
    if (!pet.last_ok || n == 0) {
        pet.mood = M_BORED;
        pet.boredom += 12;
        if (pet.boredom > 100)
            pet.boredom = 100;
        pet.excitement -= 8;
        if (pet.excitement < 0)
            pet.excitement = 0;
    } else if (neu > 3) {
        pet.mood = M_EXCITED;
        pet.excitement += 25;
        if (pet.excitement > 100)
            pet.excitement = 100;
        pet.boredom -= 20;
        if (pet.boredom < 0)
            pet.boredom = 0;
        pet.wow++;
    } else if (op > 0) {
        pet.mood = M_SURPRISED;
        pet.excitement += 15;
        if (pet.excitement > 100)
            pet.excitement = 100;
    } else if (neu > 0) {
        pet.mood = M_CURIOUS;
        pet.boredom -= 10;
        if (pet.boredom < 0)
            pet.boredom = 0;
    } else if (pet.boredom > 60)
        pet.mood = M_BORED;
    else
        pet.mood = (int)(time(NULL) % 3 == 0 ? M_HAPPY : M_COOL);
    pet_save();
}

static const char *mood_name(void)
{
    static const char *n[] = {"curious", "happy", "excited", "bored", "surprised", "cool"};
    return n[pet.mood < M_N ? pet.mood : 0];
}

static const char *face(void)
{
    if (pet.energy < 20)
        return FACES[5];
    if (pet.excitement > 75)
        return FACES[2];
    if (pet.boredom > 70)
        return FACES[3];
    if (pet.mood == M_SURPRISED)
        return FACES[4];
    if (pet.mood == M_HAPPY || pet.mood == M_EXCITED)
        return FACES[2];
    if (pet.mood == M_BORED)
        return FACES[3];
    return FACES[1];
}

static const char *speak(void)
{
    if (!pet.last_ok)
        return "The air is quiet. Or nmcli is napping.";
    if (pet.mood == M_EXCITED)
        return "New signals. I like this block.";
    if (pet.mood == M_BORED)
        return "Same old nets. Take me somewhere else.";
    if (pet.mood == M_SURPRISED)
        return "Open network. Unexpected.";
    if (pet.mood == M_HAPPY)
        return "The airwaves feel friendly.";
    if (pet.mood == M_COOL)
        return "Stay curious.";
    return "The spectrum is full of secrets.";
}

static void bar(char *out, int n, float pct)
{
    int i, k = (int)(pct / 100.f * n + 0.5f);
    if (k < 0)
        k = 0;
    if (k > n)
        k = n;
    for (i = 0; i < n; i++)
        out[i] = i < k ? '#' : '-';
    out[n] = 0;
}

static void clip(char *dst, const char *s)
{
    snprintf(dst, COLS + 1, "%s", s);
    if ((int)strlen(dst) > COLS)
        dst[COLS - 1] = '~', dst[COLS] = 0;
}

static void draw(void)
{
    char b1[16], b2[16], b3[16], line[64];
    int i;
    bar(b1, 10, pet.boredom);
    bar(b2, 10, pet.excitement);
    bar(b3, 10, pet.energy);
    fwrite("\033[H\033[J", 1, 6, stdout);
    puts("+----------------------------------------------+");
    snprintf(line, sizeof line, "cGotchi %s", face());
    printf("|%-46s|\n", line);
    puts("+----------------------------------------------+");
    printf("%s  %.1fd  %s\n", pet.name, pet.age_h / 24.f, mood_name());
    printf("bor %s %3.0f\n", b1, pet.boredom);
    printf("exc %s %3.0f\n", b2, pet.excitement);
    printf("en  %s %3.0f\n", b3, pet.energy);
    puts("-----------------------------------------------");
    printf("scan %u  ssid %d  ip %d\n", pet.scans, nssid, nip);
    printf("open %u  wpa3 %u  wow %u\n", pet.opens, pet.wpa3, pet.wow);
    if (pet.last_ok)
        printf("last %d nets\n", pet.last_n);
    else
        printf("scan fail: %s\n", pet.last_msg);
    puts("-----------------------------------------------");
    clip(line, speak());
    puts(line);
    puts("-----------------------------------------------");
    printf("[s]can [a]uto%s [l]og [q]uit\n", autoscan ? "*" : "");
    fflush(stdout);
    (void)i;
}

static void show_log(void)
{
    FILE *f;
    char lines[8][160];
    int n = 0, i;
    f = fopen(LOG_PATH, "r");
    fwrite("\033[H\033[J", 1, 6, stdout);
    puts("cGotchi log  /home/working/ogotchi_log.txt");
    puts("-----------------------------------------------");
    if (!f) {
        puts("(empty or unreadable)");
        puts("[any key]");
        return;
    }
    while (fgets(lines[n % 8], sizeof lines[0], f))
        n++;
    fclose(f);
    {
        int start = n > 8 ? n - 8 : 0;
        int shown = n > 8 ? 8 : n;
        /* last 8 lines: if n>=8 they sit at n%8 start */
        if (n <= 8)
            for (i = 0; i < n; i++)
                fputs(lines[i], stdout);
        else {
            int idx = n % 8;
            for (i = 0; i < 8; i++)
                fputs(lines[(idx + i) % 8], stdout);
        }
        (void)start;
        (void)shown;
    }
    puts("-----------------------------------------------");
    puts("[any key]");
    fflush(stdout);
}

static void raw(int on)
{
    struct termios t;
    if (on) {
        tcgetattr(0, &oldt);
        t = oldt;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        raw_on = 1;
    } else if (raw_on) {
        tcsetattr(0, TCSANOW, &oldt);
        raw_on = 0;
    }
}

int main(void)
{
    struct pollfd pfd = {.fd = 0, .events = POLLIN};
    pet_load();
    log_preload();
    capture_ips();
    raw(1);
    draw();
    for (;;) {
        int pr = poll(&pfd, 1, autoscan ? 25000 : 400);
        if (pr > 0) {
            unsigned char ch = 0;
            if (read(0, &ch, 1) != 1)
                continue;
            if (ch == 'q' || ch == 'Q')
                break;
            if (ch == 's' || ch == 'S') {
                observe();
                draw();
            } else if (ch == 'a' || ch == 'A') {
                autoscan ^= 1;
                draw();
            } else if (ch == 'l' || ch == 'L') {
                show_log();
                poll(&pfd, 1, 8000);
                if (pfd.revents & POLLIN) {
                    unsigned char d;
                    read(0, &d, 1);
                }
                draw();
            }
        } else if (autoscan) {
            observe();
            draw();
        }
    }
    raw(0);
    pet_save();
    return 0;
}
