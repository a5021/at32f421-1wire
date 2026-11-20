#include "lineedit.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void out(const char *fmt, ...)
{
#ifdef LINEEDIT_NO_IO
    (void)fmt;
#else
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
#endif
}

void lineedit_init(lineedit_t *le, const char *prompt)
{
    memset(le, 0, sizeof(*le));
    le->prompt = prompt;
    le->hist_index = -1;
}

void lineedit_clear_history(lineedit_t *le)
{
    le->hist_count = 0;
    le->hist_index = -1;
    le->last[0] = '\0';
}

void lineedit_add_history(lineedit_t *le, const char *cmd)
{
    if ((cmd == NULL) || (cmd[0] == '\0')) {
        return;
    }

    for (int i = 0; i < le->hist_count; i++) {
        if (strcmp(le->hist[i], cmd) == 0) {
            for (int j = i; j + 1 < le->hist_count; j++) {
                memcpy(le->hist[j], le->hist[j + 1], LINEEDIT_MAX);
            }
            le->hist_count--;
            break;
        }
    }

    if (le->hist_count == LINEEDIT_HISTORY) {
        for (int i = 0; i + 1 < LINEEDIT_HISTORY; i++) {
            memcpy(le->hist[i], le->hist[i + 1], LINEEDIT_MAX);
        }
        le->hist_count--;
    }

    strncpy(le->hist[le->hist_count], cmd, LINEEDIT_MAX - 1);
    le->hist[le->hist_count][LINEEDIT_MAX - 1] = '\0';
    le->hist_count++;
    le->hist_index = -1;
}

int lineedit_history_count(const lineedit_t *le)
{
    return le->hist_count;
}

const char *lineedit_history_at(const lineedit_t *le, int index)
{
    if ((index < 0) || (index >= le->hist_count)) {
        return NULL;
    }
    return le->hist[index];
}

void lineedit_redraw(lineedit_t *le)
{
    out("\r%s%.*s\x1b[K", le->prompt, le->len, le->buf);
    if (le->pos < le->len) {
        out("\x1b[%dD", le->len - le->pos);
    }
}

static void load_line(lineedit_t *le, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    strncpy(le->buf, text, LINEEDIT_MAX - 1);
    le->buf[LINEEDIT_MAX - 1] = '\0';
    le->len = (int)strlen(le->buf);
    le->pos = le->len;
    lineedit_redraw(le);
}

static void insert_char(lineedit_t *le, char c)
{
    if (le->len >= LINEEDIT_MAX - 1) {
        return;
    }
    if (le->pos < le->len) {
        memmove(&le->buf[le->pos + 1], &le->buf[le->pos],
                (size_t)(le->len - le->pos));
    }
    le->buf[le->pos] = c;
    le->len++;
    le->pos++;
    le->buf[le->len] = '\0';
    lineedit_redraw(le);
}

static void backspace(lineedit_t *le)
{
    if (le->pos == 0) {
        return;
    }
    if (le->pos < le->len) {
        memmove(&le->buf[le->pos - 1], &le->buf[le->pos],
                (size_t)(le->len - le->pos));
    }
    le->pos--;
    le->len--;
    le->buf[le->len] = '\0';
    lineedit_redraw(le);
}

static void delete_char(lineedit_t *le)
{
    if (le->pos >= le->len) {
        return;
    }
    memmove(&le->buf[le->pos], &le->buf[le->pos + 1],
            (size_t)(le->len - le->pos - 1));
    le->len--;
    le->buf[le->len] = '\0';
    lineedit_redraw(le);
}

static void kill_to_end(lineedit_t *le)
{
    le->len = le->pos;
    le->buf[le->len] = '\0';
    lineedit_redraw(le);
}

static void kill_to_start(lineedit_t *le)
{
    if (le->pos == 0) {
        return;
    }
    memmove(le->buf, &le->buf[le->pos], (size_t)(le->len - le->pos));
    le->len -= le->pos;
    le->pos = 0;
    le->buf[le->len] = '\0';
    lineedit_redraw(le);
}

static void kill_word(lineedit_t *le)
{
    int start = le->pos;

    while ((start > 0) && (le->buf[start - 1] == ' ')) {
        start--;
    }
    while ((start > 0) && (le->buf[start - 1] != ' ')) {
        start--;
    }
    if (start == le->pos) {
        return;
    }
    memmove(&le->buf[start], &le->buf[le->pos],
            (size_t)(le->len - le->pos));
    le->len -= le->pos - start;
    le->pos = start;
    le->buf[le->len] = '\0';
    lineedit_redraw(le);
}

