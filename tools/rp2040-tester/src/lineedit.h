#ifndef LINEEDIT_H
#define LINEEDIT_H

#define LINEEDIT_MAX        64
#define LINEEDIT_HISTORY    64

typedef struct {
    const char *prompt;
    char        buf[LINEEDIT_MAX];
    int         len;
    int         pos;

    char        hist[LINEEDIT_HISTORY][LINEEDIT_MAX];
    int         hist_count;
    int         hist_index;          /* -1 = editing the current draft */
    char        stash[LINEEDIT_MAX];

    int         esc;                 /* escape sequence state */
    int         esc_param;
    int         esc_more;            /* a ';' was seen: keep the first number */

    char        last[LINEEDIT_MAX];
} lineedit_t;

void        lineedit_init(lineedit_t *le, const char *prompt);
void        lineedit_add_history(lineedit_t *le, const char *cmd);
void        lineedit_clear_history(lineedit_t *le);
void        lineedit_redraw(lineedit_t *le);
const char *lineedit_feed(lineedit_t *le, int c);

int         lineedit_history_count(const lineedit_t *le);
const char *lineedit_history_at(const lineedit_t *le, int index);

#endif /* LINEEDIT_H */
