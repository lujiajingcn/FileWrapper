/* 诊断工具：打印每个 MOBI 样本的 flow / markup 部件数量与类型，
   用于验证 loadMobi() 里「拼接所有 flow 部件」是否会把 CSS 等非正文内容混入 HTML。
   编译：gcc test/dumpflow.c -I thirdparty/libmobi/src -L thirdparty/libmobi -lmobi -o test/dumpflow.exe */
#include <stdio.h>
#include <string.h>
#include "mobi.h"

static const char *ftype(MOBIFiletype t)
{
    switch (t) {
    case T_HTML: return "HTML";
    case T_CSS:  return "CSS";
    case T_SVG:  return "SVG";
    case T_JPG:  return "JPG";
    case T_GIF:  return "GIF";
    case T_PNG:  return "PNG";
    case T_BMP:  return "BMP";
    case T_OTF:  return "OTF";
    case T_TTF:  return "TTF";
    case T_NCX:  return "NCX";
    case T_OPF:  return "OPF";
    default:     return "OTHER";
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *path = argv[i];
        MOBIData *m = mobi_init();
        if (!m) { printf("%-40s init failed\n", path); continue; }
        if (mobi_load_filename(m, path) != MOBI_SUCCESS) {
            printf("%-40s LOAD FAIL (encrypted=%d)\n", path, mobi_is_encrypted(m));
            mobi_free(m); continue;
        }
        int isKf8 = mobi_is_kf8(m);
        MOBIRawml *r = mobi_init_rawml(m);
        if (!r) { printf("%-40s rawml init failed\n", path); mobi_free(m); continue; }
        if (mobi_parse_rawml(r, m) != MOBI_SUCCESS) {
            printf("%-40s PARSE FAIL (encrypted=%d)\n", path, mobi_is_encrypted(m));
            mobi_free_rawml(r); mobi_free(m); continue;
        }

        printf("%-40s kf8=%d\n", path, isKf8);

        int nflow = 0; size_t cssBytes = 0;
        printf("    flow  :");
        for (MOBIPart *p = r->flow; p; p = p->next) {
            printf(" [%d:%s %zub]", nflow, ftype(p->type), p->size);
            if (p->type == T_CSS) cssBytes += p->size;
            nflow++;
        }
        printf("  -> %d part(s), CSS bytes=%zu\n", nflow, cssBytes);

        int nmk = 0;
        printf("    markup:");
        for (MOBIPart *p = r->markup; p; p = p->next) {
            if (nmk < 6) printf(" [%s %zub]", ftype(p->type), p->size);
            nmk++;
        }
        printf("  -> %d part(s)\n", nmk);

        mobi_free_rawml(r);
        mobi_free(m);
    }
    return 0;
}
