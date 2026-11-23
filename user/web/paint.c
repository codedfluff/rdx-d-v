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

/* Text of a control in its font, vertically centred in h. Returns its width. */
static int control_text(canvas *v, Node *n, int x, int y, int h, const char *s, int len, unsigned color) {
    int a, d, g;
    font_metrics(FONT_OF(&n->style), &a, &d, &g);
    font_draw(v, x, y + (h - a - d) / 2 + a, s, len, FONT_OF(&n->style), color);
    return text_w(&n->style, s, len);
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
        control_text(v, n, x + (it->w - text_w(&n->style, label, len)) / 2, y, it->h, label, len, color);
        return;
    }
    if (is(n->tag, "select")) {
        char label[80];
        control_label(n, label, sizeof(label));
        int len = (int)strlen(label);
        while (len > 0 && text_w(&n->style, label, len) > it->w - 16) len--;
        control_text(v, n, x, y, it->h, label, len, color);
        /* the arrow */
        int ax = x + it->w - 10, ay = y + it->h / 2 - 2;
        for (int i = 0; i < 4; i++) fill_rect(v, ax + i, ay + i, 8 - i * 2, 1, color);
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
        int a, d, g, lh = line_metrics(&n->style, NULL);
        font_metrics(FONT_OF(&n->style), &a, &d, &g);
        if (is(n->tag, "textarea")) {
            /* Lines, wrapped to the box. */
            int col = 0, row = 0, rows = it->h / (lh > 0 ? lh : 1);
            for (int i = 0; i < len && row < rows; i++) {
                int cw = text_w(&n->style, shown + i, 1);
                if (shown[i] == '\n' || col + cw > it->w) {
                    row++;
                    col = 0;
                    if (shown[i] == '\n') continue;
                }
                if (row < rows) font_draw(v, x + col, y + row * lh + a, shown + i, 1, FONT_OF(&n->style), text_color);
                col += cw;
            }
            if (n == focused_control()) fill_rect(v, x + col, y + row * lh, 1, a + d, 0x000000);
            return;
        }
        int start = 0;
        while (start < len && text_w(&n->style, shown + start, len - start) > it->w - 2) start++;
        int w = control_text(v, n, x, y, it->h, shown + start, len - start, text_color);
        if (n == focused_control()) {
            int caret = n->value && n->value[0] ? w : 0;
            fill_rect(v, x + caret, y + (it->h - a - d) / 2, 1, a + d, 0x000000);
        }
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
    if (it->w > 48) font_draw(v, x + 6, y + 18, label, (int)strlen(label), FACE_SANS, 0, 0, 13, 0x8A8F9C);
}

/* ---------------------------------------------------------------- see-through colours and round corners */

static unsigned paint_opacity = 255; /* the item's element's opacity */

/* A colour's alpha with the element's opacity applied: 0..255. */
static unsigned color_alpha(unsigned color) { return COLOR_ALPHA(color) * paint_opacity / 255; }

static void blend_px(unsigned *p, unsigned rgb, unsigned a) {
    if (a >= 255) {
        *p = rgb & 0xFFFFFF;
        return;
    }
    unsigned d = *p, inv = 255 - a;
    unsigned r = ((rgb >> 16 & 255) * a + (d >> 16 & 255) * inv + 127) / 255;
    unsigned g = ((rgb >> 8 & 255) * a + (d >> 8 & 255) * inv + 127) / 255;
    unsigned b = ((rgb & 255) * a + (d & 255) * inv + 127) / 255;
    *p = r << 16 | g << 8 | b;
}

