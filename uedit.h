#ifndef UEDIT_H
#define UEDIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
    #include <conio.h>
    #define GETCH() _getch()
#else
    #include <termios.h>
    #include <unistd.h>
    #define GETCH() getchar()
#endif

#define UEDIT_MAX_LINE 1024
#define CTRL_KEY(k) ((k) & 0x1f)

/* Single-entry history; one slot per uedit() call site is typical usage */
static char uedit_last_cmd[UEDIT_MAX_LINE] = {0};

#ifndef _WIN32
static struct termios uedit_orig_termios;

static void uedit_disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &uedit_orig_termios);
}

static void uedit_enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &uedit_orig_termios);
    atexit(uedit_disable_raw_mode);
    struct termios raw = uedit_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
#else
static DWORD uedit_orig_in_mode;
static DWORD uedit_orig_out_mode;

static void uedit_disable_raw_mode(void) {
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),  uedit_orig_in_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), uedit_orig_out_mode);
}

static void uedit_enable_raw_mode(void) {
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hIn,  &uedit_orig_in_mode);
    GetConsoleMode(hOut, &uedit_orig_out_mode);
    SetConsoleMode(hIn, uedit_orig_in_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
    /* Enable ANSI escape processing on the output side */
    SetConsoleMode(hOut, uedit_orig_out_mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
}
#endif

/* Redraw the current line in place. */
static void uedit_refresh_line(const char *prompt, const char *buf, int cur) {
    int col = (int)strlen(prompt) + cur;
    printf("\r\033[K%s%s\r", prompt, buf);
    if (col > 0)
        printf("\033[%dC", col);
    fflush(stdout);
}

/* Interactive line editor. */
static int uedit(const char *prompt, char *buf, int max_line) {
    int r = 0;
    int len = 0, cur = 0, c;

    if (max_line <= 0) return -1;

    memset(buf, 0, max_line);
    printf("%s", prompt);
    fflush(stdout);

    uedit_enable_raw_mode();

    while (1) {
        c = GETCH();

        if (c == CTRL_KEY('a')) {
            cur = 0;
        } else if (c == CTRL_KEY('e')) {
            cur = len;
        } else if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            putchar('\n');
            if (len > 0)
                strncpy(uedit_last_cmd, buf, UEDIT_MAX_LINE - 1);
            r = len;
            break;
        } else if (c == 127 || c == 8) {
            if (cur > 0) {
                memmove(&buf[cur - 1], &buf[cur], len - cur);
                len--; cur--;
                buf[len] = '\0';
            }
        } else if (c == 27) {
            c = GETCH();
            if (c == '[') {
                c = GETCH();
                if      (c == 'D' && cur > 0)   { cur--; }
                else if (c == 'C' && cur < len)  { cur++; }
                else if (c == 'H')               { cur = 0; }
                else if (c == 'F')               { cur = len; }
                else if (c == 'A' && uedit_last_cmd[0]) {
                    strncpy(buf, uedit_last_cmd, max_line - 1);
                    buf[max_line - 1] = '\0';
                    len = (int)strlen(buf); cur = len;
                } else if (c == '3') {
                    int tilde = GETCH();
                    (void)tilde; 
                    if (cur < len) {
                        memmove(&buf[cur], &buf[cur + 1], len - cur - 1);
                        len--; buf[len] = '\0';
                    }
                }
            }
#ifdef _WIN32
        } else if (c == 0 || c == 0xE0) {
            c = GETCH();
            if      (c == 75 && cur > 0)   { cur--; }       
            else if (c == 77 && cur < len) { cur++; }       
            else if (c == 71)              { cur = 0; }     
            else if (c == 79)              { cur = len; }   
            else if (c == 83 && cur < len) {                
                memmove(&buf[cur], &buf[cur + 1], len - cur - 1);
                len--; buf[len] = '\0';
            } else if (c == 72 && uedit_last_cmd[0]) {     
                // strncpy(buf, uedit_last_cmd, max_line - 1);
                snprintf(buf, max_line, "%s", uedit_last_cmd);
                buf[max_line - 1] = '\0';
                len = (int)strlen(buf); cur = len;
            }
#endif
        } else if (c == CTRL_KEY('d')) {
            if (len > 0) {
                if (cur < len) {
                    memmove(&buf[cur], &buf[cur + 1], len - cur - 1);
                    len--; buf[len] = '\0';
                }
            } else {
                r = -1;
                break;
            }
        } else if (c >= 32 && c <= 126 && len < max_line - 1) {
            memmove(&buf[cur + 1], &buf[cur], len - cur);
            buf[cur] = (char)c;
            len++; cur++;
        }
        uedit_refresh_line(prompt, buf, cur);
    }
    uedit_disable_raw_mode();
    return r;
}

#endif /* UEDIT_H */
