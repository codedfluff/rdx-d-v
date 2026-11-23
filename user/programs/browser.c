/* browser - a web browser.

   Pages come over HTTP and HTTPS (tls.c; compressed replies are unpacked
   by inflate.c) or from files. They are parsed into a document tree
   (web/dom.c), styled by their CSS (web/css.c, web/cascade.c), laid out
   (web/layout.c: block flow, floats, inline boxes, flexbox, grid, tables,
   positioning) and drawn (web/paint.c), with PNG, JPEG and GIF pictures
   (image.c) and working forms.

   In the window: click the address bar (or Ctrl+L), type, Enter. "<" goes
   back, "R" reloads. Scroll with the arrows, Page Up/Down, Space,
   Home/End or the scroll bar. Click links, buttons, checkboxes; click a
   text field and type; Enter in a field submits its form; Tab moves to the
   next field.

   Usage: browser [address]          open a window (default: home.html)
          browser --dump <address>   print the page as text (for scripts) */
#include "gui.h"
#include "image.h"
#include "inflate.h"
#include "tls.h"

/* The page code keeps its statics in the realm sections (web/realm.c):
   every frame and worker has its own copy. The networking is shared. */
#include "web/font.c"
#pragma clang section bss=".rlmb$m" data=".rlmd$m"
#include "web/dom.c"
#include "web/css.c"
#include "web/cascade.c"
#include "web/layout.c"
#include "web/paint.c"
#pragma clang section bss="" data=""
#include "web/cookies.c"
#include "web/fetch.c"
#pragma clang section bss=".rlmb$m" data=".rlmd$m"
#include "web/js_core.c"
#include "web/js_parse.c"
#include "web/js_bigint.c"
#include "web/js_eval.c"
#include "web/js_lib.c"
#include "web/js_prelude.c"
#include "web/js_dom.c"
#include "web/svg.c"
#pragma clang section bss="" data=""
#include "web/realm.c"
#pragma clang section bss=".rlmb$m" data=".rlmd$m"
#include "web/js_frames.c"

#define W 820
#define H 580
#define BAR 34
#define STATUS 20
#define SCROLLBAR 12
#define PAGE_W (W - SCROLLBAR)
#define HOME "/home.html"
#define MAX_IMAGES 60
#define MAX_IMAGE_BYTES (3 * 1024 * 1024)

#define C_TEXT 0x1C2030
#define C_DIM 0x6A7287

static char page_title[128];
static char status_text[200];
static char page_security[256];
static char *css_buffers[64];
static int css_buffer_count;
static int scroll; /* (each frame scrolls on its own) */

/* ================================================================ loading a page */

/* Pictures for CSS backgrounds, by address (many elements share one). */
static struct {
    char *url;
    image *img;
} bg_cache[96];
static int bg_cache_count;

static void bg_cache_clear(void) {
    for (int i = 0; i < bg_cache_count; i++) {
        if (bg_cache[i].img) {
            scaled_forget(bg_cache[i].img);
            image_free(bg_cache[i].img);
            free(bg_cache[i].img);
        }
        free(bg_cache[i].url);
    }
    bg_cache_count = 0;
}

/* data:[type][;base64],payload -> its bytes (malloc'd), or NULL. */
static char *data_url(const char *url, size_t *len, char *type, int type_max) {
    const char *comma = strchr(url, ',');
    if (strncasecmp(url, "data:", 5) || !comma) return NULL;
    int t = 0, base64 = 0;
    for (const char *p = url + 5; p < comma; p++) {
        if (*p == ';') {
            base64 |= !strncasecmp(p, ";base64", 7);
            break;
        }
        if (t < type_max - 1) type[t++] = *p;
    }
    type[t] = 0;
    for (const char *p = url + 5; p < comma; p++) base64 |= !strncasecmp(p, ";base64", 7);
    size_t in = strlen(comma + 1);
    char *out = malloc(in + 1);
    if (!out) return NULL;
    size_t n = 0;
    const char *p = comma + 1;
    if (base64) {
        unsigned bits = 0;
        int have = 0;
        for (; *p; p++) {
            int c = *p, v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                          : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
            if (c == '%' && p[1] == '3' && (p[2] == 'D' || p[2] == 'd')) break; /* %3D: = */
            if (v < 0) continue;
            bits = bits << 6 | (unsigned)v;
            have += 6;
            if (have >= 8) {
                have -= 8;
                out[n++] = (char)(bits >> have & 255);
            }
        }
    } else {
        for (; *p; p++) {
            if (*p == '%' && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
                out[n++] = (char)(hex_digit(p[1]) * 16 + hex_digit(p[2]));
                p += 2;
            } else out[n++] = *p;
        }
    }
    out[n] = 0;
    *len = n;
    return out;
}

/* The first url(...) in a value, as an address. */
static int css_url(const char *v, char *out, int max) {
    const char *u = strstr(v, "url(");
    if (!u) return 0;
    u += 4;
    while (is_space(*u)) u++;
    char quote = *u == '"' || *u == '\'' ? *u++ : 0;
    int n = 0;
    while (*u && (quote ? *u != quote : (*u != ')' && !is_space(*u))) && n < max - 1) out[n++] = *u++;
    out[n] = 0;
    return n > 0;
}

/* A picture from bytes: SVG drawn at its own size, else PNG / JPEG / GIF. */
static image *picture_from(const char *body, size_t len, const char *type) {
    image *img = malloc(sizeof(image));
    if (!img || len > MAX_IMAGE_BYTES) {
        free(img);
        return NULL;
    }
    char why[96];
    int nw, nh;
    if (looks_like_svg(body, len, type)) {
        if (svg_render_text(body, (int)len, 0, 0, 0x000000, img, &nw, &nh) == 0) {
            int big = nw > nh ? nw : nh;
            if (big > 0 && big < 256) { /* a small drawing: again at 256 px, so it stays sharp when enlarged */
                image sharp;
                if (svg_render_text(body, (int)len, nw * 256 / big, nh * 256 / big, 0x000000, &sharp, &nw, &nh) == 0) {
                    image_free(img);
                    *img = sharp;
                }
            }
            return img;
        }
    } else if (image_decode((const unsigned char *)body, len, img, why) == 0) {
        return img;
    }
    free(img);
    return NULL;
}

static image *bg_picture(const char *url, PrefetchSet *pictures) {
    for (int i = 0; i < bg_cache_count; i++)
        if (!strcmp(bg_cache[i].url, url)) return bg_cache[i].img;
    image *img = NULL;
    if (!strncasecmp(url, "data:", 5)) {
        size_t len;
        char type[64];
        char *body = data_url(url, &len, type, sizeof(type));
        if (body) img = picture_from(body, len, type);
        free(body);
    } else {
        Fetched f;
        char error[400], address[1024];
        strncpy(address, url, sizeof(address) - 1);
        address[sizeof(address) - 1] = 0;
        int taken = prefetch_take(pictures, address, &f);
        if (taken == 1 || (taken == 0 && fetch_url(address, NULL, &f, error) == 0)) {
            img = picture_from(f.body, f.len, f.type);
            free(f.body);
        }
    }
    if (bg_cache_count < 96) {
        bg_cache[bg_cache_count].url = copy_of(url, (int)strlen(url));
        bg_cache[bg_cache_count++].img = img;
    } else if (img) { /* (no room to keep it: not used) */
        image_free(img);
        free(img);
        img = NULL;
    }
    return img;
}

/* A linked stylesheet's url(...)s made absolute (they are relative to the
   sheet, not the page). Returns a new buffer, or NULL to keep the old. */
static char *css_absolute_urls(const char *css, size_t len, const char *sheet_url, size_t *out_len) {
    int count = 0;
    for (size_t i = 0; i + 4 <= len; i++) count += !strncasecmp(css + i, "url(", 4);
    if (!count) return NULL;
    size_t cap = len + (size_t)count * 1100 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        if (i + 4 <= len && !strncasecmp(css + i, "url(", 4)) {
            size_t j = i + 4;
            while (j < len && is_space(css[j])) j++;
            char quote = j < len && (css[j] == '"' || css[j] == '\'') ? css[j++] : 0;
            char rel[1024];
            int k = 0;
            while (j < len && (quote ? css[j] != quote : css[j] != ')') && k < 1023) rel[k++] = css[j++];
            rel[k] = 0;
            while (k > 0 && is_space(rel[k - 1])) rel[--k] = 0;
            if (quote && j < len) j++;
            char abs[1100];
            if (!k || !strncasecmp(rel, "data:", 5) || rel[0] == '#') strcpy(abs, rel);
            else resolve_url(sheet_url, rel, abs, sizeof(abs));
            memcpy(out + n, "url(\"", 5);
            n += 5;
            for (int q = 0; abs[q]; q++) out[n++] = abs[q] == '"' ? '\'' : abs[q];
            out[n++] = '"';
            i = j; /* (the ")" is copied next) */
            continue;
        }
        out[n++] = css[i++];
    }
    out[n] = 0;
    *out_len = n;
    return out;
}

