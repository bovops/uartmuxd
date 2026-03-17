#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define MAX_DLCI 16

struct chan {
    int enabled;
    int dlci;
    int mfd;
    char pts[128];
    char linkpath[256];
};

struct cfg {
    const char *dev;
    speed_t baud;
    int channels;
    int hwflow;
    const char *on_cmd;
    const char *startup_seq;
    const char *on_eol;
    int on_delay_ms;
    int no_serial_init;
    const char *link_dir;
    const char *tap_addr1;
    int verbose;
};

struct hashmux_state {
    int rx_addr;
    int tx_addr;
    int hash_pending;
    uint64_t rx_bytes_total;
    uint64_t rx_to_addr[MAX_DLCI];
    uint64_t rx_hash_esc;
    uint64_t rx_hash_sel;
    uint64_t rx_hash_drop;
};

static volatile sig_atomic_t g_stop = 0;

static int write_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int find_chan_by_dlci(struct chan *chans, int n, int dlci) {
    for (int i = 0; i < n; i++) {
        if (chans[i].enabled && chans[i].dlci == dlci) return i;
    }
    return -1;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static speed_t parse_baud(int b) {
    switch (b) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: return 0;
    }
}

static int set_serial_raw(int fd, speed_t baud, int hwflow) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    if (hwflow) t.c_cflag |= CRTSCTS;
    else t.c_cflag &= ~CRTSCTS;
    cfsetispeed(&t, baud);
    cfsetospeed(&t, baud);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &t);
}

static int set_tty_raw_default(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &t);
}

static int open_pty(struct chan *c, const char *link_dir, int idx) {
    c->mfd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (c->mfd < 0) return -1;
    if (grantpt(c->mfd) < 0 || unlockpt(c->mfd) < 0) {
        close(c->mfd);
        return -1;
    }
    char *p = ptsname(c->mfd);
    if (!p) {
        close(c->mfd);
        return -1;
    }
    snprintf(c->pts, sizeof(c->pts), "%s", p);

    int sfd = open(c->pts, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (sfd >= 0) {
        (void)set_tty_raw_default(sfd);
        close(sfd);
    }

    snprintf(c->linkpath, sizeof(c->linkpath), "%s/umxtty%d", link_dir, idx);
    unlink(c->linkpath);
    if (symlink(c->pts, c->linkpath) < 0) {
        close(c->mfd);
        return -1;
    }
    return 0;
}

static void close_chan(struct chan *c) {
    if (c->linkpath[0]) unlink(c->linkpath);
    if (c->mfd >= 0) close(c->mfd);
    c->mfd = -1;
}

static int hashmux_select_addr(int fd, struct hashmux_state *st, int addr) {
    uint8_t sel[2];
    if (addr < 0 || addr > 9) {
        errno = EINVAL;
        return -1;
    }
    if (st->tx_addr == addr) return 0;
    sel[0] = '#';
    sel[1] = (uint8_t)('0' + addr);
    if (write_all(fd, sel, sizeof(sel)) < 0) return -1;
    st->tx_addr = addr;
    return 0;
}

static int hashmux_write_escaped(int fd, const uint8_t *buf, size_t len) {
    uint8_t out[2048];
    size_t p = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (p + (c == '#' ? 2U : 1U) > sizeof(out)) {
            if (write_all(fd, out, p) < 0) return -1;
            p = 0;
        }
        out[p++] = c;
        if (c == '#') out[p++] = '#';
    }
    if (p) return write_all(fd, out, p);
    return 0;
}

static int hashmux_write_crlf_escaped(int fd, const uint8_t *buf, size_t len) {
    uint8_t out[2048];
    size_t p = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            if (p + 2 > sizeof(out)) {
                if (write_all(fd, out, p) < 0) return -1;
                p = 0;
            }
            out[p++] = '\r';
            out[p++] = '\n';
            continue;
        }
        if (p + (c == '#' ? 2U : 1U) > sizeof(out)) {
            if (write_all(fd, out, p) < 0) return -1;
            p = 0;
        }
        out[p++] = c;
        if (c == '#') out[p++] = '#';
    }
    if (p) return write_all(fd, out, p);
    return 0;
}

