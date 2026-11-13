/* dumpmobi.c - mirrors PluginBook::loadMobi() exactly, prints concatenated
 * rawml flow text (UTF-8) so we can learn the book body content for the
 * self-test assertion. Build:
 *   gcc dumpmobi.c -I <libmobi>/src -L <libmobi> -lmobi -o dumpmobi
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mobi.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s file.mobi\n", argv[0]); return 2; }
    MOBIData *m = mobi_init();
    if (!m) { fprintf(stderr, "mobi_init failed\n"); return 1; }
    MOBI_RET ret = mobi_load_filename(m, argv[1]);
    if (ret != MOBI_SUCCESS) { fprintf(stderr, "load failed: %d\n", ret); mobi_free(m); return 1; }

    char *title = mobi_meta_get_title(m);
    if (title) { printf("TITLE: %s\n", title); free(title); }

    MOBIRawml *rawml = mobi_init_rawml(m);
    if (!rawml) { fprintf(stderr, "init_rawml failed\n"); mobi_free(m); return 1; }
    ret = mobi_parse_rawml(rawml, m);
    if (ret != MOBI_SUCCESS) { fprintf(stderr, "parse_rawml failed: %d\n", ret); mobi_free_rawml(rawml); mobi_free(m); return 1; }

    printf("=== FLOW DUMP (first 1200 bytes) ===\n");
    size_t total = 0;
    for (MOBIPart *p = rawml->flow; p; p = p->next) {
        if (p->data && p->size > 0) {
            total += p->size;
            size_t show = p->size < 1200 ? p->size : 1200;
            fwrite(p->data, 1, show, stdout);
        }
    }
    printf("\n=== TOTAL FLOW BYTES: %zu ===\n", total);

    mobi_free_rawml(rawml);
    mobi_free(m);
    return 0;
}