static void free_page(void) {
    for (int i = 1; i < MAX_REALMS; i++) /* its frames and workers go with it */
        if (realm_list[i].used && realm_list[i].parent == realm_list[realm_now].id) realm_destroy(i);
    js_page_end(); /* scripts' objects first: they point at nodes */
    blobs_clear();
    if (document) free_tree(document);
    document = html_element = body_element = head_element = NULL;
    focused_node = NULL;
    refresh_due = 0;
    for (int i = 0; i < css_buffer_count; i++) free(css_buffers[i]);
    css_buffer_count = 0;
    free_rules();
    generated_clear();
    bg_cache_clear();
    scaled_forget(NULL);
    indexed_count = -1;
    page_title[0] = 0;
}

/* <meta http-equiv="refresh" content="3; url=..."> (not one inside
   <noscript> when scripts run: that is for browsers without them). */
static void find_meta_refresh(void) {
    char kind[32], content[1024];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || !is(n->tag, "meta")) continue;
        if (!attr(n, "http-equiv", kind, sizeof(kind)) || strncasecmp(kind, "refresh", 8) || !attr(n, "content", content, sizeof(content))) continue;
        int hidden = 0;
        for (Node *a = n->parent; a; a = a->parent) {
            if (a->type == NODE_ELEMENT && is(a->tag, "noscript")) hidden = js_enabled;
        }
        if (!hidden) {
            schedule_refresh(content, base_url);
            return;
        }
    }
}

/* page_title from the <title> (again after scripts, which may change it).
   Returns whether it changed. */
static int update_page_title(void) {
    char was[sizeof(page_title)];
    strcpy(was, page_title);
    page_title[0] = 0;
    Node *title = document ? find_element(document, "title") : NULL;
    if (title) {
        char raw[256], text[256];
        int raw_len = text_content(title, raw, sizeof(raw));
        ascii_text(raw, raw_len, text, sizeof(text));
        int k = 0, space = 0;
        for (const char *p = text; *p && k < (int)sizeof(page_title) - 1; p++) {
            if (is_space(*p)) {
                space = k > 0;
                continue;
            }
            if (space) page_title[k++] = ' ';
            space = 0;
            page_title[k++] = *p;
        }
        page_title[k] = 0;
    }
    return strcmp(was, page_title) != 0;
}

static void keep_css(char *text) {
    if (css_buffer_count < 64) css_buffers[css_buffer_count++] = text;
    else free(text);
}

/* The built-in stylesheet, then <style> blocks and linked stylesheets in
   document order. */
static void load_stylesheets(void) {
    char *defaults = copy_of(DEFAULT_CSS, (int)sizeof(DEFAULT_CSS) - 1);
    if (defaults) {
        parse_css(defaults, (int)sizeof(DEFAULT_CSS) - 1, 0);
        keep_css(defaults);
    }
    if (!js_enabled) { /* without scripts, <noscript> content shows */
        static const char show[] = "noscript{display:block}";
        char *css = copy_of(show, (int)sizeof(show) - 1);
        if (css) {
            parse_css(css, (int)sizeof(show) - 1, 0);
            keep_css(css);
        }
    }
    int fetched = 0;
    char value[1024], media[128];
    /* the linked ones, fetched all at once */
    PrefetchSet *sheets = prefetch_begin(12);
    for (Node *n = document; n && sheets; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || !is(n->tag, "link")) continue;
        media[0] = 0;
        attr(n, "media", media, sizeof(media));
        if (attr(n, "rel", value, sizeof(value)) && strstr(value, "stylesheet") && !strstr(value, "alternate") &&
            media_applies(media, (int)strlen(media)) && attr(n, "href", value, sizeof(value)) && value[0]) {
            char url[1024];
            resolve_url(base_url, value, url, sizeof(url));
            prefetch_add(sheets, url);
        }
    }
    prefetch_go(sheets);
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT) continue;
        media[0] = 0;
        attr(n, "media", media, sizeof(media));
        if (is(n->tag, "style") && n->first && n->first->type == NODE_TEXT && media_applies(media, (int)strlen(media))) {
            char *css = copy_of(n->first->text, n->first->text_len);
            if (css) {
                parse_css(css, n->first->text_len, 1);
                keep_css(css);
            }
        } else if (is(n->tag, "link") && attr(n, "rel", value, sizeof(value)) && strstr(value, "stylesheet") &&
                   !strstr(value, "alternate") && media_applies(media, (int)strlen(media)) && fetched < 12 &&
                   attr(n, "href", value, sizeof(value)) && value[0]) {
            char url[1024], error[400];
            resolve_url(base_url, value, url, sizeof(url));
            Fetched f;
            fetched++;
            int taken = prefetch_take(sheets, url, &f);
            if (taken == 1 || (taken == 0 && fetch_url(url, NULL, &f, error) == 0)) {
                if (f.len > 1024 * 1024) f.len = 1024 * 1024;
                size_t abs_len;
                char *abs = css_absolute_urls(f.body, f.len, url, &abs_len);
                if (abs) {
                    free(f.body);
                    f.body = abs;
                    f.len = abs_len;
                }
                parse_css(f.body, (int)f.len, 1);
                keep_css(f.body);
            }
        } else if (is(n->tag, "base") && attr(n, "href", value, sizeof(value)) && value[0]) {
            char full[1024];
            resolve_url(base_url, value, full, sizeof(full));
            strcpy(base_url, full);
        }
    }
    prefetch_end(sheets);
}

/* Form controls start out as their attributes say. only_new: just those
   not set up yet (made by scripts). */
static void init_controls_mode(int only_new) {
    static char value[1024];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT) continue;
        if (only_new && (n->value || is(n->tag, "select"))) continue;
        if (is(n->tag, "input")) {
            if (!attr(n, "value", value, sizeof(value))) value[0] = 0;
            free(n->value);
            n->value = copy_of(value, (int)strlen(value));
            n->checked = has_attr(n, "checked");
        } else if (is(n->tag, "textarea")) {
            int len = text_content(n, value, sizeof(value));
            free(n->value);
            n->value = copy_of(value, len);
        } else if (is(n->tag, "select")) {
            int index = 0;
            n->selected_index = 0;
            for (Node *o = n; o; o = next_in_tree(o, n)) {
                if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                    if (has_attr(o, "selected")) n->selected_index = index;
                    index++;
                }
            }
        }
    }
}

static void init_controls(void) { init_controls_mode(0); }

static u64 cascade_ms, layout_ms;

static void relayout(void) {
    u64 t0 = uptime_ms();
    generated_clear(); /* (the last layout's ::before / ::after boxes) */
    compute_styles();
    u64 t1 = uptime_ms();
    layout_page(layout_viewport_w ? layout_viewport_w : PAGE_W);
    generated_detach();
    cascade_ms = t1 - t0;
    layout_ms = uptime_ms() - t1;
    frames_update();
}

static void error_page(const char *message, const char *url) {
    static char page[4096];
    strcpy(page, "<html><head><title>Can't open this page</title></head><body style='margin:24px'>"
                 "<h1>Can't open this page</h1><p>");
    strncat_safe(page, message, 600);
    strncat_safe(page, "</p><p>Address: <code>", 40);
    strncat_safe(page, url, 900);
    strncat_safe(page, "</code></p>", 30);
    if (strstr(message, "untrusted certificate")) {
        strncat_safe(page,
                     "<p style='background:#fdecea;padding:8px'>This site could not prove it is the site you asked for. "
                     "Someone between you and it could be reading or changing what you send and see. "
                     "Do not type passwords or other private things into it.</p><p><a href='rdx-proceed:",
                     400);
        strncat_safe(page, url, 900);
        strncat_safe(page, "'>Go on anyway (not safe)</a></p>", 40);
    }
    strncat_safe(page, "</body></html>", 20);
    parse_html(page, (int)strlen(page));
}

static void number_text(char *out, unsigned long long v) {
    char digits[24];
    int k = 0, m = 0;
    do digits[k++] = (char)('0' + v % 10);
    while (v /= 10);
    while (k) out[m++] = digits[--k];
    out[m] = 0;
}

static unsigned page_epoch; /* changes with every page loaded */