static void history_up(lineedit_t *le)
{
    if (le->hist_count == 0) {
        return;
    }
    if (le->hist_index == -1) {
        memcpy(le->stash, le->buf, LINEEDIT_MAX);
        le->hist_index = le->hist_count - 1;
    } else if (le->hist_index > 0) {
        le->hist_index--;
    } else {
        return;
    }
    load_line(le, le->hist[le->hist_index]);
}

static void history_down(lineedit_t *le)
{
    if (le->hist_index == -1) {
        return;
    }
    if (le->hist_index < le->hist_count - 1) {
        le->hist_index++;
        load_line(le, le->hist[le->hist_index]);
    } else {
        le->hist_index = -1;
        load_line(le, le->stash);
    }
}

static void apply_final(lineedit_t *le, int c)
{
    switch (c) {
    case 'A':
        history_up(le);
        break;
    case 'B':
        history_down(le);
        break;
    case 'C':
        if (le->pos < le->len) {
            le->pos++;
            lineedit_redraw(le);
        }
        break;
    case 'D':
        if (le->pos > 0) {
            le->pos--;
            lineedit_redraw(le);
        }
        break;
    case 'H':
        le->pos = 0;
        lineedit_redraw(le);
        break;
    case 'F':
        le->pos = le->len;
        lineedit_redraw(le);
        break;
    default:
        break;
    }
}

static void apply_tilde(lineedit_t *le, int param)
{
    switch (param) {
    case 1:
        le->pos = 0;
        lineedit_redraw(le);
        break;
    case 3:
        delete_char(le);
        break;
    case 4:
        le->pos = le->len;
        lineedit_redraw(le);
        break;
    case 7:
        le->pos = 0;
        lineedit_redraw(le);
        break;
    case 8:
        le->pos = le->len;
        lineedit_redraw(le);
        break;
    default:
        break;
    }
}

const char *lineedit_feed(lineedit_t *le, int c)
{
    if (le->esc == 1) {
        if (c == '[') {
            le->esc = 2;
        } else if (c == 'O') {
            le->esc = 3;
        } else {
            le->esc = 0;
        }
        return NULL;
    }
    if (le->esc == 2) {
        if ((c >= 0x30) && (c <= 0x3F)) {
            le->esc_param = ((c >= '0') && (c <= '9')) ? c - '0' : 0;
            le->esc_more = 0;
            le->esc = 4;
        } else if ((c >= 0x40) && (c <= 0x7E)) {
            le->esc = 0;
            apply_final(le, c);
        } else {
            le->esc = 0;
        }
        return NULL;
    }
    if (le->esc == 3) {
        le->esc = 0;
        apply_final(le, c);
        return NULL;
    }
    if (le->esc == 4) {
        if (c == ';') {
            le->esc_more = 1;
        } else if ((c >= '0') && (c <= '9')) {
            if (!le->esc_more && (le->esc_param < 100)) {
                le->esc_param = le->esc_param * 10 + (c - '0');
            }
        } else if ((c >= 0x40) && (c <= 0x7E)) {
            if (c == '~') {
                apply_tilde(le, le->esc_param);
            } else {
                apply_final(le, c);
            }
            le->esc = 0;
        } else {
            le->esc = 0;
        }
        return NULL;
    }

    if (c == 27) {
        le->esc = 1;
        return NULL;
    }

    if ((c == '\r') || (c == '\n')) {
        out("\n");
        if (le->len != 0) {
            memcpy(le->last, le->buf, LINEEDIT_MAX);
            lineedit_add_history(le, le->last);
        } else {
            le->last[0] = '\0';
        }
        le->len = 0;
        le->pos = 0;
        le->buf[0] = '\0';
        le->hist_index = -1;
        return le->last;
    }

    if (c == 3) {                    /* Ctrl-C */
        out("^C\n");
        le->len = 0;
        le->pos = 0;
        le->buf[0] = '\0';
        le->hist_index = -1;
        return NULL;
    }
    if (c == 1) {                    /* Ctrl-A */
        le->pos = 0;
        lineedit_redraw(le);
        return NULL;
    }
    if (c == 5) {                    /* Ctrl-E */
        le->pos = le->len;
        lineedit_redraw(le);
        return NULL;
    }
    if (c == 11) {                   /* Ctrl-K */
        kill_to_end(le);
        return NULL;
    }
    if (c == 21) {                   /* Ctrl-U */
        kill_to_start(le);
        return NULL;
    }
    if (c == 23) {                   /* Ctrl-W */
        kill_word(le);
        return NULL;
    }
    if (c == 12) {                   /* Ctrl-L */
        out("\x1b[2J\x1b[H");
        lineedit_redraw(le);
        return NULL;
    }
    if ((c == 8) || (c == 127)) {
        backspace(le);
        return NULL;
    }
    if ((c >= 32) && (c < 127)) {
        insert_char(le, (char)c);
    }
    return NULL;
}
