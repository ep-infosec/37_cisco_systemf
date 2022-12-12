#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glob.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include "systemf-internal.h"

#define DEBUG 0
#define VA_ARGS(...) , ##__VA_ARGS__
#define DBG(fmt, ...) if (DEBUG) { printf("%s:%-3d:%24s: " fmt "\n", __FILE__, __LINE__, __FUNCTION__ VA_ARGS(__VA_ARGS__)); }

void _sf1_merge_and_free_syllables (_sf1_syllable *syl, char **text_pp, char **trusted_path_pp, int *is_glob_p) {
    int is_glob = 0;
    int is_file = 0;
    int is_trusted = 1;
    size_t escapes = 0;
    size_t slen = 0;
    size_t sandbox_len = 0;
    size_t sandbox_candidate = 0;
    size_t sandbox_index = 0;
    int doing_sandbox_detection = 1;
    const char glob_chars[] = "?*[]"; // Globs supported by glob()
    char *text, *cursor, *trusted_path;

    /*
     * First, walk all the syllables.  Determine the following of the merged argument:
     * is_glob: Glob processing should be run because one syllable is a glob.
     * is_file: Filename sandboxing candidate was detected.
     * is_trusted: No syllables are not trusted. (%s, %p, %*p, %i)
     * escapes: Number of globlike characters that need escaping if is_glob is detected.
     * slen: String length of all syllables concatenated (not including escapes)
     * sandbox_len: Longest span of trusted bytes ending with '/'
     */
    DBG("begin")
    for (_sf1_syllable *s = syl; s != NULL; s = s->next)
    {
        int syl_is_glob = s->flags & SYL_IS_GLOB;
        int syl_escape_glob = s->flags & SYL_ESCAPE_GLOB;
        int syl_is_file = s->flags & SYL_IS_FILE;
        int syl_is_trusted = s->flags & SYL_IS_TRUSTED;

        DBG("SYL - %-8s ig %d, eg %d, if %d, it %d",
            s->text, syl_is_glob, syl_escape_glob, syl_is_file, syl_is_trusted);

        if (syl_escape_glob) {
            int i;

            // look for glob characters and if detected, count how many need to be escaped.
            for (i = strcspn(s->text, glob_chars); s->text[i]; i += strcspn(s->text + i, glob_chars)) {
                 escapes += 1;
                 i += 1;
            }
            slen += i;
        } else {
            slen += strlen(s->text);
        }

        // Scan for fmt string glob patterns and set as globbed file path if detected
        if (syl_is_file) {
           is_file = 1;
        }
        if (syl_is_glob) {
           is_glob = 1;
        }
        if (!syl_is_trusted) {
            is_trusted = 0;
        }

        if (doing_sandbox_detection) {
            if (syl_is_trusted && !syl_is_glob) {
                int i;
                char *cursor = s->text;
                // search for directory separators adding spans including them as we go
                for (cursor = s->text; *cursor; cursor += i) {
                    i = strcspn(cursor, "/");

                    if (cursor[i]) {
                        i += 1;
                        sandbox_len += sandbox_candidate + i;
                        sandbox_candidate = 0;
                    } else {
                        sandbox_candidate += i;
                    }
                }
            } else {
                doing_sandbox_detection = 0;
            }
        }
    }

    DBG("ig %d, if %d, it %d, es %lu, sl %lu, snl %lu, snc %lu, sni %lu, dsd %d",
        is_glob, is_file, is_trusted, escapes, slen, sandbox_len, sandbox_candidate,
        sandbox_index & 0, doing_sandbox_detection);

    if (is_glob) {
        slen += escapes;
    }

    // Allocate memory for the trusted path.
    if (is_file && !is_trusted) {
        if (sandbox_len) {
            trusted_path = malloc(sandbox_len + 1);
            trusted_path[sandbox_len] = 0;
        } else {
            trusted_path = strdup("");
        }
        // FIXME: handle NULL path
    } else {
        sandbox_len = 0;
        trusted_path = NULL;
    }
    text = malloc(slen + 1);
    cursor = text;

    /*
     * Now walk the syllables.
     * * Filling in the sandbox_path if needed.
     * * Filling in text (with escaped glob patterns when needed).
     */
    for (_sf1_syllable *s = syl; s != NULL;)
    {
        int syl_escape_glob = s->flags & SYL_ESCAPE_GLOB;
        _sf1_syllable *save_next;

        for (int i = 0; (sandbox_index < sandbox_len) && s->text[i]; i+=1, sandbox_index +=1) {
            trusted_path[sandbox_index] = s->text[i];
        }

        if (is_glob && syl_escape_glob) {
            // look for glob characters and if detected, count how many need to be escaped.
            char *start = s->text;
            while (*start) {
                int span = strcspn(start, glob_chars);
                memcpy(cursor, start, span);
                cursor += span;
                start += span;
                if (*start) {
                    cursor[0] = '\\';
                    cursor[1] = *start;
                    start += 1;
                    cursor += 2;
                }
            }
        } else {
            cursor = stpcpy(cursor, s->text);
        }
        save_next = s->next;
        free(s);
        s = save_next;
        cursor[0] = 0;
    }
    cursor[0] = 0;
    assert(cursor == text + slen);

    *text_pp = text;
    *trusted_path_pp = trusted_path;
    *is_glob_p = is_glob;
    DBG("end: trusted_path=%s", trusted_path)
    return;
}


_sf1_redirect *_sf1_merge_redirects (_sf1_redirect *left, _sf1_redirect *right) {
    _sf1_redirect *cursor;
    for (cursor = left; cursor->next; cursor = cursor->next);
    cursor->next = right;
    return left;
}

_sf1_redirect *_sf1_create_redirect(_sf1_stream stream, _sf1_stream target, int append, _sf1_syllable *file_syllables)
{
    _sf1_redirect *redirect = calloc(1, sizeof(*redirect));
    // FIXME: Handle malloc error
    redirect->stream = stream;
    redirect->target = target;
    redirect->append = append;
    DBG("begin: stream %d, target %d, append %d, file %p", stream, target, append, file_syllables);
    if (file_syllables) {
        int is_glob;

        _sf1_merge_and_free_syllables(file_syllables, &redirect->text, &redirect->trusted_path, &is_glob);
        // FIXME: This needs to be cleanly handled similar to a syntax error.
        // Currently we don't support globs in file targets.
        assert(!is_glob);
    }
    DBG("end");

    return redirect;
}

static void append_redirect(_sf1_task *task, _sf1_redirect *redirect) {
    _sf1_redirect **next_pp = &(task->redirects);
    while (*next_pp) {
        next_pp = &((*next_pp)->next);
    }
    *next_pp = redirect;
}

void _sf1_create_redirect_pipe (_sf1_task *left, _sf1_task *right) {
    append_redirect(left, _sf1_create_redirect(_SF1_STDOUT, _SF1_PIPE, 0, NULL));
    append_redirect(right, _sf1_create_redirect(_SF1_STDIN, _SF1_PIPE, 0, NULL));
}

_sf1_task *_sf1_create_cmd (_sf1_syllable *syllables, _sf1_redirect *redirects) {
    int is_glob;
    char *text;
    char *trusted_path;
    _sf1_syllable *next;
    _sf1_task *task;

    task = _sf1_task_create();

    while (syllables) {
        next = syllables->next_word;
        _sf1_merge_and_free_syllables(syllables, &text, &trusted_path, &is_glob);
        _sf1_task_add_arg(task, text, trusted_path, is_glob);
        syllables = next;
    }

    _sf1_task_add_redirects(task, redirects);

    return task;
}