/* Load `address` (GET, or POST with `post`) and lay it out. */
static void load(const char *address, const char *post) {
    page_epoch++;
    static char url[1024];
    strcpy(url, address);
    static char error[400], fragment[512];
    error[0] = 0;
    fragment[0] = 0;
    const char *hash = strchr(address, '#');
    if (hash) strncat_safe(fragment, hash, sizeof(fragment) - 1);
    if (getenv("BROWSER_TIMING")) printf("[heap] start: %d KiB\n", (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024));
    free_page();
    Fetched f;
    u64 t0 = uptime_ms();
    fetch_navigating = 1;
    int failed = fetch_url(url, post, &f, error) != 0; /* (url loses the fragment, may follow redirects) */
    fetch_navigating = 0;
    u64 t1 = uptime_ms();
    strcpy(base_url, url);
    if (getenv("BROWSER_TIMING")) printf("[heap] fetched: %d KiB\n", (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024));
    strcpy(current_url, url);
    if (fragment[0] && strlen(current_url) + strlen(fragment) < sizeof(current_url)) strcat(current_url, fragment);
    if (failed) {
        error_page(error, url);
        strcpy(status_text, "Failed");
        page_security[0] = 0;
    } else {
        strcpy(page_security, f.security);
        if (!strncasecmp(f.type, "text/html", 9) || !strncasecmp(f.type, "application/xhtml", 17)) {
            parse_html(f.body, (int)f.len);
        } else if (!strncasecmp(f.type, "image/", 6) || image_sniff((unsigned char *)f.body, f.len)) {
            static char page[1400];
            strcpy(page, "<html><body style='margin:0;background:#2b2f3a;text-align:center'><img style='max-width:100%' src='");
            strncat_safe(page, url, 1000);
            strncat_safe(page, "'></body></html>", 20);
            parse_html(page, (int)strlen(page));
        } else if (!strncasecmp(f.type, "text/", 5) || !strncasecmp(f.type, "application/json", 16) ||
                   !strncasecmp(f.type, "application/javascript", 22)) {
            /* Plain text: in a <pre>. */
            static const char shell[] = "<html><body><pre></pre></body></html>";
            parse_html(shell, (int)sizeof(shell) - 1);
            Node *pre = find_element(document, "pre");
            Node *t = make_text(f.body, (int)f.len, 0);
            if (pre && t) append_child(pre, t);
        } else {
            char message[200] = "This is not a page but a file of type ";
            strncat_safe(message, f.type, 80);
            strncat_safe(message, ". Use wget to save it.", 30);
            error_page(message, url);
        }
        if (f.refresh[0]) schedule_refresh(f.refresh, url);
        free(f.body);
        char kib[24];
        number_text(kib, (f.len + 1023) / 1024);
        strcpy(status_text, "Done: ");
        strncat_safe(status_text, kib, 16);
        strncat_safe(status_text, " KiB, ", 8);
        strncat_safe(status_text, page_security, 180);
    }
    update_page_title();
    if (getenv("BROWSER_TIMING")) printf("[heap] parsed: %d KiB\n", (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024));
    find_meta_refresh();
    load_stylesheets();
    init_controls();
    u64 t2 = uptime_ms();
    if (js_enabled && document) {
    if (getenv("BROWSER_TIMING")) printf("[heap] stylesheets: %d KiB\n", (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024));
        js_page_start();
        dom_dirty = 1; /* the first question about layout lays the page out */
        js_run_page_scripts();
        js_fire(document, "DOMContentLoaded", 1, 0);
        js_fire(document, "readystatechange", 0, 0);
        init_controls_mode(1);
        dom_dirty = 0;
        if (js_errors) {
            char n[16];
            number_text(n, (unsigned long long)js_errors);
            strncat_safe(status_text, " - ", 4);
            strncat_safe(status_text, n, 10);
            strncat_safe(status_text, js_errors == 1 ? " script error" : " script errors", 16);
        }
    }
    update_page_title();
    collect_links();
    strncpy(realm_list[realm_now].url, current_url, sizeof(realm_list[realm_now].url) - 1);
    u64 t3 = uptime_ms();
    relayout();
    if (getenv("BROWSER_TIMING")) printf("[timing] scripts %llu ms\n", t3 - t2);
    if (getenv("BROWSER_TIMING"))
        printf("[timing] fetch %llu ms, stylesheets %llu ms, cascade %llu ms, layout %llu ms, %d rules, %d nodes, %d items\n",
               t1 - t0, t2 - t1, cascade_ms, layout_ms, rule_count, node_count, item_count);
    if (getenv("BROWSER_TIMING"))
        printf("[timing] %ld compound tests, %ld keyless tries, %ld rules applied\n", stat_compound, stat_keyless, stat_applied);
    cookies_save();
}

/* Fetch and decode the page's pictures. `each` is called after every one
   (to redraw); it returns nonzero to stop early. */
static int load_images_from(int (*each)(void), PrefetchSet *pictures, unsigned page, unsigned dom, int *loaded_count);
static u64 image_wait_ms, image_decode_ms;

static int load_images(int (*each)(void)) {
    int loaded = 0;
    unsigned page = page_epoch, dom = dom_epoch;
    static char src[1024], url[1024];
    /* the pictures still to get, fetched several at once */
    int wanted = 0;
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        wanted += n->type == NODE_ELEMENT && !n->img_state && is(n->tag, "img");
        wanted += n->type == NODE_ELEMENT && !n->bg_state && n->style.bg_image && strstr(n->style.bg_image, "url(") != NULL;
    }
    PrefetchSet *pictures = wanted > 1 ? prefetch_begin(wanted < MAX_IMAGES ? wanted : MAX_IMAGES) : NULL;
    for (Node *n = document; n && pictures; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || n->img_state || !is(n->tag, "img") || n->style.display == D_NONE) continue;
        if ((!attr(n, "src", src, sizeof(src)) || !src[0] || !strncasecmp(src, "data:", 5)) &&
            (!attr(n, "data-src", src, sizeof(src)) || !src[0]))
            continue;
        resolve_url(base_url, src, url, sizeof(url));
        prefetch_add(pictures, url);
    }
    for (Node *n = document; n && pictures; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || n->bg_state || !n->style.bg_image || n->style.display == D_NONE) continue;
        if (!css_url(n->style.bg_image, src, sizeof(src)) || !strncasecmp(src, "data:", 5)) continue;
        resolve_url(base_url, src, url, sizeof(url));
        prefetch_add(pictures, url);
    }
    u64 t0 = uptime_ms();
    image_wait_ms = image_decode_ms = 0;
    prefetch_go(pictures);
    int stopped = load_images_from(each, pictures, page, dom, &loaded);
    prefetch_end(pictures);
    if (getenv("BROWSER_TIMING") && wanted)
        printf("[timing] %d pictures in %d ms: waiting for them %d ms, decoding %d ms\n", wanted, (int)(uptime_ms() - t0), (int)image_wait_ms, (int)image_decode_ms);
    return stopped;
}

static int load_images_from(int (*each)(void), PrefetchSet *pictures, unsigned page, unsigned dom, int *loaded_count) {
    int loaded = *loaded_count;
    static char src[1024], url[1024], error[400];
    for (Node *n = document; n && loaded < MAX_IMAGES; n = next_in_tree(n, document)) {
        if (n->type == NODE_ELEMENT && !n->bg_state && n->style.bg_image && n->style.display != D_NONE && strstr(n->style.bg_image, "url(")) {
            n->bg_state = 2; /* a CSS background picture */
            if (css_url(n->style.bg_image, src, sizeof(src))) {
                if (strncasecmp(src, "data:", 5)) resolve_url(base_url, src, url, sizeof(url));
                else strncpy(url, src, sizeof(url) - 1);
                url[sizeof(url) - 1] = 0;
                image *img = bg_picture(url, pictures);
                if (img) {
                    n->bg_img = img;
                    n->bg_state = 1;
                }
                loaded++;
                if (each && each()) return 1;
                if (page != page_epoch) return 1;
                if (dom != dom_epoch) {
                    dom = dom_epoch;
                    n = document;
                    continue;
                }
            }
        }
        if (n->type != NODE_ELEMENT || n->img_state) continue;
        if (is(n->tag, "svg")) { /* a drawing in the page: drawn at the size it was laid out */
            n->img_state = 2;
            if (n->style.display == D_NONE) continue;
            image *img = malloc(sizeof(image));
            int cw = n->w - frame_h(n, 0), ch = n->h - frame_v(n, 0);
            if (img && svg_render_node(n, cw > 0 ? cw : 0, ch > 0 ? ch : 0, n->style.color & 0xFFFFFF, img) == 0) {
                n->img = img;
                svg_natural_size(n, &n->img_w, &n->img_h);
                n->img_state = 1;
            } else {
                free(img);
            }
            continue;
        }
        if (!is(n->tag, "img")) continue;
        if (n->style.display == D_NONE) continue;
        if (attr(n, "src", src, sizeof(src)) && !strncasecmp(src, "data:", 5) && (!attr(n, "data-src", url, sizeof(url)) || !url[0])) {
            static char data[65536]; /* a picture written into the page */
            n->img_state = 2;
            if (attr(n, "src", data, sizeof(data))) {
                size_t len;
                char type[64];
                char *body = data_url(data, &len, type, sizeof(type));
                image *img = body ? picture_from(body, len, type) : NULL;
                free(body);
                if (img) {
                    n->img = img;
                    n->img_w = img->width;
                    n->img_h = img->height;
                    n->img_state = 1;
                }
            }
            continue;
        }
        if ((!attr(n, "src", src, sizeof(src)) || !src[0] || !strncasecmp(src, "data:", 5)) &&
            (!attr(n, "data-src", src, sizeof(src)) || !src[0])) {
            n->img_state = 2;
            continue;
        }
        resolve_url(base_url, src, url, sizeof(url));
        Fetched f;
        n->img_state = 2;
        u64 wait_t0 = uptime_ms();
        int taken = prefetch_take(pictures, url, &f);
        int got = taken == 1 || (taken == 0 && fetch_url(url, NULL, &f, error) == 0);
        u64 decode_t0 = uptime_ms();
        image_wait_ms += decode_t0 - wait_t0;
        if (got) {
            image *img = malloc(sizeof(image));
            char why[96] = "not a picture RDX can show";
            int nw = 0, nh = 0, cw = n->w - frame_h(n, 0), ch = n->h - frame_v(n, 0);
            if (img && f.len <= MAX_IMAGE_BYTES && looks_like_svg(f.body, f.len, f.type) &&
                svg_render_text(f.body, (int)f.len, cw > 0 ? cw : 0, ch > 0 ? ch : 0, n->style.color & 0xFFFFFF, img, &nw, &nh) == 0) {
                n->img = img; /* drawn at its laid-out size (or its own), sized for layout by what it asks for */
                n->img_w = nw;
                n->img_h = nh;
                n->img_state = 1;
            } else if (img && f.len <= MAX_IMAGE_BYTES && image_decode((unsigned char *)f.body, f.len, img, why) == 0) {
                n->img = img;
                n->img_w = img->width;
                n->img_h = img->height;
                n->img_state = 1;
            } else {
                if (getenv("BROWSER_TIMING")) printf("[image] %s: %s\n", url, img ? why : "no memory");
                free(img);
            }
            free(f.body);
            image_decode_ms += uptime_ms() - decode_t0;
        } else if (getenv("BROWSER_TIMING")) {
            printf("[image] %s: %s\n", url, error);
        }
        loaded++;
        if (each && each()) return 1;
        /* `each` handles the user's input, which may load another page or
           let a script remove nodes (perhaps n): never walk on from a freed one. */
        if (page != page_epoch) return 1;
        if (dom != dom_epoch) {
            dom = dom_epoch;
            n = document; /* start again: pictures already tried are skipped */
        }
    }
    return 0;
}