static void paint_fill(canvas *v, int x, int y, int w, int h, unsigned color) {
    unsigned a = color_alpha(color);
    if (!a) return;
    if (a >= 255) {
        fill_rect(v, x, y, w, h, color & 0xFFFFFF);
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > v->width ? v->width : x + w, y1 = y + h > v->height ? v->height : y + h;
    for (int py = y0; py < y1; py++) {
        unsigned *row = v->pixels + (size_t)py * (size_t)v->stride;
        for (int px = x0; px < x1; px++) blend_px(&row[px], color, a);
    }
}

/* Corner radii, resolved for a w x h box: across and down for top-left,
   top-right, bottom-right, bottom-left; scaled down to fit as CSS says. */
typedef struct {
    float rx[4], ry[4];
} Radii;

static int radii_for(const Style *s, int w, int h, Radii *r) {
    int any = 0;
    for (int i = 0; i < 4; i++) {
        int a = s->radius[i], d = s->radius[i + 4];
        r->rx[i] = a >= 0 ? (float)a : (float)w * (float)-a / 100.0f;
        r->ry[i] = d >= 0 ? (float)d : (float)h * (float)-d / 100.0f;
        if (r->rx[i] <= 0 || r->ry[i] <= 0) r->rx[i] = r->ry[i] = 0;
        any |= r->rx[i] > 0;
    }
    if (!any) return 0;
    float f = 1, t;
    if ((t = r->rx[0] + r->rx[1]) > (float)w && (float)w / t < f) f = (float)w / t;
    if ((t = r->rx[3] + r->rx[2]) > (float)w && (float)w / t < f) f = (float)w / t;
    if ((t = r->ry[0] + r->ry[3]) > (float)h && (float)h / t < f) f = (float)h / t;
    if ((t = r->ry[1] + r->ry[2]) > (float)h && (float)h / t < f) f = (float)h / t;
    for (int i = 0; i < 4; i++) {
        r->rx[i] *= f;
        r->ry[i] *= f;
    }
    return 1;
}

/* Is point (X, Y) inside a w x h box with round corners? */
static int round_inside(const Radii *r, float w, float h, float X, float Y) {
    if (X < 0 || Y < 0 || X >= w || Y >= h) return 0;
    float cx, cy, rx, ry;
    if (X < r->rx[0] && Y < r->ry[0]) rx = r->rx[0], ry = r->ry[0], cx = rx, cy = ry;
    else if (X > w - r->rx[1] && Y < r->ry[1]) rx = r->rx[1], ry = r->ry[1], cx = w - rx, cy = ry;
    else if (X > w - r->rx[2] && Y > h - r->ry[2]) rx = r->rx[2], ry = r->ry[2], cx = w - rx, cy = h - ry;
    else if (X < r->rx[3] && Y > h - r->ry[3]) rx = r->rx[3], ry = r->ry[3], cx = rx, cy = h - ry;
    else return 1;
    float dx = (X - cx) / rx, dy = (Y - cy) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

/* How much of pixel (px, py) the shape covers, 0..16. */
static int round_cover(const Radii *r, float w, float h, int px, int py) {
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) n += round_inside(r, w, h, (float)px + (float)sx * 0.25f + 0.125f, (float)py + (float)sy * 0.25f + 0.125f);
    return n;
}

/* Fill a box with round corners at (x, y); with `inner` set, only the
   ring between it and the box inset by bt/br/bb/bl (a border). */
static void paint_round(canvas *v, int x, int y, int w, int h, const Radii *r, int inner, int bt, int br, int bb, int bl, unsigned color) {
    unsigned a = color_alpha(color);
    if (!a || w <= 0 || h <= 0) return;
    Radii in;
    float iw = (float)(w - bl - br), ih = (float)(h - bt - bb);
    if (inner) {
        static const int side_x[4] = {3, 1, 1, 3}, side_y[4] = {0, 0, 2, 2};
        int widths[4] = {bt, br, bb, bl};
        for (int i = 0; i < 4; i++) {
            in.rx[i] = r->rx[i] - (float)widths[side_x[i]];
            in.ry[i] = r->ry[i] - (float)widths[side_y[i]];
            if (in.rx[i] < 0 || in.ry[i] < 0) in.rx[i] = in.ry[i] = 0;
        }
    }
    float top_zone = r->ry[0] > r->ry[1] ? r->ry[0] : r->ry[1], bottom_zone = r->ry[2] > r->ry[3] ? r->ry[2] : r->ry[3];
    float left_zone = r->rx[0] > r->rx[3] ? r->rx[0] : r->rx[3], right_zone = r->rx[1] > r->rx[2] ? r->rx[1] : r->rx[2];
    for (int py = 0; py < h; py++) {
        int vy = y + py;
        if (vy < 0 || vy >= v->height) continue;
        unsigned *row = v->pixels + (size_t)vy * (size_t)v->stride;
        int corner_row = (float)py < top_zone + 1 || (float)py >= (float)h - bottom_zone - 1;
        int band = py < bt || py >= h - bb; /* a row of the top or bottom border */
        for (int px = 0; px < w; px++) {
            int vx = x + px;
            if (vx < 0 || vx >= v->width) continue;
            int corner = corner_row && ((float)px < left_zone + 1 || (float)px >= (float)w - right_zone - 1);
            int cover;
            if (corner) {
                cover = round_cover(r, (float)w, (float)h, px, py);
                if (inner && cover) cover -= round_cover(&in, iw, ih, px - bl, py - bt);
            } else if (inner) {
                cover = band || px < bl || px >= w - br ? 16 : 0;
            } else {
                cover = 16;
            }
            if (cover <= 0) continue;
            blend_px(&row[vx], color, a * (unsigned)cover / 16);
        }
    }
}

/* Draw the page, `scroll` pixels down, into `v` (the visible part). */
static void paint_page(canvas *v, int scroll, int left) {
    fill_rect(v, 0, 0, v->width, v->height, page_bg & 0xFFFFFF);
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
        paint_opacity = it->node && it->node->type == NODE_ELEMENT ? it->node->style.alpha : 255;
        if (!paint_opacity) continue;
        Radii radii;
        int round = it->node && it->node->type == NODE_ELEMENT && (it->kind == IT_RECT || it->kind == IT_BORDER || it->kind == IT_IMAGE) &&
                    radii_for(&it->node->style, it->w, it->h, &radii);
        switch (it->kind) {
        case IT_RECT:
            if (round) paint_round(v, x, y, it->w, it->h, &radii, 0, 0, 0, 0, 0, it->color);
            else paint_fill(v, x, y, it->w, it->h, it->color);
            break;
        case IT_BORDER:
            if (round) { /* one colour all round (the first side shown) */
                unsigned c = it->border_color[0];
                for (int k = 0; k < 4; k++)
                    if (it->border[k] && it->border_color[k] != BORDER_INVISIBLE) {
                        c = it->border_color[k];
                        break;
                    }
                paint_round(v, x, y, it->w, it->h, &radii, 1, it->border[0], it->border[1], it->border[2], it->border[3], c);
                break;
            }
            if (it->border[0] && it->border_color[0] != BORDER_INVISIBLE) paint_fill(v, x, y, it->w, it->border[0], it->border_color[0]);
            if (it->border[2] && it->border_color[2] != BORDER_INVISIBLE)
                paint_fill(v, x, y + it->h - it->border[2], it->w, it->border[2], it->border_color[2]);
            if (it->border[3] && it->border_color[3] != BORDER_INVISIBLE) paint_fill(v, x, y, it->border[3], it->h, it->border_color[3]);
            if (it->border[1] && it->border_color[1] != BORDER_INVISIBLE)
                paint_fill(v, x + it->w - it->border[1], y, it->border[1], it->h, it->border_color[1]);
            break;
        case IT_TEXT:
        case IT_MARKER: {
            unsigned color = (it->color & 0xFFFFFF) | (255 - color_alpha(it->color)) << 24;
            font_draw(v, x, y + it->base, pool + it->text, it->len, it->face, it->bold, it->italic, it->size, color);
            int thick = it->size >= 24 ? 2 : 1;
            if (it->underline) paint_fill(v, x, y + it->base + (it->size + 8) / 10, it->w, thick, it->color);
            if (it->strike) paint_fill(v, x, y + it->base - it->base * 3 / 10, it->w, thick, it->color);
            break;
        }
        case IT_IMAGE: {
            Node *n = it->node;
            if (n->img_state == 1 && n->img && round) { /* round corners (a round picture): drawn aside, then masked */
                canvas aside = canvas_new(it->w, it->h);
                if (aside.pixels) {
                    for (int py = 0; py < it->h; py++) /* (what is under it, so see-through pictures blend) */
                        for (int px = 0; px < it->w; px++) {
                            int vx = x + px, vy = y + py;
                            aside.pixels[(size_t)py * (size_t)aside.stride + (size_t)px] =
                                vx >= 0 && vy >= 0 && vx < v->width && vy < v->height ? v->pixels[(size_t)vy * (size_t)v->stride + (size_t)vx] : 0xFFFFFF;
                        }
                    image_draw(&aside, (image *)n->img, 0, 0, it->w, it->h);
                    for (int py = 0; py < it->h; py++) {
                        int vy = y + py;
                        if (vy < 0 || vy >= v->height) continue;
                        for (int px = 0; px < it->w; px++) {
                            int vx = x + px;
                            if (vx < 0 || vx >= v->width) continue;
                            int cover = round_cover(&radii, (float)it->w, (float)it->h, px, py);
                            if (cover) blend_px(&v->pixels[(size_t)vy * (size_t)v->stride + (size_t)vx], aside.pixels[(size_t)py * (size_t)aside.stride + (size_t)px], paint_opacity * (unsigned)cover / 16);
                        }
                    }
                    free(aside.pixels);
                } else {
                    image_draw(v, (image *)n->img, x, y, it->w, it->h);
                }
            } else if (n->img_state == 1 && n->img) {
                image_draw(v, (image *)n->img, x, y, it->w, it->h);
            } else if (it->w > 4 && it->h > 4) { /* not (yet) shown: its description, if it has one */
                char alt[80];
                if (attr(n, "alt", alt, sizeof(alt)) && alt[0] && it->h >= 14) {
                    int len = (int)strlen(alt);
                    while (len > 0 && text_w(&n->style, alt, len) > it->w - 4) len--;
                    int a, d, g;
                    font_metrics(FONT_OF(&n->style), &a, &d, &g);
                    font_draw(v, x + 2, y + 1 + a, alt, len, FONT_OF(&n->style), 0x6A7287);
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
        int slack = it->kind == IT_TEXT ? it->size / 8 + 1 : 0; /* half a space: the gaps between words */
        if (px >= it->x - slack && px < it->x + it->w + slack && py >= it->y - slack && py < it->y + it->h + slack) {
            if (it->kind == IT_RECT && !it->node) continue;
            return i;
        }
    }
    return -1;
}