static void hashmux_write_to_addr(const struct cfg *cfg, struct chan *chans,
                                  int addr, uint8_t b, int tap_fd) {
    if (addr < 0 || addr >= MAX_DLCI) return;
    int ci = find_chan_by_dlci(chans, cfg->channels, addr);
    if (ci < 0) return;
    (void)write_all(chans[ci].mfd, &b, 1);
    if (addr == 1 && tap_fd >= 0) {
        (void)write_all(tap_fd, &b, 1);
    }
}

static void hashmux_feed_rx(const struct cfg *cfg, struct chan *chans,
                            struct hashmux_state *st, const uint8_t *buf,
                            size_t len, int tap_fd) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        st->rx_bytes_total++;

        if (st->hash_pending) {
            st->hash_pending = 0;
            if (b == '#') {
                st->rx_hash_esc++;
                hashmux_write_to_addr(cfg, chans, st->rx_addr, b, tap_fd);
                if (st->rx_addr >= 0 && st->rx_addr < MAX_DLCI) {
                    st->rx_to_addr[st->rx_addr]++;
                }
            } else if (b >= '0' && b <= '9') {
                st->rx_addr = (int)(b - '0');
                st->rx_hash_sel++;
                if (cfg->verbose) {
                    fprintf(stderr, "HASHMUX RX select addr=%d\n", st->rx_addr);
                }
            } else {
                st->rx_hash_drop++;
                if (cfg->verbose) {
                    fprintf(stderr, "HASHMUX RX drop seq '#%02x' addr=%d\n", b, st->rx_addr);
                }
            }
            continue;
        }

        if (b == '#') {
            st->hash_pending = 1;
            continue;
        }

        hashmux_write_to_addr(cfg, chans, st->rx_addr, b, tap_fd);
        if (st->rx_addr >= 0 && st->rx_addr < MAX_DLCI) {
            st->rx_to_addr[st->rx_addr]++;
        }
    }
}

static int send_startup_cmd(int sfd, const char *cmd, const char *on_eol,
                            int on_delay_ms, int verbose) {
    char cmdbuf[256];
    const char *eol = "\n";
    if (!strcmp(on_eol, "crlf")) eol = "\r\n";
    else if (!strcmp(on_eol, "cr")) eol = "\r";
    else if (!strcmp(on_eol, "none")) eol = "";

    snprintf(cmdbuf, sizeof(cmdbuf), "%s%s", cmd, eol);
    if (write_all(sfd, (const uint8_t *)cmdbuf, strlen(cmdbuf)) < 0) return -1;

    if (verbose) {
        fprintf(stderr, "startup cmd=%s\n", cmdbuf);
    }

    if (on_delay_ms > 0) usleep((useconds_t)on_delay_ms * 1000U);
    return 0;
}