/* ================================================================ forms */

static Node *form_of(Node *n) {
    for (Node *a = n; a && a != document; a = a->parent) {
        if (a->type == NODE_ELEMENT && is(a->tag, "form")) return a;
    }
    return NULL;
}

static void url_encode(char *out, size_t max, const char *text) {
    size_t at = strlen(out);
    static const char *hexdigits = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p && at + 4 < max; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            out[at++] = (char)*p;
        } else if (*p == ' ') {
            out[at++] = '+';
        } else {
            out[at++] = '%';
            out[at++] = hexdigits[*p >> 4];
            out[at++] = hexdigits[*p & 15];
        }
    }
    out[at] = 0;
}

static void add_field(char *out, size_t max, const char *name, const char *value) {
    if (out[0]) strncat_safe(out, "&", 2);
    url_encode(out, max, name);
    strncat_safe(out, "=", 2);
    url_encode(out, max, value);
}

static void go(const char *url, int remember, const char *post);

/* Submit the form `form`, pressed by `submitter` (or NULL). */
static void submit(Node *form, Node *submitter) {
    if (js_fire(form, "submit", 1, 1)) return; /* a script handled it */
    static char data[8192];
    data[0] = 0;
    static char name[128], value[1024];
    char type[16];
    for (Node *n = form; n; n = next_in_tree(n, form)) {
        if (n->type != NODE_ELEMENT || !attr(n, "name", name, sizeof(name)) || !name[0] || has_attr(n, "disabled")) continue;
        if (is(n->tag, "input")) {
            strcpy(type, "text");
            attr(n, "type", type, sizeof(type));
            if (!strcasecmp_word(type, "checkbox") || !strcasecmp_word(type, "radio")) {
                if (!n->checked) continue;
                add_field(data, sizeof(data), name, attr(n, "value", value, sizeof(value)) ? value : "on");
            } else if (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "button") || !strcasecmp_word(type, "image") ||
                       !strcasecmp_word(type, "reset")) {
                if (n == submitter && attr(n, "value", value, sizeof(value))) add_field(data, sizeof(data), name, value);
            } else if (strcasecmp_word(type, "file")) {
                add_field(data, sizeof(data), name, n->value ? n->value : "");
            }
        } else if (is(n->tag, "textarea")) {
            add_field(data, sizeof(data), name, n->value ? n->value : "");
        } else if (is(n->tag, "select")) {
            int index = 0;
            for (Node *o = n; o; o = next_in_tree(o, n)) {
                if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                    if (index == n->selected_index) {
                        if (!attr(o, "value", value, sizeof(value))) text_content(o, value, sizeof(value));
                        add_field(data, sizeof(data), name, value);
                        break;
                    }
                    index++;
                }
            }
        } else if (is(n->tag, "button") && n == submitter && attr(n, "value", value, sizeof(value))) {
            add_field(data, sizeof(data), name, value);
        }
    }
    static char action[1024];
    char method[16] = "get";
    action[0] = 0;
    attr(form, "action", action, sizeof(action));
    attr(form, "method", method, sizeof(method));
    if (submitter) {
        attr(submitter, "formaction", action, sizeof(action));
        attr(submitter, "formmethod", method, sizeof(method));
    }
    static char target[2048];
    resolve_url(base_url, action[0] ? action : current_url, target, sizeof(target));
    if (!strcasecmp_word(method, "post")) {
        go(target, 1, data);
    } else {
        char *q = strchr(target, '?');
        if (q) *q = 0;
        strncat_safe(target, "?", 2);
        strncat_safe(target, data, sizeof(target) - strlen(target) - 1);
        go(target, 1, NULL);
    }
}

static int navigate_requested;

/* Activate a control (clicked, or Enter / Space while it has focus).
   Returns 1 if anything changed. */
static int activate(Node *n) {
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    if (has_attr(n, "disabled")) return 0;
    if (is(n->tag, "input") && !strcasecmp_word(type, "checkbox")) {
        n->checked = !n->checked;
        js_fire(n, "input", 1, 0);
        js_fire(n, "change", 1, 0);
        return 1;
    }
    if (is(n->tag, "input") && !strcasecmp_word(type, "radio")) {
        char name[128], other[128];
        name[0] = 0;
        attr(n, "name", name, sizeof(name));
        Node *scope = form_of(n) ? form_of(n) : document;
        for (Node *o = scope; o; o = next_in_tree(o, scope)) {
            if (o->type == NODE_ELEMENT && is(o->tag, "input") && attr(o, "name", other, sizeof(other)) && !strcmp(name, other))
                o->checked = 0;
        }
        n->checked = 1;
        js_fire(n, "input", 1, 0);
        js_fire(n, "change", 1, 0);
        return 1;
    }
    if (is(n->tag, "select")) {
        int count = 0;
        for (Node *o = n; o; o = next_in_tree(o, n)) count += o->type == NODE_ELEMENT && is(o->tag, "option");
        if (count) n->selected_index = (n->selected_index + 1) % count;
        js_fire(n, "input", 1, 0);
        js_fire(n, "change", 1, 0);
        return 1;
    }
    int submit_button = (is(n->tag, "input") && (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "image"))) ||
                        (is(n->tag, "button") && (!attr(n, "type", type, sizeof(type)) || !strcasecmp_word(type, "submit")));
    if (submit_button && form_of(n)) {
        submit(form_of(n), n);
        return 1;
    }
    if (is(n->tag, "input") && !strcasecmp_word(type, "reset") && form_of(n)) {
        init_controls();
        return 1;
    }
    return 0;
}

#pragma clang section bss="" data=""
static int dumping;
#pragma clang section bss=".rlmb$m" data=".rlmd$m"

/* ================================================================ --dump: the page as text */

typedef struct {
    int y, x, index;
} Placed;

static void print_page_text(void);

