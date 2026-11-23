/* web/paint.c - drawing the display list, and finding what is under the
   pointer. Part of the browser. */

static Node *focused_control(void) { return focused_node; }
static int frame_paint(Node *n, canvas *v, int x, int y); /* (realm.c) */

static int is_text_control(Node *n) {
    if (!n || n->type != NODE_ELEMENT) return 0;
    if (is(n->tag, "textarea")) return 1;
    if (!is(n->tag, "input")) return 0;
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    return strcasecmp_word(type, "checkbox") && strcasecmp_word(type, "radio") && strcasecmp_word(type, "submit") &&
           strcasecmp_word(type, "button") && strcasecmp_word(type, "reset") && strcasecmp_word(type, "image") &&
           strcasecmp_word(type, "hidden") && strcasecmp_word(type, "file") && strcasecmp_word(type, "range") &&
           strcasecmp_word(type, "color");
}

static void draw_control(canvas *v, Item *it, int x, int y) {
    Node *n = it->node;
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    unsigned color = n->style.color;
    if (is(n->tag, "input") && (!strcasecmp_word(type, "checkbox") || !strcasecmp_word(type, "radio"))) {
        int radio = !strcasecmp_word(type, "radio");
        fill_rect(v, x, y, 13, 13, 0xFFFFFF);
        draw_rect(v, x, y, 13, 13, n == focused_control() ? 0x3D7DFF : 0x767676);
        if (n->checked) {
            if (radio) fill_rect(v, x + 4, y + 4, 5, 5, 0x1A5FD6);
            else { /* a tick: down from (3,6) to (5,9), then up to (10,3) */
                for (int i = 0; i <= 3; i++) fill_rect(v, x + 3 + i * 2 / 3, y + 6 + i, 2, 2, 0x1A5FD6);
                for (int i = 0; i <= 6; i++) fill_rect(v, x + 5 + i * 5 / 6, y + 9 - i, 2, 2, 0x1A5FD6);
            }
        }
        return;
    }
    if (is(n->tag, "input") && (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "button") || !strcasecmp_word(type, "reset"))) {
        char label[64];
        control_label(n, label, sizeof(label));
        int len = (int)strlen(label);
        draw_glyphs(v, x + (it->w - len * 8) / 2, y + (it->h - 16) / 2, label, len, 8, color, 0);
        return;
    }
    if (is(n->tag, "select")) {
        char label[80];
        control_label(n, label, sizeof(label));
        int fit = (it->w - 16) / 8;
        int len = (int)strlen(label);
        draw_glyphs(v, x, y + (it->h - 16) / 2, label, len < fit ? len : fit, 8, color, 0);
        draw_glyphs(v, x + it->w - 10, y + (it->h - 16) / 2, "v", 1, 8, color, 0);
        return;
    }
    if (is(n->tag, "progress") || is(n->tag, "meter")) {
        char vbuf[16], mbuf[16];
        int value = attr(n, "value", vbuf, sizeof(vbuf)) ? atoi(vbuf) : 0;
        int max = attr(n, "max", mbuf, sizeof(mbuf)) ? atoi(mbuf) : 1;
        if (max <= 0) max = 1;
        fill_rect(v, x, y + 3, it->w, it->h - 6, 0xE0E0E0);
        fill_rect(v, x, y + 3, (int)((long long)it->w * (value > max ? max : value) / max), it->h - 6, 0x3D7DFF);
        return;
    }
    if (is_text_control(n)) {
        const char *text = n->value ? n->value : "";
        int password = is(n->tag, "input") && !strcasecmp_word(type, "password");
        char shown[256];
        int len = (int)strlen(text);
        if (len > 255) len = 255;
        for (int i = 0; i < len; i++) shown[i] = password ? '*' : text[i];
        unsigned text_color = color;
        if (!len) { /* the placeholder, dimmed */
            if (attr(n, "placeholder", shown, sizeof(shown))) {
                len = (int)strlen(shown);
                text_color = 0x8A8F9C;
            }
        }
        int fit = it->w / 8;
        int start = len > fit ? len - fit : 0;
        if (is(n->tag, "textarea")) {
            /* Lines, wrapped to the box. */
            int col = 0, row = 0, rows = it->h / 18;
            for (int i = 0; i < len && row < rows; i++) {
                if (shown[i] == '\n' || col >= fit) {
                    row++;
                    col = 0;
                    if (shown[i] == '\n') continue;
                }
                if (row < rows) draw_glyphs(v, x + col * 8, y + row * 18, shown + i, 1, 8, text_color, 0);
                col++;
            }
            if (n == focused_control()) fill_rect(v, x + col * 8, y + row * 18, 2, 16, 0x1A5FD6);
            return;
        }
        draw_glyphs(v, x, y + (it->h - 16) / 2, shown + start, len - start, 8, text_color, 0);
        if (n == focused_control()) fill_rect(v, x + (n->value ? (int)strlen(n->value) - start : 0) * 8, y + (it->h - 16) / 2, 2, 16, 0x1A5FD6);
        return;
    }
    /* svg: its drawing, once made; video, canvas, iframe, object: a labelled box */
    if (is(n->tag, "svg")) {
        if (n->img_state == 1 && n->img) image_draw(v, (image *)n->img, x, y, it->w, it->h);
        return;
    }
    if (is(n->tag, "iframe") && frame_paint(n, v, x, y)) return;
    fill_rect(v, x, y, it->w, it->h, 0xEDEFF3);
    draw_rect(v, x, y, it->w, it->h, 0xC9CDD6);
    char label[32] = "[";
    strcpy(label + 1, n->tag);
    strcpy(label + strlen(label), "]");
    if (it->w > 48) draw_glyphs(v, x + 6, y + 6, label, (int)strlen(label), 8, 0x8A8F9C, 0);
}