static int send_startup_sequence(int sfd, const struct cfg *cfg) {
    if (cfg->startup_seq && cfg->startup_seq[0]) {
        char seqbuf[512];
        snprintf(seqbuf, sizeof(seqbuf), "%s", cfg->startup_seq);
        char *save = NULL;
        for (char *tok = strtok_r(seqbuf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            while (*tok == ' ' || *tok == '\t') tok++;
            size_t n = strlen(tok);
            while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '\t')) tok[--n] = '\0';
            if (!n) continue;
            if (send_startup_cmd(sfd, tok, cfg->on_eol, cfg->on_delay_ms, cfg->verbose) < 0) return -1;
        }
        return 0;
    }
    if (!cfg->on_cmd || !cfg->on_cmd[0]) return 0;
    return send_startup_cmd(sfd, cfg->on_cmd, cfg->on_eol, cfg->on_delay_ms, cfg->verbose);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -d <device> [options]\n"
            "Options:\n"
            "  -d, --device <path>      serial device (e.g. /dev/ttyS1)\n"
            "  -b, --baud <rate>        default 115200\n"
            "  -n, --channels <count>   default 3 (max 10 for #<digit>)\n"
            "  -l, --link-dir <path>    default /dev\n"
            "      --tap-addr1 <path>   append raw channel1 RX bytes to file\n"
            "      --on-cmd <text>      startup command (default '#0on 6')\n"
            "      --startup-seq <seq>  startup sequence 'cmd1;cmd2;...'\n"
            "      --startup-stock18    same as '#0on 4;#0on 5;#0on 6'\n"
            "      --on-eol <lf|crlf|cr|none>  default lf\n"
            "      --on-delay-ms <ms>   default 200\n"
            "      --no-flow            disable RTS/CTS\n"
            "      --no-serial-init     do not change tty attrs\n"
            "  -v, --verbose            debug logs\n"
            "  -h, --help\n",
            prog);
}

static int parse_args(int argc, char **argv, struct cfg *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->baud = B115200;
    cfg->channels = 3;
    cfg->hwflow = 1;
    cfg->on_cmd = "#0on 6";
    cfg->startup_seq = NULL;
    cfg->on_eol = "lf";
    cfg->on_delay_ms = 200;
    cfg->no_serial_init = 0;
    cfg->link_dir = "/dev";
    cfg->tap_addr1 = NULL;
    cfg->verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) {
            if (i + 1 >= argc) return -1;
            cfg->dev = argv[++i];
        } else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--baud")) {
            if (i + 1 >= argc) return -1;
            speed_t s = parse_baud(atoi(argv[++i]));
            if (!s) return -1;
            cfg->baud = s;
        } else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--channels")) {
            if (i + 1 >= argc) return -1;
            cfg->channels = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--link-dir")) {
            if (i + 1 >= argc) return -1;
            cfg->link_dir = argv[++i];
        } else if (!strcmp(argv[i], "--tap-addr1")) {
            if (i + 1 >= argc) return -1;
            cfg->tap_addr1 = argv[++i];
        } else if (!strcmp(argv[i], "--on-cmd")) {
            if (i + 1 >= argc) return -1;
            cfg->on_cmd = argv[++i];
        } else if (!strcmp(argv[i], "--startup-seq")) {
            if (i + 1 >= argc) return -1;
            cfg->startup_seq = argv[++i];
        } else if (!strcmp(argv[i], "--startup-stock18")) {
            cfg->startup_seq = "#0on 4;#0on 5;#0on 6";
        } else if (!strcmp(argv[i], "--on-eol")) {
            if (i + 1 >= argc) return -1;
            cfg->on_eol = argv[++i];
            if (strcmp(cfg->on_eol, "lf") && strcmp(cfg->on_eol, "crlf") &&
                strcmp(cfg->on_eol, "cr") && strcmp(cfg->on_eol, "none")) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--on-delay-ms")) {
            if (i + 1 >= argc) return -1;
            cfg->on_delay_ms = atoi(argv[++i]);
            if (cfg->on_delay_ms < 0 || cfg->on_delay_ms > 30000) return -1;
        } else if (!strcmp(argv[i], "--no-flow")) {
            cfg->hwflow = 0;
        } else if (!strcmp(argv[i], "--no-serial-init")) {
            cfg->no_serial_init = 1;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            cfg->verbose = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            exit(0);
        } else {
            return -1;
        }
    }

    if (!cfg->dev) return -1;
    if (cfg->channels < 1 || cfg->channels > MAX_DLCI) return -1;
    if (cfg->channels > 10) return -1;
    return 0;
}