static int dump(const char *address) {
    dumping = 1;
    realm_dumping = 1;
    if (getenv("BROWSER_CONSOLE")) console_echo = 1;
    if (getenv("BROWSER_TRACE")) js_trace_throws = 1;
    if (getenv("BROWSER_WIDTH")) layout_viewport_w = atoi(getenv("BROWSER_WIDTH")); /* (to compare with other browsers) */
    if (getenv("BROWSER_HEIGHT")) layout_viewport_h = atoi(getenv("BROWSER_HEIGHT"));
    load(address, NULL);
    /* Let the page's timers run for a moment, and follow script redirects. */
    for (int hops = 0; hops < 4; hops++) {
        u64 start = uptime_ms();
        int wait = getenv("BROWSER_WAIT") ? atoi(getenv("BROWSER_WAIT")) : 4000;
        for (int i = 0; i < 4000 && !js_navigate; i++) {
            int t = realms_next_timer();
            if (t < 0 || uptime_ms() - start + (u64)t > (u64)wait) break;
            if (t > 0) sleep_ms((u64)t);
            js_run_timers();
            if (dom_dirty) { /* (new frames start when the page is laid out) */
                init_controls_mode(1);
                collect_links();
                relayout();
                dom_dirty = 0;
            }
            realms_run_others();
            js_maybe_gc();
        }
        if (!js_navigate) break;
        static char next[2048];
        strcpy(next, strncmp(js_navigate, "rdx-back:", 9) ? js_navigate : current_url);
        free(js_navigate);
        js_navigate = NULL;
        load(next, NULL);
    }
    if (getenv("BROWSER_IMAGES")) load_images(NULL);
    page_loaded = 1;
    js_fire(NULL, "load", 0, 0);
    if (dom_dirty || getenv("BROWSER_IMAGES")) {
        init_controls_mode(1);
        collect_links();
        relayout();
        dom_dirty = 0;
    }
    update_page_title();
    if (getenv("BROWSER_DOM") && html_element) { /* the page as it is now, scripts' changes included */
        Buf b = {0};
        serialize(html_element, &b, 0);
        printf("[dom] %s\n", b.p ? b.p : "");
        free(b.p);
    }
    if (page_title[0]) printf("Title: %s\n", page_title);
    printf("Connection: %s\n\n", page_security[0] ? page_security : "none");
    print_page_text();
    if (getenv("BROWSER_BOXES") && body_element) { /* every element's box, to compare layouts (tools/layoutdiff.py) */
        int k = 0;
        for (Node *n = body_element; n; n = next_in_tree(n, body_element)) {
            if (n->type != NODE_ELEMENT || n->tag[0] == '#') continue;
            int shown = 1;
            for (Node *a = n; a && a != document; a = a->parent)
                if (a->type == NODE_ELEMENT && a->style.display == D_NONE) shown = 0;
            char id[40] = "", cls[40] = "";
            attr(n, "id", id, sizeof(id));
            attr(n, "class", cls, sizeof(cls));
            for (char *c = cls; *c; c++)
                if (*c == ' ') *c = 0;
            printf("[box] %d %s %d %d %d %d %s %s\n", k++, n->tag, shown ? n->x : 0, shown ? n->y : 0, shown ? n->w : 0,
                   shown ? n->h : 0, id[0] ? id : "-", cls[0] ? cls : "-");
        }
    }
    for (int i = 1; i < MAX_REALMS; i++) { /* and what the frames show */
        if (!realm_list[i].used || realm_list[i].kind != REALM_FRAME) continue;
        printf("\n--- frame %s (%dx%d) ---\n", realm_list[i].url, realm_list[i].width, realm_list[i].height);
        realm_switch(i);
        if (getenv("BROWSER_DOM") && html_element) {
            Buf b = {0};
            serialize(html_element, &b, 0);
            printf("[dom] %s\n", b.p ? b.p : "");
            free(b.p);
        }
        print_page_text();
        realm_switch(0);
    }
    return strcmp(status_text, "Failed") == 0;
}

/* The page's text items, in reading order. */
static void print_page_text(void) {
    /* Text items, in reading order: by line (rows of 12 pixels), then left
       to right. */
    Placed *order = malloc(sizeof(Placed) * (size_t)(item_count + 1));
    if (!order) return;
    int n = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        if (it->clipped && (it->clip[2] - it->clip[0] < 4 || it->clip[3] - it->clip[1] < 4 || it->x >= it->clip[2] ||
                            it->x + it->w <= it->clip[0] || it->y >= it->clip[3] || it->y + it->h <= it->clip[1]))
            continue;
        if (it->kind == IT_TEXT || it->kind == IT_MARKER || (it->kind == IT_IMAGE && it->w > 0) || it->kind == IT_CONTROL)
            order[n++] = (Placed){it->y + it->h / 2, it->x, i};
    }
    for (int i = 1; i < n; i++) { /* insertion sort by height: nearly sorted already */
        Placed p = order[i];
        int j = i;
        while (j > 0 && order[j - 1].y > p.y) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = p;
    }
    /* Rows: items whose middles are within 8 pixels of the row's first
       (text of different sizes on one line). Then left to right in each. */
    int row_id = 0, first_y = n ? order[0].y : 0;
    for (int i = 0; i < n; i++) {
        if (order[i].y - first_y > 8) {
            first_y = order[i].y;
            row_id++;
        }
        order[i].y = row_id * 12;
    }
    for (int i = 1; i < n; i++) {
        Placed p = order[i];
        int j = i;
        while (j > 0 && order[j - 1].y == p.y && order[j - 1].x > p.x) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = p;
    }
    static char line[600];
    int used = 0, row = -1000, prev_end = -1, prev_bottom = 0;
    for (int k = 0; k <= n; k++) {
        if (k == n || order[k].y / 12 != row) {
            if (used) {
                while (used > 0 && line[used - 1] == ' ') used--;
                line[used] = 0;
                printf("%s\n", line);
            }
            if (k == n) break;
            if (row != -1000 && items[order[k].index].y - prev_bottom > 12) printf("\n");
            row = order[k].y / 12;
            used = 0;
            prev_end = -1;
        }
        Item *it = &items[order[k].index];
        char label[160];
        const char *text;
        int len;
        if (it->kind == IT_IMAGE) {
            char alt[120] = "";
            attr(it->node, "alt", alt, sizeof(alt));
            strcpy(label, "[image");
            if (alt[0]) {
                strncat_safe(label, ": ", 3);
                strncat_safe(label, alt, 100);
            }
            strncat_safe(label, "]", 2);
            text = label;
            len = (int)strlen(label);
        } else if (it->kind == IT_CONTROL && is(it->node->tag, "svg")) {
            continue; /* icons */
        } else if (it->kind == IT_CONTROL) {
            char inner[160];
            control_label(it->node, inner, sizeof(inner));
            if (!inner[0] && is_text_control(it->node)) {
                strcpy(label, "[");
                strncat_safe(label, it->node->value && it->node->value[0] ? it->node->value : "________", 60);
                strncat_safe(label, "]", 2);
            } else if (inner[0]) {
                strcpy(label, "[");
                strncat_safe(label, inner, 150);
                strncat_safe(label, "]", 2);
            } else {
                char type[16] = "";
                attr(it->node, "type", type, sizeof(type));
                strcpy(label, !strcasecmp_word(type, "checkbox") ? (it->node->checked ? "[x]" : "[ ]")
                              : !strcasecmp_word(type, "radio") ? (it->node->checked ? "(*)" : "( )")
                                                                : "[ ]");
            }
            text = label;
            len = (int)strlen(label);
        } else {
            static char plain[600];
            len = ascii_text(pool + it->text, it->len, plain, sizeof(plain));
            text = plain;
        }
        int col = it->x / 8;
        if (prev_end >= 0) {
            /* Words a space apart (in any font size) are one space apart in
               the text; only wide gaps (table columns) keep their position. */
            int gap = it->x - prev_end, cell = it->kind == IT_TEXT && it->size ? it->size / 2 : 8;
            int space = gap > 0 ? 1 : 0;
            if (gap <= 2 * cell || col < used + space) col = used + space;
        }
        if (col > 500) col = 500;
        while (used < col && used < 590) line[used++] = ' ';
        for (int q = 0; q < len && used < 590; q++) line[used++] = text[q];
        /* (a link's word may be stretched over the space after it: measure the word) */
        prev_end = it->x + (it->kind == IT_TEXT ? font_width(it->face, it->bold, it->italic, it->size, pool + it->text, it->len) : it->w);
        if (it->y + it->h > prev_bottom || k == 0) prev_bottom = it->y + it->h;
        /* A link's last word gets [number] after it. */
        int link = it->kind == IT_TEXT || it->kind == IT_IMAGE ? it->link : -1;
        int next_link = -1;
        if (k + 1 < n && order[k + 1].y / 12 == row) {
            Item *next = &items[order[k + 1].index];
            next_link = next->kind == IT_TEXT || next->kind == IT_IMAGE ? next->link : -1;
        }
        if (link >= 0 && next_link != link) {
            char mark[16] = "[";
            number_text(mark + 1, (unsigned long long)link + 1);
            strncat_safe(mark, "]", 2);
            for (int q = 0; mark[q] && used < 590; q++) line[used++] = mark[q];
        }
    }
    free(order);
    if (link_count) {
        printf("\nLinks:\n");
        for (int i = 0; i < link_count; i++) {
            static char full[1024];
            resolve_url(base_url, links[i], full, sizeof(full));
            printf("%4d. %s\n", i + 1, full);
        }
    }
}

/* ================================================================ the window */

