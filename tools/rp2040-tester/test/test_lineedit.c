#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lineedit.h"

static void feed_str(lineedit_t *le, const char *s)
{
    while (*s != '\0') {
        lineedit_feed(le, (unsigned char)*s++);
    }
}

static void test_enter_and_history(void)
{
    lineedit_t le;

    lineedit_init(&le, "> ");
    feed_str(&le, "abc");
    const char *line = lineedit_feed(&le, '\r');

    assert(line != NULL);
    assert(strcmp(line, "abc") == 0);
    assert(lineedit_history_count(&le) == 1);

    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'A');
    assert(strcmp(le.buf, "abc") == 0);

    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'B');
    assert(strcmp(le.buf, "") == 0);
}

static void test_editing(void)
{
    lineedit_t le;

    lineedit_init(&le, "> ");
    feed_str(&le, "abcd");

    lineedit_feed(&le, 1);             /* Ctrl-A */
    feed_str(&le, "X");
    assert(strcmp(le.buf, "Xabcd") == 0);

    lineedit_feed(&le, 5);             /* Ctrl-E */
    lineedit_feed(&le, 8);             /* backspace */
    assert(strcmp(le.buf, "Xabc") == 0);

    lineedit_feed(&le, 1);             /* Ctrl-A */
    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, '3');
    lineedit_feed(&le, '~');           /* Delete */
    assert(strcmp(le.buf, "abc") == 0);

    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'F');           /* End */
    feed_str(&le, "de");
    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'D');           /* Left */
    feed_str(&le, "Z");
    assert(strcmp(le.buf, "abcdZe") == 0);
    assert(le.pos == le.len - 1);

    /* Modified keys must be consumed, not typed into the buffer. */
    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, '1');
    lineedit_feed(&le, ';');
    lineedit_feed(&le, '5');
    lineedit_feed(&le, 'C');           /* Ctrl-Right */
    assert(le.pos == le.len);
}

static void test_kill(void)
{
    lineedit_t le;

    lineedit_init(&le, "> ");
    feed_str(&le, "one two three");
    lineedit_feed(&le, 23);            /* Ctrl-W */
    assert(strcmp(le.buf, "one two ") == 0);

    lineedit_feed(&le, 21);            /* Ctrl-U */
    assert(strcmp(le.buf, "") == 0);

    feed_str(&le, "hello");
    lineedit_feed(&le, 1);             /* Ctrl-A */
    lineedit_feed(&le, 11);            /* Ctrl-K */
    assert(strcmp(le.buf, "") == 0);
}

static void test_history_dedup(void)
{
    lineedit_t le;

    lineedit_init(&le, "> ");
    lineedit_add_history(&le, "a");
    lineedit_add_history(&le, "b");
    lineedit_add_history(&le, "a");

    assert(lineedit_history_count(&le) == 2);
    assert(strcmp(lineedit_history_at(&le, 0), "b") == 0);
    assert(strcmp(lineedit_history_at(&le, 1), "a") == 0);
}

static void test_up_down_seeded(void)
{
    lineedit_t le;

    lineedit_init(&le, "> ");
    lineedit_add_history(&le, "help");
    lineedit_add_history(&le, "status");

    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'A');
    assert(strcmp(le.buf, "status") == 0);

    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'A');
    assert(strcmp(le.buf, "help") == 0);

    lineedit_feed(&le, 27);
    lineedit_feed(&le, '[');
    lineedit_feed(&le, 'B');
    assert(strcmp(le.buf, "status") == 0);
}

int main(void)
{
    test_enter_and_history();
    test_editing();
    test_kill();
    test_history_dedup();
    test_up_down_seeded();
    printf("lineedit: all tests passed\n");
    return 0;
}