int main(int argc, char **argv) {
    struct cfg cfg;
    if (parse_args(argc, argv, &cfg) < 0) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (mkdir(cfg.link_dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir link-dir");
        return 1;
    }

    int sfd = open(cfg.dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (sfd < 0) {
        perror("open serial");
        return 1;
    }

    if (!cfg.no_serial_init) {
        if (set_serial_raw(sfd, cfg.baud, cfg.hwflow) < 0) {
            perror("set_serial_raw");
            close(sfd);
            return 1;
        }
    }

    if (send_startup_sequence(sfd, &cfg) < 0) {
        perror("startup-seq");
        close(sfd);
        return 1;
    }

    struct chan chans[MAX_DLCI];
    memset(chans, 0, sizeof(chans));
    for (int i = 0; i < MAX_DLCI; i++) chans[i].mfd = -1;

    for (int i = 0; i < cfg.channels; i++) {
        chans[i].enabled = 1;
        chans[i].dlci = i;
        if (open_pty(&chans[i], cfg.link_dir, i) < 0) {
            perror("open_pty");
            for (int j = 0; j <= i; j++) close_chan(&chans[j]);
            close(sfd);
            return 1;
        }
        fprintf(stderr, "umxtty%d -> %s (DLCI %d)\n", i, chans[i].pts, chans[i].dlci);
    }

    struct hashmux_state hm;
    memset(&hm, 0, sizeof(hm));
    hm.rx_addr = 0;
    hm.tx_addr = -1;

    int tap_fd = -1;
    if (cfg.tap_addr1 && cfg.tap_addr1[0]) {
        tap_fd = open(cfg.tap_addr1, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    struct pollfd pfds[1 + MAX_DLCI];
    uint8_t rbuf[4096];

    while (!g_stop) {
        int pcount = 0;
        pfds[pcount].fd = sfd;
        pfds[pcount].events = POLLIN;
        pcount++;

        for (int i = 0; i < cfg.channels; i++) {
            pfds[pcount].fd = chans[i].mfd;
            pfds[pcount].events = POLLIN;
            pcount++;
        }

        int rc = poll(pfds, (nfds_t)pcount, 250);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(sfd, rbuf, sizeof(rbuf));
            if (n > 0) {
                hashmux_feed_rx(&cfg, chans, &hm, rbuf, (size_t)n, tap_fd);
            }
        }

        for (int i = 0; i < cfg.channels; i++) {
            if (pfds[1 + i].revents & POLLIN) {
                uint8_t tmp[1024];
                ssize_t n = read(chans[i].mfd, tmp, sizeof(tmp));
                if (n <= 0) continue;

                int addr = chans[i].dlci;
                if (hashmux_select_addr(sfd, &hm, addr) < 0) {
                    if (cfg.verbose) perror("HASHMUX select");
                    continue;
                }

                if (addr == 0) {
                    if (hashmux_write_crlf_escaped(sfd, tmp, (size_t)n) < 0 && cfg.verbose) {
                        perror("HASHMUX dlci0 write");
                    }
                } else {
                    if (hashmux_write_escaped(sfd, tmp, (size_t)n) < 0 && cfg.verbose) {
                        perror("HASHMUX data write");
                    }
                }
            }
        }

        if (cfg.verbose) {
            static uint64_t last_ticks = 0;
            last_ticks++;
            if ((last_ticks % 20U) == 0U) {
                fprintf(stderr,
                        "HASHMUX stats total=%llu a0=%llu a1=%llu a2=%llu esc=%llu sel=%llu drop=%llu cur=%d\n",
                        (unsigned long long)hm.rx_bytes_total,
                        (unsigned long long)hm.rx_to_addr[0],
                        (unsigned long long)hm.rx_to_addr[1],
                        (unsigned long long)hm.rx_to_addr[2],
                        (unsigned long long)hm.rx_hash_esc,
                        (unsigned long long)hm.rx_hash_sel,
                        (unsigned long long)hm.rx_hash_drop,
                        hm.rx_addr);
            }
        }
    }

    for (int i = 0; i < cfg.channels; i++) close_chan(&chans[i]);
    if (tap_fd >= 0) close(tap_fd);
    close(sfd);
    return 0;
}