#pragma clang section bss="" data=""
static gui_window *win;
static char address[1024];
static int editing, selected_all;
static char *history[32];
static int history_count;

static int view_height(void) { return H - BAR - STATUS; }

static int max_scroll(void) {
    int m = page_height - view_height();
    return m > 0 ? m : 0;
}

static void start_editing(void) {
    editing = 1;
    selected_all = 1;
    strcpy(address, current_url);
}

static void draw(void) {
    if (realm_now) { /* in a frame: the window is drawn again soon */
        frames_changed = 1;
        return;
    }
    canvas *c = &win->pixels;
    fill_rect(c, 0, 0, W, BAR, 0x2B3245);
    fill_rect(c, 6, 6, 26, BAR - 12, history_count ? 0x4A5878 : 0x353C52);
    draw_char(c, 15, 9, '<', history_count ? 0xFFFFFF : 0x7A8299);
    fill_rect(c, 36, 6, 26, BAR - 12, 0x4A5878);
    draw_char(c, 45, 9, 'R', 0xFFFFFF);
    fill_rect(c, 70, 6, W - 80, BAR - 12, editing ? 0xFFFFFF : 0xE8EBF2);
    draw_rect(c, 70, 6, W - 80, BAR - 12, editing ? 0x3D7DFF : 0xB8BFCF);
    const char *shown = editing ? address : current_url;
    int fit = (W - 96) / CHAR_WIDTH;
    int n = (int)strlen(shown);
    if (editing && selected_all && n) {
        fill_rect(c, 77, 8, (n > fit ? fit : n) * CHAR_WIDTH + 2, BAR - 16, 0x3D7DFF);
        draw_glyphs(c, 78, 9, shown + (n > fit ? n - fit : 0), n > fit ? fit : n, 8, 0xFFFFFF, 0);
    } else {
        draw_glyphs(c, 78, 9, shown + (n > fit ? n - fit : 0), n > fit ? fit : n, 8, C_TEXT, 0);
    }
    if (editing && !selected_all) fill_rect(c, 78 + (n > fit ? fit : n) * CHAR_WIDTH, 9, 2, CHAR_HEIGHT, 0x3D7DFF);

    canvas view = {c->pixels + (size_t)BAR * (size_t)c->stride, PAGE_W, view_height(), c->stride};
    paint_page(&view, scroll, 0);
    canvas bar = {c->pixels + (size_t)BAR * (size_t)c->stride + PAGE_W, SCROLLBAR, view_height(), c->stride};
    int track = view_height();
    fill_rect(&bar, 0, 0, SCROLLBAR, track, 0xEEF0F4);
    if (page_height > track) {
        int thumb = (int)((long long)track * track / page_height);
        if (thumb < 20) thumb = 20;
        int top = (int)((long long)(track - thumb) * scroll / (max_scroll() ? max_scroll() : 1));
        fill_rect(&bar, 2, top, SCROLLBAR - 4, thumb, 0xA9B1C4);
    }
    fill_rect(c, 0, H - STATUS, W, STATUS, 0xE3E6EE);
    int sl = (int)strlen(status_text);
    draw_glyphs(c, 8, H - STATUS + 2, status_text, sl > (W - 16) / 8 ? (W - 16) / 8 : sl, 8, C_DIM, 0);
    gui_update(win, 0, 0, W, H);
}

static void set_window_title(void) {
    char title[40] = "Browser";
    if (page_title[0]) {
        strcpy(title, "Browser - ");
        size_t n = strlen(title);
        for (int i = 0; page_title[i] && n < sizeof(title) - 1; i++) title[n++] = page_title[i];
        title[n] = 0;
    }
    gui_set_title(win, title);
}

static void handle(gui_message *m);
static void after_js(void);
static void back(void);

/* Between pictures: redraw, and deal with anything the user did. */
static int image_progress(void) {
    relayout();
    if (scroll > max_scroll()) scroll = max_scroll();
    draw();
    gui_message m;
    unsigned page = page_epoch;
    while (gui_poll(&m) > 0) {
        handle(&m);
        if (navigate_requested || page != page_epoch) return 1; /* another page is showing now */
    }
    return 0;
}

/* The element a #fragment names (id, or <a name>). */
static Node *fragment_target(const char *frag) {
    char want[512];
    int k = 0;
    for (const char *p = frag; *p && k < 511; p++) { /* %xx escapes */
        if (*p == '%' && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
            want[k++] = (char)(hex_digit(p[1]) * 16 + hex_digit(p[2]));
            p += 2;
        } else {
            want[k++] = *p;
        }
    }
    want[k] = 0;
    if (!want[0]) return NULL;
    char v[512];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT) continue;
        if (attr(n, "id", v, sizeof(v)) && !strcmp(v, want)) return n;
        if (is(n->tag, "a") && attr(n, "name", v, sizeof(v)) && !strcmp(v, want)) return n;
    }
    return NULL;
}

static int max_scroll(void);

/* Scroll so that the #fragment's element is at the top. */
static void scroll_to_fragment(const char *url) {
    const char *hash = strchr(url, '#');
    Node *n = hash ? fragment_target(hash + 1) : NULL;
    if (!n) return;
    /* an element with no box of its own (an empty anchor): the next one with a box */
    for (Node *m = n; m && !(n->w || n->h); m = next_in_tree(m, document)) {
        if (m->type == NODE_ELEMENT && (m->w || m->h)) {
            n = m;
            break;
        }
    }
    scroll = n->y > 4 ? n->y - 4 : 0;
    if (scroll > max_scroll()) scroll = max_scroll();
    page_scroll_y = scroll;
}

/* Is `url` the page already showing (apart from its #fragment)? */
static int same_page(const char *url) {
    const char *a = strchr(url, '#'), *b = strchr(current_url, '#');
    size_t la = a ? (size_t)(a - url) : strlen(url), lb = b ? (size_t)(b - current_url) : strlen(current_url);
    return la == lb && !strncmp(url, current_url, la) && document;
}

/* A frame goes to another page. */
static void frame_go(const char *url, const char *post) {
    static char target[2048], body[8192];
    strncpy(target, url, sizeof(target) - 1);
    if (post) strncpy(body, post, sizeof(body) - 1);
    load(target, post ? body : NULL);
    load_images(NULL);
    page_loaded = 1;
    js_fire(NULL, "load", 0, 0);
    relayout();
    frames_changed = 1;
}

static void go(const char *url, int remember, const char *post) {
    if (realm_now) {
        frame_go(url, post);
        return;
    }
    if (!post && !dumping && strchr(url, '#') && same_page(url)) { /* a place in this page: just scroll there */
        if (remember && history_count < 32) {
            history[history_count] = copy_of(current_url, (int)strlen(current_url));
            if (history[history_count]) history_count++;
        }
        strncpy(current_url, url, sizeof(current_url) - 1);
        current_url[sizeof(current_url) - 1] = 0;
        scroll_to_fragment(url);
        draw();
        return;
    }
    navigate_requested = 1;
    if (remember && current_url[0] && history_count < 32) {
        history[history_count] = copy_of(current_url, (int)strlen(current_url));
        if (history[history_count]) history_count++;
    }
    strcpy(status_text, "Loading ");
    strncat_safe(status_text, url, sizeof(status_text) - 10);
    editing = 0;
    if (!dumping) draw();
    static char target[2048];
    strcpy(target, url);
    static char body[8192];
    if (post) strcpy(body, post);
    load(target, post ? body : NULL);
    scroll = 0;
    navigate_requested = 0;
    if (dumping) return;
    scroll_to_fragment(current_url);
    int fragment_scroll = scroll;
    set_window_title();
    draw();
    char saved_status[200];
    strcpy(saved_status, status_text);
    if (!load_images(image_progress)) {
        strcpy(status_text, saved_status);
        page_loaded = 1;
        js_fire(NULL, "load", 0, 0);
        relayout();
        /* pictures moved things down: the #place again, unless the user scrolled meanwhile */
        if (scroll == fragment_scroll) scroll_to_fragment(current_url);
        draw();
        after_js();
    }
}

/* The same in a frame (or worker). */
static void frame_after_js(void) {
    run_microtasks();
    if (js_navigate) {
        char *url = js_navigate;
        js_navigate = NULL;
        if (strncmp(url, "rdx-back:", 9) && document) frame_go(url, NULL);
        free(url);
        return;
    }
    if (dom_dirty && document) {
        init_controls_mode(1);
        collect_links();
        relayout();
        dom_dirty = 0;
        load_images(NULL);
        relayout();
        frames_changed = 1;
    }
    js_maybe_gc();
}