/* Draw the page, `scroll` pixels down, into `v` (the visible part). */
static void paint_page(canvas *v, int scroll, int left) {
    fill_rect(v, 0, 0, v->width, v->height, page_bg);
    canvas whole = *v;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        int x = it->x + left, y = it->y - scroll;
        if (y > whole.height || y + it->h < 0 || it->w <= 0) continue;
        v = &whole;
        canvas part;
        if (it->clipped) {
            /* Draw into the clip rectangle's part of the view. */
            int x0 = it->clip[0] + left, y0 = it->clip[1] - scroll, x1 = it->clip[2] + left, y1 = it->clip[3] - scroll;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > whole.width) x1 = whole.width;
            if (y1 > whole.height) y1 = whole.height;
            if (x1 <= x0 || y1 <= y0) continue;
            part.pixels = whole.pixels + (size_t)y0 * (size_t)whole.stride + (size_t)x0;
            part.width = x1 - x0;
            part.height = y1 - y0;
            part.stride = whole.stride;
            v = &part;
            x -= x0;
            y -= y0;
        }
        switch (it->kind) {
        case IT_RECT:
            fill_rect(v, x, y, it->w, it->h, it->color);
            break;
        case IT_BORDER:
            if (it->border[0] && !(it->border_color[0] & BORDER_INVISIBLE)) fill_rect(v, x, y, it->w, it->border[0], it->border_color[0]);
            if (it->border[2] && !(it->border_color[2] & BORDER_INVISIBLE))
                fill_rect(v, x, y + it->h - it->border[2], it->w, it->border[2], it->border_color[2]);
            if (it->border[3] && !(it->border_color[3] & BORDER_INVISIBLE)) fill_rect(v, x, y, it->border[3], it->h, it->border_color[3]);
            if (it->border[1] && !(it->border_color[1] & BORDER_INVISIBLE))
                fill_rect(v, x + it->w - it->border[1], y, it->border[1], it->h, it->border_color[1]);
            break;
        case IT_TEXT:
        case IT_MARKER: {
            int flags = (it->bold ? GLYPH_BOLD : 0) | (it->italic ? GLYPH_ITALIC : 0);
            draw_glyphs(v, x, y, pool + it->text, it->len, it->cell, it->color, flags);
            if (it->underline) fill_rect(v, x, y + it->h - 1, it->w, 1, it->color);
            if (it->strike) fill_rect(v, x, y + it->h / 2, it->w, 1, it->color);
            break;
        }
        case IT_IMAGE: {
            Node *n = it->node;
            if (n->img_state == 1 && n->img) {
                image_draw(v, (image *)n->img, x, y, it->w, it->h);
            } else if (it->w > 4 && it->h > 4) { /* not (yet) shown: its description, if it has one */
                char alt[80];
                if (attr(n, "alt", alt, sizeof(alt)) && alt[0] && it->h >= 18) {
                    int len = (int)strlen(alt), fit = (it->w - 4) / 8;
                    canvas inside = *v;
                    draw_glyphs(&inside, x + 2, y + 1, alt, len < fit ? len : fit, 8, 0x6A7287, 0);
                }
            }
            break;
        }
        case IT_CONTROL:
            draw_control(v, it, x, y);
            break;
        }
    }
}

/* The deepest element whose box holds page point (px, py). */
static Node *node_at(int px, int py) {
    Node *found = NULL;
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || n == document) continue;
        if (n->style.display == D_NONE) continue;
        if (n->w > 0 && n->h > 0 && px >= n->x && px < n->x + n->w && py >= n->y && py < n->y + n->h) found = n;
    }
    return found;
}

/* The item under page point (px, py): the topmost text, picture or
   control; -1 if there is none. */
static int item_at(int px, int py) {
    for (int i = item_count - 1; i >= 0; i--) {
        Item *it = &items[i];
        if (it->kind == IT_BORDER || it->kind == IT_MARKER) continue;
        if (it->clipped && (px < it->clip[0] || px >= it->clip[2] || py < it->clip[1] || py >= it->clip[3])) continue;
        int slack = it->kind == IT_TEXT ? it->cell / 2 + 1 : 0; /* half a space: the gaps between words */
        if (px >= it->x - slack && px < it->x + it->w + slack && py >= it->y - slack && py < it->y + it->h + slack) {
            if (it->kind == IT_RECT && !it->node) continue;
            return i;
        }
    }
    return -1;
}