/* After scripts ran (an event, a timer): do what they asked for. */
static void after_js(void) {
    if (realm_now) {
        frame_after_js();
        return;
    }
    if (js_navigate) {
        char *url = js_navigate;
        js_navigate = NULL;
        if (!strncmp(url, "rdx-back:", 9)) back();
        else go(url, 1, NULL);
        free(url);
        return;
    }
    int redraw = 0;
    if (js_alert[0]) {
        strcpy(status_text, js_alert);
        js_alert[0] = 0;
        redraw = 1;
    }
    if (dom_dirty) {
        if (update_page_title()) set_window_title();
        init_controls_mode(1);
        collect_links();
        relayout();
        dom_dirty = 0;
        load_images(NULL);
        relayout();
        redraw = 1;
    }
    if (js_scroll_to >= 0) {
        scroll = js_scroll_to;
        if (scroll > max_scroll()) scroll = max_scroll();
        js_scroll_to = -1;
        redraw = 1;
    }
    page_scroll_y = scroll;
    if (redraw) draw();
    js_maybe_gc();
}

static void back(void) {
    if (!history_count) return;
    char *url = history[--history_count];
    go(url, 0, NULL);
    free(url);
}

static void scroll_by(int dy) {
    scroll += dy;
    if (scroll > max_scroll()) scroll = max_scroll();
    if (scroll < 0) scroll = 0;
    draw();
}

/* Move the keyboard to the next (or previous) form field. */
static void focus_next(int backwards) {
    static Node *list[512];
    int count = 0, at = -1;
    for (Node *n = document; n && count < 512; n = next_in_tree(n, document)) {
        if (n->type == NODE_ELEMENT && (is(n->tag, "input") || is(n->tag, "textarea") || is(n->tag, "select") || is(n->tag, "button")) &&
            n->style.display != D_NONE && n->w > 0) {
            char type[16] = "";
            attr(n, "type", type, sizeof(type));
            if (!strcasecmp_word(type, "hidden")) continue;
            if (n == focused_node) at = count;
            list[count++] = n;
        }
    }
    if (!count) return;
    at = backwards ? (at <= 0 ? count - 1 : at - 1) : (at + 1) % count;
    focused_node = list[at];
    /* Bring it into view. */
    if (focused_node->y < scroll || focused_node->y + focused_node->h > scroll + view_height())
        scroll = focused_node->y - view_height() / 3;
    if (scroll > max_scroll()) scroll = max_scroll();
    if (scroll < 0) scroll = 0;
    draw();
}

static void type_into(Node *n, int k) {
    size_t len = n->value ? strlen(n->value) : 0;
    if (k == 8) {
        if (len) n->value[len - 1] = 0;
    } else if ((k >= 32 && k < 127) || (k == '\n' && is(n->tag, "textarea"))) {
        char *bigger = realloc(n->value, len + 2);
        if (!bigger) return;
        bigger[len] = (char)k;
        bigger[len + 1] = 0;
        n->value = bigger;
    } else if (k == CTRL('U')) {
        if (n->value) n->value[0] = 0;
    }
    js_fire(n, "input", 1, 0);
    draw();
}

/* A key for scripts: keydown at the focused element (or the page).
   Returns 1 if a script prevented the browser's own handling. */
static int key_event(int k) {
    if (!js_enabled || !global_obj) return 0;
    const char *name = NULL;
    int code = k;
    switch (k) {
    case '\n': name = "Enter"; code = 13; break;
    case 8: name = "Backspace"; break;
    case '\t': name = "Tab"; code = 9; break;
    case KEY_ESCAPE: name = "Escape"; code = 27; break;
    case KEY_UP: name = "ArrowUp"; code = 38; break;
    case KEY_DOWN: name = "ArrowDown"; code = 40; break;
    case KEY_LEFT: name = "ArrowLeft"; code = 37; break;
    case KEY_RIGHT: name = "ArrowRight"; code = 39; break;
    case KEY_PAGE_UP: name = "PageUp"; code = 33; break;
    case KEY_PAGE_DOWN: name = "PageDown"; code = 34; break;
    case KEY_HOME: name = "Home"; code = 36; break;
    case KEY_END: name = "End"; code = 35; break;
    case KEY_DELETE: name = "Delete"; code = 46; break;
    }
    char ch[2] = {(char)k, 0};
    if (!name) {
        if (k < 32 || k >= 127) return 0;
        name = ch;
        code = k >= 'a' && k <= 'z' ? k - 32 : k;
    }
    JsObj *e = new_event("keydown", 1, 1);
    prop_set_raw(e, intern_c("key"), v_str(str_c(name)), 0);
    prop_set_raw(e, intern_c("code"), v_str(str_c(name)), 0);
    prop_set_raw(e, intern_c("keyCode"), v_num(code), 0);
    prop_set_raw(e, intern_c("which"), v_num(code), 0);
    Node *target = focused_node ? focused_node : body_element;
    int prevented = js_dispatch(target, e);
    JsObj *up = new_event("keyup", 1, 1);
    prop_set_raw(up, intern_c("key"), v_str(str_c(name)), 0);
    prop_set_raw(up, intern_c("keyCode"), v_num(code), 0);
    js_dispatch(target, up);
    return prevented;
}

static void key(int k) {
    if (editing) {
        if (selected_all && ((k >= 32 && k < 127) || k == 8 || k == KEY_DELETE)) address[0] = 0;
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_END || k == KEY_HOME) {
            selected_all = 0;
            draw();
            return;
        }
        if (k != '\n') selected_all = 0;
        size_t n = strlen(address);
        if (k == '\n') {
            static char url[1100];
            if (!strstr(address, "://") && address[0] != '/' && address[0] != '.' && strchr(address, '.') && !strstr(address, ".htm") &&
                !strchr(address, ' ')) {
                strcpy(url, "https://");
                strncat_safe(url, address, 1000);
            } else if (strchr(address, ' ') || (!strchr(address, '.') && address[0] != '/' && !strstr(address, "://"))) {
                /* Words, not an address: search for them. */
                strcpy(url, "https://html.duckduckgo.com/html/?q=");
                url_encode(url, sizeof(url), address);
            } else {
                strcpy(url, address);
            }
            go(url, 1, NULL);
        } else if (k == KEY_ESCAPE) {
            editing = 0;
            draw();
        } else if (k == 8 || k == KEY_DELETE) {
            if (n) address[n - 1] = 0;
            draw();
        } else if (k == CTRL('U')) {
            address[0] = 0;
            draw();
        } else if (k >= 32 && k < 127 && n < sizeof(address) - 1) {
            address[n] = (char)k;
            address[n + 1] = 0;
            draw();
        }
        return;
    }
    if (key_event(k) || js_navigate) {
        after_js();
        return;
    }
    if (k == '\t') {
        focus_next(0);
        return;
    }
    Node *f = focused_node;
    if (f && is_text_control(f)) {
        if (k == '\n' && !is(f->tag, "textarea")) {
            if (form_of(f)) submit(form_of(f), NULL);
            return;
        }
        if (k == KEY_ESCAPE) {
            focused_node = NULL;
            draw();
            return;
        }
        if (k != KEY_UP && k != KEY_DOWN && k != KEY_PAGE_UP && k != KEY_PAGE_DOWN) {
            type_into(f, k);
            return;
        }
    } else if (f && (k == '\n' || k == ' ')) {
        if (activate(f) && !navigate_requested) {
            relayout();
            draw();
        }
        return;
    }
    int page = view_height() - 40;
    switch (k) {
    case KEY_DOWN: scroll_by(54); break;
    case KEY_UP: scroll_by(-54); break;
    case KEY_PAGE_DOWN: case ' ': scroll_by(page); break;
    case KEY_PAGE_UP: scroll_by(-page); break;
    case KEY_HOME: scroll_by(-scroll); break;
    case KEY_END: scroll_by(max_scroll()); break;
    case 8: back(); break;
    case CTRL('L'):
        start_editing();
        draw();
        break;
    case CTRL('R'): go(current_url, 0, NULL); break;
    }
}

static int is_checkbox(Node *n) {
    char type[16] = "";
    return n && n->type == NODE_ELEMENT && is(n->tag, "input") && attr(n, "type", type, sizeof(type)) && !strcasecmp_word(type, "checkbox");
}

/* A checkbox clicked (directly, or through its label): as in browsers, it
   changes before the click event, back again if a script cancels it,
   and input and change follow. */
static void checkbox_clicked(Node *box, int already_toggled, int prevented) {
    if (!already_toggled) box->checked = !box->checked;
    if (prevented) {
        box->checked = !box->checked;
        return;
    }
    js_fire(box, "input", 1, 0);
    js_fire(box, "change", 1, 0);
}

static JsObj *mouse_event(const char *type, int bubbles, Node *t, int x, int vy, int buttons);

static Node *control_for_label(Node *label) {
    char id[64], other[64];
    if (attr(label, "for", id, sizeof(id))) {
        for (Node *o = document; o; o = next_in_tree(o, document)) {
            if (o->type == NODE_ELEMENT && attr(o, "id", other, sizeof(other)) && !strcmp(id, other)) return o;
        }
        return NULL;
    }
    for (Node *o = label; o; o = next_in_tree(o, label)) {
        if (o->type == NODE_ELEMENT && (is(o->tag, "input") || is(o->tag, "select") || is(o->tag, "textarea"))) return o;
    }
    return NULL;
}

static void click(int x, int y) {
    if (y < BAR) {
        if (x >= 6 && x < 32) back();
        else if (x >= 36 && x < 62) go(current_url, 0, NULL);
        else if (x >= 70) {
            start_editing();
            draw();
        }
        return;
    }
    if (editing) {
        editing = 0;
        draw();
    }
    if (y >= H - STATUS) return;
    int vy = y - BAR;
    if (x >= PAGE_W) {
        int track = view_height();
        int thumb = page_height > track ? (int)((long long)track * track / page_height) : track;
        int thumb_top = max_scroll() ? (int)((long long)(track - thumb) * scroll / max_scroll()) : 0;
        scroll_by(vy < thumb_top ? -(track - 40) : track - 40);
        return;
    }
    click_page(x, vy);
    if (frames_changed) {
        frames_changed = 0;
        draw();
    }
}

/* A click at (x, vy) in the page's view (in the running realm). */
static int click_page(int x, int vy) {
    for (Node *f = node_at(x, vy + scroll); f; f = f->parent) { /* in a frame: its page's */
        if (f->type == NODE_ELEMENT && is(f->tag, "iframe") && frame_click(f, x, vy + scroll)) return 1;
    }
    int i = item_at(x, vy + scroll);
    Node *target = i >= 0 ? real_node(items[i].node) : NULL;
    if (i < 0 || items[i].kind == IT_RECT) { /* not on text: the innermost box there */
        Node *box = node_at(x, vy + scroll);
        if (box) target = box;
    }
    static char href[2048];
    href[0] = 0;
    Node *toggled = NULL;
    if (i >= 0 && items[i].link >= 0) resolve_url(base_url, links[items[i].link], href, sizeof(href));
    /* Scripts first: they may handle the click themselves. */
    if (js_enabled && global_obj) {
        Node *t = target ? target : body_element;
        if (is_checkbox(t)) { /* (changed before the click is seen) */
            t->checked = !t->checked;
            toggled = t;
        }
        static const char *const before[] = {"pointerover", "pointerenter", "mouseover", "mouseenter", "pointermove", "mousemove",
                                             "pointerdown", "mousedown", "focus", "pointerup", "mouseup"};
        int prevented = 0;
        for (int k = 0; k < 12; k++) { /* the events a real click makes, then the click */
            JsObj *e = new_event(k < 11 ? before[k] : "click", k < 11 ? strcmp(before[k], "mouseenter") && strcmp(before[k], "pointerenter") && strcmp(before[k], "focus") : 1, 1);
            prop_set_raw(e, intern_c("clientX"), v_num(x), 0);
            prop_set_raw(e, intern_c("clientY"), v_num(vy), 0);
            prop_set_raw(e, intern_c("pageX"), v_num(x), 0);
            prop_set_raw(e, intern_c("pageY"), v_num(vy + scroll), 0);
            prop_set_raw(e, intern_c("screenX"), v_num(x + 112), 0);
            prop_set_raw(e, intern_c("screenY"), v_num(vy + 146), 0);
            prop_set_raw(e, intern_c("offsetX"), v_num(t && t->w ? x - t->x : 0), 0);
            prop_set_raw(e, intern_c("offsetY"), v_num(t && t->h ? vy + scroll - t->y : 0), 0);
            prop_set_raw(e, intern_c("button"), v_num(0), 0);
            prop_set_raw(e, intern_c("buttons"), v_num(k >= 6 && k < 9 ? 1 : 0), 0);
            prop_set_raw(e, intern_c("which"), v_num(1), 0);
            prop_set_raw(e, intern_c("detail"), v_num(1), 0);
            prop_set_raw(e, intern_c("pointerId"), v_num(1), 0);
            prop_set_raw(e, intern_c("pointerType"), v_str(intern_c("mouse")), 0);
            prop_set_raw(e, intern_c("isPrimary"), v_bool(1), 0);
            int p = js_dispatch(t, e);
            if (k == 11) prevented = p;
            if (js_navigate) break;
        }
        if (toggled) {
            focused_node = toggled;
            checkbox_clicked(toggled, 1, prevented);
            after_js();
            relayout();
            draw();
            return 1;
        }
        if (prevented || js_navigate) {
            after_js();
            return 1;
        }
        if (dom_dirty) after_js(); /* (the clicked node is still there: scripts hold it) */
    }
    /* Form controls and labels. */
    for (Node *n = target; n && n != document; n = n->parent) {
        if (n->type != NODE_ELEMENT) continue;
        if (is(n->tag, "input") || is(n->tag, "select") || is(n->tag, "textarea") || is(n->tag, "button")) {
            focused_node = n;
            if (!is_text_control(n) && activate(n)) {
                if (navigate_requested) return 1;
                relayout();
            }
            draw();
            return 1;
        }
        if (is(n->tag, "label")) {
            Node *control = control_for_label(n);
            if (control && is_checkbox(control) && js_enabled && global_obj) { /* the label clicks its checkbox */
                focused_node = control;
                control->checked = !control->checked;
                int p = js_dispatch(control, mouse_event("click", 1, control, x, vy, 0));
                checkbox_clicked(control, 1, p);
                after_js();
                relayout();
                draw();
                return 1;
            }
            if (control) {
                focused_node = control;
                if (!is_text_control(control)) activate(control);
                if (navigate_requested) return 1;
                relayout();
                draw();
                return 1;
            }
        }
    }
    if (href[0]) {
        char *url = href;
        if (!strncasecmp(url, "javascript:", 11)) { /* run it */
            if (js_enabled && global_obj) {
                js_run_script(url + 11, (int)strlen(url + 11), "link");
                run_microtasks();
                after_js();
            }
            return 1;
        }
        if (!strncasecmp(url, "mailto:", 7)) return 1;
        if (!strncasecmp(url, "rdx-proceed:", 12)) { /* the user chose to open an unverified site */
            allow_insecure(url + 12);
            go(url + 12, 0, NULL);
            return 1;
        }
        go(url, 1, NULL);
        return 1;
    }
    if (focused_node) {
        focused_node = NULL;
        draw();
    }
    return 1;
}

static JsObj *mouse_event(const char *type, int bubbles, Node *t, int x, int vy, int buttons) {
    JsObj *e = new_event(type, bubbles, 1);
    prop_set_raw(e, intern_c("clientX"), v_num(x), 0);
    prop_set_raw(e, intern_c("clientY"), v_num(vy), 0);
    prop_set_raw(e, intern_c("pageX"), v_num(x), 0);
    prop_set_raw(e, intern_c("pageY"), v_num(vy + scroll), 0);
    prop_set_raw(e, intern_c("offsetX"), v_num(t && t->w ? x - t->x : 0), 0);
    prop_set_raw(e, intern_c("offsetY"), v_num(t && t->h ? vy + scroll - t->y : 0), 0);
    prop_set_raw(e, intern_c("button"), v_num(0), 0);
    prop_set_raw(e, intern_c("buttons"), v_num(buttons), 0);
    prop_set_raw(e, intern_c("which"), v_num(1), 0);
    prop_set_raw(e, intern_c("detail"), v_num(1), 0);
    return e;
}

static int last_buttons;

static void handle(gui_message *m) {
    if (m->type == MSG_KEY) key(m->a);
    if (m->type == MSG_MOUSE) {
        int pressed = m->c & ~last_buttons;
        last_buttons = m->c;
        if (pressed & MOUSE_LEFT) click(m->a, m->b);
    }
}

int main(int argc, char **argv) {
    char stack_top;
    realm_boot(&stack_top); /* (first: it keeps the page code's statics as they start) */
    GUI_PROGRAM();
    js_stack_top = &stack_top; /* for collecting garbage while scripts run */
    if (getenv("BROWSER_CONSOLE")) console_echo = 1;
    if (getenv("BROWSER_TRACE")) js_trace_throws = 1;
    if (getenv("BROWSER_NOJS")) js_enabled = 0; /* scripts off */
    if (argc > 2 && strcmp(argv[1], "--dump") == 0) return dump(argv[2]);
    if (gui_connect() < 0) {
        printf("browser: the desktop is not running (start it with: desktop)\n");
        printf("         or print a page as text with: browser --dump <address>\n");
        return 1;
    }
    win = gui_window_new(W, H, "Browser");
    if (!win) return 1;
    go(argc > 1 ? argv[1] : HOME, 0, NULL);
    gui_message m;
    for (;;) {
        int got = gui_next(&m, realms_next_timer());
        if (got < 0) return 0;
        if (got > 0) {
            if (m.type == MSG_CLOSE) break;
            handle(&m);
        }
        js_run_timers();
        after_js();
        realms_run_others();
        if (frames_changed) {
            frames_changed = 0;
            draw();
        }
    }
    cookies_save();
    gui_window_close(win);
    return 0;
}
