/* web/js_dom.c - the page as JavaScript sees it: window, document,
   elements, events, timers, fetch; and the garbage collector's roots.
   Part of the browser. */

static void relayout(void);

#define PAGE_VIEW_W (layout_viewport_w ? layout_viewport_w : 808) /* the page's part of the browser window (or a frame) */
#define PAGE_VIEW_H (layout_viewport_h ? layout_viewport_h : 526)

static int dom_dirty;              /* scripts changed the page: lay it out again */
static char *js_navigate;          /* a script asked to go to this address */
static char js_alert[200];         /* a message from alert() for the status bar */
static int js_scroll_to = -1;      /* a script asked to scroll here */
static int page_scroll_y;          /* the browser's scroll position, for scripts */
static int js_enabled = 1;
static int js_errors;              /* how many errors this page's scripts had */
static Node *current_script;       /* the <script> running (document.write, currentScript) */
static int page_loaded;            /* the load event has fired */

static JsObj *DocumentProto, *StyleProto, *ClassListProto, *DatasetProto, *StorageProto, *XhrProto;
static JsObj *local_storage, *session_storage;

/* Nodes taken out of the page (or made by scripts and not yet put in):
   kept until the page goes, since scripts may still hold them. */
static Node **orphans;
static int orphan_count, orphan_cap;

static void orphan_add(Node *n) {
    if (grow((void **)&orphans, &orphan_cap, orphan_count + 1, sizeof(Node *))) orphans[orphan_count++] = n;
}

/* Names of DOM properties, compared by pointer. */
#define DOM_NAMES(X)                                                                                                    \
    X(nodeType) X(nodeName) X(tagName) X(localName) X(id) X(className) X(classList) X(style) X(dataset) X(textContent) \
    X(innerText) X(innerHTML) X(outerHTML) X(value) X(checked) X(disabled) X(selectedIndex) X(options) X(type)         \
    X(href) X(src) X(title) X(hidden) X(parentNode) X(parentElement) X(childNodes) X(children) X(firstChild)          \
    X(lastChild) X(firstElementChild) X(lastElementChild) X(nextSibling) X(previousSibling) X(nextElementSibling)      \
    X(previousElementSibling) X(childElementCount) X(ownerDocument) X(isConnected) X(nodeValue) X(data)               \
    X(offsetWidth) X(offsetHeight) X(offsetTop) X(offsetLeft) X(offsetParent) X(clientWidth) X(clientHeight)          \
    X(scrollWidth) X(scrollHeight) X(scrollTop) X(scrollLeft) X(form) X(elements) X(body) X(head) X(documentElement)  \
    X(readyState) X(location) X(cookie) X(URL) X(documentURI) X(domain) X(referrer) X(forms) X(images) X(links)       \
    X(scripts) X(activeElement) X(defaultView) X(visibilityState) X(characterSet) X(currentScript) X(cssText)         \
    X(htmlFor) X(name) X(alt) X(placeholder) X(rel) X(target) X(action) X(method) X(lang) X(dir) X(role) X(tabIndex)   \
    X(width) X(height) X(content) X(selected) X(text) X(label) X(colSpan) X(rowSpan) X(naturalWidth)                  \
    X(naturalHeight) X(complete) X(defaultValue) X(defaultChecked) X(readOnly) X(required) X(multiple) X(maxLength)   \
    X(baseURI) X(compatMode) X(contentType) X(fullscreenElement) X(hasFocus) X(attributes) \
    X(protocol) X(host) X(hostname) X(port) X(pathname) X(search) X(hash) X(origin) X(contentWindow) X(contentDocument)

#define DECLARE_NAME(n) static JsStr *K_##n;
DOM_NAMES(DECLARE_NAME)
static JsStr *K_listeners, *K_stop, *K_stop_now, *K_defaultPrevented, *K_target, *K_currentTarget, *K_bubbles,
    *K_cancelable, *K_type, *K_length, *K_status, *K_responseText, *K_readyState, *K_onload, *K_onerror,
    *K_onreadystatechange, *K_response, *K_statusText, *K_responseURL;

static void init_dom_names(void) {
#define INIT_NAME(n) K_##n = intern_c(#n);
    DOM_NAMES(INIT_NAME)
    K_listeners = intern_c("__listeners");
    K_stop = intern_c("__stop");
    K_stop_now = intern_c("__stopNow");
    K_defaultPrevented = intern_c("defaultPrevented");
    K_target = intern_c("target");
    K_currentTarget = intern_c("currentTarget");
    K_bubbles = intern_c("bubbles");
    K_cancelable = intern_c("cancelable");
    K_type = intern_c("type");
    K_length = intern_c("length");
    K_status = intern_c("status");
    K_responseText = intern_c("responseText");
    K_readyState = intern_c("readyState");
    K_onload = intern_c("onload");
    K_onerror = intern_c("onerror");
    K_onreadystatechange = intern_c("onreadystatechange");
    K_response = intern_c("response");
    K_statusText = intern_c("statusText");
    K_responseURL = intern_c("responseURL");
}

/* ---------------------------------------------------------------- wrappers */

/* The DOM's interfaces: each has its own prototype, under one another as
   in browsers (HTMLInputElement.prototype -> HTMLElement.prototype ->
   Element.prototype -> Node.prototype, where the methods all are), so
   instanceof tells them apart. */
static const char *const IFACE_TAGS[][2] = {
    {"div", "HTMLDivElement"}, {"span", "HTMLSpanElement"}, {"a", "HTMLAnchorElement"}, {"img", "HTMLImageElement"},
    {"input", "HTMLInputElement"}, {"button", "HTMLButtonElement"}, {"form", "HTMLFormElement"}, {"select", "HTMLSelectElement"},
    {"option", "HTMLOptionElement"}, {"textarea", "HTMLTextAreaElement"}, {"script", "HTMLScriptElement"},
    {"canvas", "HTMLCanvasElement"}, {"iframe", "HTMLIFrameElement"}, {"video", "HTMLVideoElement"}, {"audio", "HTMLAudioElement"},
    {"p", "HTMLParagraphElement"}, {"ul", "HTMLUListElement"}, {"ol", "HTMLOListElement"}, {"li", "HTMLLIElement"},
    {"body", "HTMLBodyElement"}, {"head", "HTMLHeadElement"}, {"html", "HTMLHtmlElement"}, {"link", "HTMLLinkElement"},
    {"style", "HTMLStyleElement"}, {"meta", "HTMLMetaElement"}, {"title", "HTMLTitleElement"}, {"label", "HTMLLabelElement"},
    {"table", "HTMLTableElement"}, {"tr", "HTMLTableRowElement"}, {"td", "HTMLTableCellElement"}, {"th", "HTMLTableCellElement"},
    {"br", "HTMLBRElement"}, {"hr", "HTMLHRElement"}, {"h1", "HTMLHeadingElement"}, {"h2", "HTMLHeadingElement"},
    {"h3", "HTMLHeadingElement"}, {"h4", "HTMLHeadingElement"}, {"h5", "HTMLHeadingElement"}, {"h6", "HTMLHeadingElement"},
    {"template", "HTMLTemplateElement"}, {"pre", "HTMLPreElement"}, {"source", "HTMLSourceElement"}, {"nav", "HTMLElement"},
    {"slot", "HTMLSlotElement"}, {"dialog", "HTMLDialogElement"}, {"progress", "HTMLProgressElement"}, {"object", "HTMLObjectElement"},
    {"embed", "HTMLEmbedElement"}, {"picture", "HTMLPictureElement"}, {"fieldset", "HTMLFieldSetElement"}, {"legend", "HTMLLegendElement"},
    {"noscript", "HTMLElement"}};
#define IFACE_COUNT ((int)(sizeof(IFACE_TAGS) / sizeof(IFACE_TAGS[0])))
static JsObj *ElementProto, *HTMLElementProto, *SVGElementProto, *CharacterDataProto, *TextProto, *CommentProto, *FragmentProto,
    *ShadowRootProto, *EventTargetProto, *tag_protos[IFACE_COUNT];

static JsObj *proto_for_node(Node *n) {
    if (n == document) return DocumentProto;
    if (!ElementProto) return NodeProto;
    if (n->type == NODE_TEXT) return TextProto;
    if (!strcmp(n->tag, "#shadow-root")) return ShadowRootProto;
    if (n->tag[0] == '#') return FragmentProto;
    for (Node *a = n; a; a = a->parent)
        if (a->type == NODE_ELEMENT && is(a->tag, "svg")) return SVGElementProto;
    for (int i = 0; i < IFACE_COUNT; i++)
        if (is(n->tag, IFACE_TAGS[i][0])) return tag_protos[i];
    return HTMLElementProto;
}

static JsObj *wrap(Node *n) {
    if (!n) return NULL;
    if (n->js) return n->js;
    JsObj *o = obj_new_raw(CL_NODE, proto_for_node(n));
    o->node = n;
    n->js = o;
    return o;
}

static Value wrap_v(Node *n) { return n ? v_obj(wrap(n)) : v_null(); }

static Node *node_of(Value v) { return v.t == V_OBJ && v.u.o->cls == CL_NODE ? v.u.o->node : NULL; }

/* Text from a script to text in the page: UTF-8 becomes what the font can show. */
static char *page_text(const char *s, int len, int *out_len) {
    char *out = malloc((size_t)len * 1 + 8);
    if (!out) {
        *out_len = 0;
        return NULL;
    }
    int k = 0;
    for (int i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out[k++] = (char)c;
            i++;
            continue;
        }
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        unsigned code = c & (0x3F >> n);
        int j = 1;
        for (; j <= n && i + j < len; j++) code = code << 6 | ((unsigned char)s[i + j] & 0x3F);
        i += j;
        char buf[8];
        int m = utf8_for(n ? code : 0xFFFD, buf); /* (kept as UTF-8) */
        for (int q = 0; q < m && k < len + 7; q++) out[k++] = buf[q];
    }
    out[k] = 0;
    *out_len = k;
    return out;
}

static Node *text_node(const char *s, int len) {
    int n;
    char *t = page_text(s, len, &n);
    Node *node = new_node(NODE_TEXT, NULL);
    if (!node) {
        free(t);
        return NULL;
    }
    node->text = t ? t : copy_of("", 0);
    node->text_len = t ? n : 0;
    return node;
}

static int subtree_has_wrapper(Node *n) {
    for (Node *d = n; d; d = next_in_tree(d, n)) {
        if (d->js) return 1;
    }
    return 0;
}

static unsigned dom_epoch; /* changes whenever nodes may have been freed */

/* Take a node out of the page for good. */
static void drop_node(Node *n) {
    dom_epoch++;
    remove_child(n);
    for (Node *d = n; d; d = next_in_tree(d, n)) {
        if (d == focused_node) focused_node = NULL;
        if (d == current_script) current_script = NULL;
    }
    if (subtree_has_wrapper(n)) orphan_add(n);
    else free_tree(n);
    dom_dirty = 1;
}

static void remove_all_children(Node *n) {
    while (n->first) drop_node(n->first);
}

static void text_of(Node *n, Buf *b, int inner_text) {
    if (n->type == NODE_TEXT) {
        buf_add(b, n->text, n->text_len);
        return;
    }
    if (inner_text && (is(n->tag, "script") || is(n->tag, "style") || is(n->tag, "noscript") || n->style.display == D_NONE)) return;
    if (inner_text && is(n->tag, "br")) buf_ch(b, '\n');
    for (Node *c = n->first; c; c = c->next) text_of(c, b, inner_text);
    if (inner_text && n->style.display != D_INLINE && n->style.display != D_NONE && b->len && b->p[b->len - 1] != '\n' && n->next)
        buf_ch(b, '\n');
}

static int is_void_tag(const char *t) {
    static const char *voids[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr", NULL};
    return in_list(t, voids);
}

static void serialize(Node *n, Buf *b, int raw) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == NODE_TEXT) {
            if (raw) {
                buf_add(b, c->text, c->text_len);
                continue;
            }
            for (int i = 0; i < c->text_len; i++) {
                char ch = c->text[i];
                if (ch == '<') buf_c(b, "&lt;");
                else if (ch == '>') buf_c(b, "&gt;");
                else if (ch == '&') buf_c(b, "&amp;");
                else buf_ch(b, ch);
            }
            continue;
        }
        buf_ch(b, '<');
        buf_c(b, c->tag);
        if (c->attrs && c->attrs[0]) {
            buf_ch(b, ' ');
            buf_c(b, c->attrs);
        }
        buf_ch(b, '>');
        if (is_void_tag(c->tag)) continue;
        serialize(c, b, is(c->tag, "script") || is(c->tag, "style"));
        buf_c(b, "</");
        buf_c(b, c->tag);
        buf_ch(b, '>');
    }
}

/* Parse an HTML fragment into the children of a new container node. */
static Node *parse_fragment(const char *html, int len) {
    Node *saved_doc = document, *saved_html = html_element, *saved_head = head_element, *saved_body = body_element;
    int plen;
    char *text = page_text(html, len, &plen);
    parse_html(text ? text : "", text ? plen : 0);
    free(text);
    Node *frag = new_node(NODE_ELEMENT, "#fragment");
    Node *temp_doc = document;
    /* <style>, <script>, <link>, <meta> may land in the head */
    for (Node *c = head_element->first; c;) {
        Node *next = c->next;
        if (!is(c->tag, "title")) {
            remove_child(c);
            append_child(frag, c);
        }
        c = next;
    }
    for (Node *c = body_element->first; c;) {
        Node *next = c->next;
        remove_child(c);
        append_child(frag, c);
        c = next;
    }
    document = saved_doc;
    html_element = saved_html;
    head_element = saved_head;
    body_element = saved_body;
    free_tree(temp_doc);
    return frag;
}

static void run_inserted_scripts(Node *root);

/* Insert the children of a fragment before `before` (NULL: at the end). */
static void insert_fragment(Node *parent, Node *frag, Node *before) {
    while (frag->first) {
        Node *c = frag->first;
        remove_child(c);
        insert_before(parent, c, before);
    }
    free_tree(frag);
    dom_dirty = 1;
}

static void set_text_children(Node *n, const char *s, int len) {
    remove_all_children(n);
    if (len) {
        Node *t = text_node(s, len);
        if (t) append_child(n, t);
    }
    dom_dirty = 1;
}

static void remove_attribute(Node *n, const char *name) {
    if (!n->attrs) return;
    char *a = n->attrs;
    size_t nl = strlen(name), k = 0, len = strlen(a);
    char *fresh = malloc(len + 1);
    if (!fresh) return;
    for (size_t i = 0; i < len;) {
        if ((i == 0 || is_space(a[i - 1])) && strncasecmp(a + i, name, nl) == 0 && (a[i + nl] == '=' || is_space(a[i + nl]) || !a[i + nl])) {
            size_t j = i + nl;
            while (is_space(a[j])) j++;
            if (a[j] == '=') {
                j++;
                while (is_space(a[j])) j++;
                char q = (a[j] == '"' || a[j] == '\'') ? a[j++] : 0;
                while (a[j] && (q ? a[j] != q : !is_space(a[j]))) j++;
                if (q && a[j]) j++;
            }
            i = j;
            continue;
        }
        fresh[k++] = a[i++];
    }
    fresh[k] = 0;
    free(n->attrs);
    n->attrs = fresh;
    dom_dirty = 1;
}

static void set_attribute(Node *n, const char *name, const char *value) {
    int len;
    char *v = page_text(value, (int)strlen(value), &len);
    set_attr(n, name, v ? v : "");
    free(v);
    if (is(name, "src") && is(n->tag, "img")) {
        if (n->img) {
            scaled_forget((image *)n->img);
            image_free((image *)n->img);
            free(n->img);
            n->img = NULL;
        }
        n->img_state = 0;
    }
    dom_dirty = 1;
}

static Value attr_value(Node *n, const char *name) {
    static char buf[4096];
    if (!attr(n, name, buf, sizeof(buf))) return v_null();
    return v_str(str_c(buf));
}

static Value attr_or_empty(Node *n, const char *name) {
    Value v = attr_value(n, name);
    return v.t == V_NULL ? v_str(S_empty) : v;
}

/* ---------------------------------------------------------------- selectors */

/* Match a selector list against elements under root (not root itself). */
static int select_nodes(Node *root, JsStr *selector, int all, JsObj *out, Node **first) {
    /* Keys for matching (classes may have changed since the last layout). */
    for (Node *d = document; d; d = next_in_tree(d, document)) {
        if (d->type == NODE_ELEMENT) node_keys(d);
    }
    if (root != document && !root->parent) {
        for (Node *d = root; d; d = next_in_tree(d, root)) {
            if (d->type == NODE_ELEMENT) node_keys(d);
        }
    }
    Rule list[16];
    int count = 0, start = 0, depth = 0;
    const char *s = selector->s;
    for (int i = 0; i <= selector->len; i++) {
        char c = i < selector->len ? s[i] : ',';
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            if (count < 16 && build_rule(s + start, i - start, &list[count])) count++;
            start = i + 1;
        }
    }
    if (first) *first = NULL;
    if (!count) {
        int blank = 1;
        for (int i = 0; i < selector->len && blank; i++) blank = is_space(s[i]);
        if (blank || strchr(selector->s, ':') == NULL) return throw_error(SyntaxErrorProto, "is not a valid selector");
        return C_NORMAL; /* pseudo-classes we cannot match (:hover...): nothing */
    }
    for (Node *d = root->first ? root->first : NULL; d; d = next_in_tree(d, root)) {
        if (d->type != NODE_ELEMENT) continue;
        for (int r = 0; r < count; r++) {
            if (matches_from(&list[r], list[r].part_count - 1, d)) {
                if (!all) {
                    if (first) *first = d;
                    return C_NORMAL;
                }
                arr_push(out, wrap_v(d));
                break;
            }
        }
    }
    return C_NORMAL;
}

static int node_matches(Node *n, JsStr *selector, int *result) {
    for (Node *d = document; d; d = next_in_tree(d, document)) {
        if (d->type == NODE_ELEMENT) node_keys(d);
    }
    node_keys(n);
    *result = 0;
    int start = 0, depth = 0;
    for (int i = 0; i <= selector->len; i++) {
        char c = i < selector->len ? selector->s[i] : ',';
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            Rule r;
            if (build_rule(selector->s + start, i - start, &r) && matches_from(&r, r.part_count - 1, n)) {
                *result = 1;
                return C_NORMAL;
            }
            start = i + 1;
        }
    }
    return C_NORMAL;
}

/* ---------------------------------------------------------------- the DOM's methods */

static int this_node(Value v, Node **out) {
    Node *n = node_of(v);
    if (!n) return throw_type("Illegal invocation (not a node)", NULL, NULL);
    *out = n;
    return C_NORMAL;
}

static int arg_string(Value *args, int argc, int i, JsStr **out) { return to_string(arg(args, argc, i), out); }

NATIVE(dom_get_element_by_id) {
    UNUSED;
    JsStr *id;
    TRY(arg_string(args, argc, 0, &id));
    char buf[256];
    for (Node *d = document; d; d = next_in_tree(d, document)) {
        if (d->type == NODE_ELEMENT && attr(d, "id", buf, sizeof(buf)) && (int)strlen(buf) == id->len && memcmp(buf, id->s, (size_t)id->len) == 0) {
            *ret = wrap_v(d);
            return C_NORMAL;
        }
    }
    *ret = v_null();
    return C_NORMAL;
}

/* magic: 0 querySelector, 1 querySelectorAll */
NATIVE(dom_query) {
    UNUSED;
    Node *root;
    TRY(this_node(this_v, &root));
    JsStr *sel;
    TRY(arg_string(args, argc, 0, &sel));
    if ((self->magic & 0xFFFF) == 1) {
        JsObj *out = new_array(0);
        TRY(select_nodes(root, sel, 1, out, NULL));
        *ret = v_obj(out);
    } else {
        Node *found;
        TRY(select_nodes(root, sel, 0, NULL, &found));
        *ret = wrap_v(found);
    }
    return C_NORMAL;
}

/* magic: 0 getElementsByTagName, 1 getElementsByClassName, 2 getElementsByName */
NATIVE(dom_get_elements) {
    UNUSED;
    Node *root;
    TRY(this_node(this_v, &root));
    JsStr *name;
    TRY(arg_string(args, argc, 0, &name));
    JsObj *out = new_array(0);
    int mode = self->magic & 0xFFFF;
    char want[64];
    int wl = name->len < 63 ? name->len : 63;
    for (int i = 0; i < wl; i++) want[i] = mode == 0 ? (char)tolower((unsigned char)name->s[i]) : name->s[i];
    want[wl] = 0;
    char buf[512];
    /* class names: all must be present */
    char classes[8][64];
    int nclasses = 0;
    if (mode == 1) {
        for (const char *p = want; *p && nclasses < 8;) {
            while (is_space(*p)) p++;
            int k = 0;
            while (*p && !is_space(*p) && k < 63) classes[nclasses][k++] = *p++;
            classes[nclasses][k] = 0;
            if (k) nclasses++;
        }
    }
    for (Node *d = root->first ? root->first : NULL; d; d = next_in_tree(d, root)) {
        if (d->type != NODE_ELEMENT) continue;
        int match = 0;
        if (mode == 0) match = (want[0] == '*' && !want[1]) || is(d->tag, want);
        else if (mode == 2) match = attr(d, "name", buf, sizeof(buf)) && !strcmp(buf, want);
        else if (attr(d, "class", buf, sizeof(buf))) {
            match = nclasses > 0;
            for (int c = 0; c < nclasses && match; c++) {
                int found = 0;
                for (const char *p = buf; *p && !found;) {
                    while (is_space(*p)) p++;
                    const char *st = p;
                    while (*p && !is_space(*p)) p++;
                    found = (size_t)(p - st) == strlen(classes[c]) && memcmp(st, classes[c], (size_t)(p - st)) == 0;
                }
                match = found;
            }
        }
        if (match) arr_push(out, wrap_v(d));
    }
    *ret = v_obj(out);
    return C_NORMAL;
}

NATIVE(dom_create_element) {
    UNUSED;
    JsStr *tag;
    TRY(arg_string(args, argc, 0, &tag));
    int text = (self->magic & 0xFFFF) == 1, frag = (self->magic & 0xFFFF) == 2, comment = (self->magic & 0xFFFF) == 3;
    if ((self->magic & 0xFFFF) == 4) { /* createElementNS(namespace, "prefix:name") */
        TRY(arg_string(args, argc, 1, &tag));
        const char *colon = strchr(tag->s, ':');
        if (colon) tag = str_c(colon + 1);
    }
    Node *n;
    if (text || comment) {
        n = text_node(comment ? "" : tag->s, comment ? 0 : tag->len);
    } else {
        char name[16];
        int k = 0;
        for (; k < tag->len && k < 15; k++) name[k] = (char)tolower((unsigned char)tag->s[k]);
        name[k] = 0;
        n = new_node(NODE_ELEMENT, frag ? "#fragment" : name);
    }
    if (!n) return throw_error(RangeErrorProto, "out of memory");
    orphan_add(n);
    *ret = wrap_v(n);
    return C_NORMAL;
}

static int contains_node(Node *outer, Node *inner) {
    for (Node *p = inner; p; p = p->parent) {
        if (p == outer) return 1;
    }
    return 0;
}

static int insert_node(Node *parent, Node *child, Node *before);

/* element.attachShadow({mode}): a shadow root, shown as the element's
   content (here: a box first inside it; its own children come after). */
NATIVE(dom_attach_shadow) {
    UNUSED;
    Node *host;
    TRY(this_node(this_v, &host));
    JsObj *h = wrap(host);
    if (own_prop(h, intern_c("__shadow"))) return throw_error(ErrorProto, "NotSupportedError: Failed to execute 'attachShadow' on 'Element': Shadow root cannot be created on a host which already hosts a shadow tree.");
    Value opts = ARG(0), mode = v_undef();
    if (opts.t == V_OBJ) TRY(get_value(opts, intern_c("mode"), &mode));
    Node *root = new_node(NODE_ELEMENT, "#shadow-root");
    if (!root) return throw_error(RangeErrorProto, "out of memory");
    set_attr(root, "style", "display:block");
    TRY(insert_node(host, root, host->first));
    JsObj *r = wrap(root);
    int open = mode.t == V_STR && !strcmp(mode.u.s->s, "open");
    prop_set_raw(r, intern_c("mode"), v_str(intern_c(open ? "open" : "closed")), P_HIDDEN);
    prop_set_raw(r, intern_c("host"), v_obj(h), P_HIDDEN);
    prop_set_raw(h, intern_c("__shadow"), v_obj(r), P_HIDDEN);
    prop_set_raw(h, intern_c("shadowRoot"), open ? v_obj(r) : v_null(), P_HIDDEN);
    dom_dirty = 1;
    *ret = v_obj(r);
    return C_NORMAL;
}

/* Insert `child` (a node or fragment) into parent before `before`. */
static int insert_node(Node *parent, Node *child, Node *before) {
    if (contains_node(child, parent)) return throw_error(ErrorProto, "HierarchyRequestError: the new child contains the parent");
    if (before && before->parent != parent) return throw_error(ErrorProto, "NotFoundError: the node before which to insert is not a child");
    if (child == before) return C_NORMAL;
    if (child->parent) remove_child(child);
    if (is(child->tag, "#fragment")) {
        while (child->first) {
            Node *c = child->first;
            remove_child(c);
            insert_before(parent, c, before);
            run_inserted_scripts(c);
        }
    } else {
        insert_before(parent, child, before);
        run_inserted_scripts(child);
    }
    dom_dirty = 1;
    return C_NORMAL;
}

/* magic: 0 appendChild, 1 insertBefore, 2 removeChild, 3 replaceChild */
NATIVE(dom_child_op) {
    UNUSED;
    Node *parent;
    TRY(this_node(this_v, &parent));
    Node *a = node_of(ARG(0)), *b = node_of(ARG(1));
    int mode = self->magic & 0xFFFF;
    if (!a) return throw_type("parameter 1 is not of type 'Node'", NULL, NULL);
    switch (mode) {
    case 0: TRY(insert_node(parent, a, NULL)); break;
    case 1: TRY(insert_node(parent, a, ARG(1).t == V_NULL || ARG(1).t == V_UNDEF ? NULL : b)); break;
    case 2:
        if (a->parent != parent) return throw_error(ErrorProto, "NotFoundError: the node to be removed is not a child of this node");
        remove_child(a);
        orphan_add(a);
        dom_dirty = 1;
        break;
    case 3:
        if (!b || b->parent != parent) return throw_error(ErrorProto, "NotFoundError: the node to be replaced is not a child");
        TRY(insert_node(parent, a, b));
        remove_child(b);
        orphan_add(b);
        *ret = wrap_v(b);
        return C_NORMAL;
    }
    *ret = ARG(0);
    return C_NORMAL;
}

/* append / prepend / before / after / replaceWith (magic 0..4): nodes or strings */
NATIVE(dom_insert_many) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    int mode = self->magic & 0xFFFF;
    if (mode == 5) { /* replaceChildren */
        remove_all_children(n);
        mode = 0;
    }
    Node *parent = mode <= 1 ? n : n->parent;
    if (!parent) {
        *ret = v_undef();
        return C_NORMAL;
    }
    Node *before = mode == 0 ? NULL : mode == 1 ? n->first : mode == 2 ? n : n->next;
    for (int i = 0; i < argc; i++) {
        Node *c = node_of(args[i]);
        if (!c) {
            JsStr *s;
            TRY(to_string(args[i], &s));
            c = text_node(s->s, s->len);
            if (!c) continue;
        }
        if (c == before) before = before->next;
        TRY(insert_node(parent, c, before));
    }
    if (mode == 4) {
        remove_child(n);
        orphan_add(n);
    }
    dom_dirty = 1;
    *ret = v_undef();
    return C_NORMAL;
}

NATIVE(dom_remove) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    if (n->parent) {
        remove_child(n);
        orphan_add(n);
        dom_dirty = 1;
    }
    *ret = v_undef();
    return C_NORMAL;
}

static Node *clone_node(Node *n, int deep) {
    Node *c = new_node(n->type, n->type == NODE_ELEMENT ? n->tag : NULL);
    if (!c) return NULL;
    if (n->attrs) c->attrs = copy_of(n->attrs, (int)strlen(n->attrs));
    if (n->text) {
        c->text = copy_of(n->text, n->text_len);
        c->text_len = n->text_len;
    }
    if (n->value) c->value = copy_of(n->value, (int)strlen(n->value));
    c->checked = n->checked;
    c->selected_index = n->selected_index;
    if (deep) {
        for (Node *k = n->first; k; k = k->next) {
            Node *kc = clone_node(k, 1);
            if (kc) append_child(c, kc);
        }
    }
    return c;
}

NATIVE(dom_clone) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    Node *c = clone_node(n, to_boolean(ARG(0)));
    if (!c) return throw_error(RangeErrorProto, "out of memory");
    orphan_add(c);
    *ret = wrap_v(c);
    return C_NORMAL;
}

NATIVE(dom_contains) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    Node *other = node_of(ARG(0));
    *ret = v_bool(other && contains_node(n, other));
    return C_NORMAL;
}

/* a.compareDocumentPosition(b): where b is relative to a, as bits:
   1 disconnected, 2 preceding, 4 following, 8 contains a, 16 inside a. */
NATIVE(dom_compare_position) {
    UNUSED;
    Node *a;
    TRY(this_node(this_v, &a));
    Node *b = node_of(ARG(0));
    if (!b) return throw_type("compareDocumentPosition: not a node", NULL, NULL);
    if (a == b) {
        *ret = v_num(0);
        return C_NORMAL;
    }
    Node *ra = a, *rb = b;
    while (ra->parent) ra = ra->parent;
    while (rb->parent) rb = rb->parent;
    if (ra != rb) { /* in different trees: some fixed order */
        *ret = v_num(1 | 32 | (a < b ? 4 : 2));
        return C_NORMAL;
    }
    if (contains_node(b, a)) {
        *ret = v_num(8 | 2);
        return C_NORMAL;
    }
    if (contains_node(a, b)) {
        *ret = v_num(16 | 4);
        return C_NORMAL;
    }
    /* otherwise by document order: which comes first walking the tree */
    for (Node *n = ra; n; n = next_in_tree(n, ra)) {
        if (n == a) {
            *ret = v_num(4);
            return C_NORMAL;
        }
        if (n == b) {
            *ret = v_num(2);
            return C_NORMAL;
        }
    }
    *ret = v_num(1 | 32);
    return C_NORMAL;
}

/* magic: 0 matches, 1 closest */
NATIVE(dom_matches) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    JsStr *sel;
    TRY(arg_string(args, argc, 0, &sel));
    if ((self->magic & 0xFFFF) == 0) {
        int r;
        TRY(node_matches(n, sel, &r));
        *ret = v_bool(r);
        return C_NORMAL;
    }
    for (Node *p = n; p && p != document; p = p->parent) {
        if (p->type != NODE_ELEMENT) continue;
        int r;
        TRY(node_matches(p, sel, &r));
        if (r) {
            *ret = wrap_v(p);
            return C_NORMAL;
        }
    }
    *ret = v_null();
    return C_NORMAL;
}

/* magic: 0 getAttribute, 1 setAttribute, 2 removeAttribute, 3 hasAttribute, 4 toggleAttribute, 5 getAttributeNames */
NATIVE(dom_attribute) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    int mode = self->magic & 0xFFFF;
    if (mode == 5) {
        JsObj *out = new_array(0);
        const char *a = n->attrs ? n->attrs : "";
        while (*a) {
            while (is_space(*a)) a++;
            const char *st = a;
            while (*a && !is_space(*a) && *a != '=') a++;
            if (a > st) arr_push(out, v_str(str_new(st, (int)(a - st))));
            if (*a == '=') {
                a++;
                char q = (*a == '"' || *a == '\'') ? *a++ : 0;
                while (*a && (q ? *a != q : !is_space(*a))) a++;
                if (q && *a) a++;
            }
        }
        *ret = v_obj(out);
        return C_NORMAL;
    }
    JsStr *name;
    TRY(arg_string(args, argc, 0, &name));
    char lname[64];
    int k = 0;
    for (; k < name->len && k < 63; k++) lname[k] = (char)tolower((unsigned char)name->s[k]);
    lname[k] = 0;
    switch (mode) {
    case 0: *ret = attr_value(n, lname); return C_NORMAL;
    case 1: {
        JsStr *v;
        TRY(arg_string(args, argc, 1, &v));
        set_attribute(n, lname, v->s);
        if (is(lname, "value") && (is(n->tag, "input"))) {
            free(n->value);
            n->value = copy_of(v->s, v->len);
        }
        if (is(lname, "checked")) n->checked = 1;
        *ret = v_undef();
        return C_NORMAL;
    }
    case 2:
        remove_attribute(n, lname);
        if (is(lname, "checked")) n->checked = 0;
        *ret = v_undef();
        return C_NORMAL;
    case 3: *ret = v_bool(has_attr(n, lname)); return C_NORMAL;
    case 4: {
        int on = argc > 1 ? to_boolean(args[1]) : !has_attr(n, lname);
        if (on) set_attribute(n, lname, "");
        else remove_attribute(n, lname);
        *ret = v_bool(on);
        return C_NORMAL;
    }
    }
    return C_NORMAL;
}

NATIVE(dom_insert_adjacent) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    JsStr *where, *html;
    TRY(arg_string(args, argc, 0, &where));
    int mode = self->magic & 0xFFFF; /* 0 HTML, 1 element, 2 text */
    Node *parent, *before;
    if (!strncasecmp(where->s, "beforebegin", 11)) parent = n->parent, before = n;
    else if (!strncasecmp(where->s, "afterbegin", 10)) parent = n, before = n->first;
    else if (!strncasecmp(where->s, "beforeend", 9)) parent = n, before = NULL;
    else parent = n->parent, before = n->next;
    if (!parent) {
        *ret = v_undef();
        return C_NORMAL;
    }
    if (mode == 1) {
        Node *c = node_of(ARG(1));
        if (c) TRY(insert_node(parent, c, before));
        *ret = ARG(1);
        return C_NORMAL;
    }
    TRY(arg_string(args, argc, 1, &html));
    if (mode == 2) {
        Node *t = text_node(html->s, html->len);
        if (t) insert_before(parent, t, before);
    } else {
        insert_fragment(parent, parse_fragment(html->s, html->len), before);
    }
    dom_dirty = 1;
    *ret = v_undef();
    return C_NORMAL;
}

static void make_rect(JsObj *r, double x, double y, double w, double h) {
    prop_set_raw(r, intern_c("x"), v_num(x), 0);
    prop_set_raw(r, intern_c("y"), v_num(y), 0);
    prop_set_raw(r, intern_c("left"), v_num(x), 0);
    prop_set_raw(r, intern_c("top"), v_num(y), 0);
    prop_set_raw(r, intern_c("width"), v_num(w), 0);
    prop_set_raw(r, intern_c("height"), v_num(h), 0);
    prop_set_raw(r, intern_c("right"), v_num(x + w), 0);
    prop_set_raw(r, intern_c("bottom"), v_num(y + h), 0);
}

static void coro_on_main_stack(void (*fn)(void *), void *arg); /* (js_coro.c) */
static void relayout_thunk(void *unused) {
    (void)unused;
    relayout();
}

static void ensure_layout(void) {
    if (dom_dirty) {
        coro_on_main_stack(relayout_thunk, NULL); /* (layout recurses deeply: never on a coroutine's stack) */
        dom_dirty = 0;
    }
}

NATIVE(dom_rect) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    ensure_layout();
    JsObj *r = new_object();
    make_rect(r, n->x, n->y - page_scroll_y, n->w, n->h);
    if ((self->magic & 0xFFFF) == 1) { /* getClientRects */
        JsObj *list = new_array(1);
        arr_push(list, v_obj(r));
        *ret = v_obj(list);
        return C_NORMAL;
    }
    *ret = v_obj(r);
    return C_NORMAL;
}

NATIVE(dom_focus) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    int mode = self->magic & 0xFFFF; /* 0 focus, 1 blur, 2 scrollIntoView, 3 select */
    if (mode == 0) focused_node = n;
    else if (mode == 1 && focused_node == n) focused_node = NULL;
    else if (mode == 2) {
        ensure_layout();
        js_scroll_to = n->y > 20 ? n->y - 20 : 0;
    }
    dom_dirty = 1;
    *ret = v_undef();
    return C_NORMAL;
}

static int dispatch_event(JsObj *target, JsObj *event);
static JsObj *new_event(const char *type, int bubbles, int cancelable);

NATIVE(dom_click) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    JsObj *e = new_event("click", 1, 1);
    int prevented = dispatch_event(wrap(n), e);
    if (prevented < 0) return C_THROW;
    /* the default action of a checkbox */
    char type[16] = "";
    attr(n, "type", type, sizeof(type));
    if (!prevented && is(n->tag, "input") && !strcasecmp_word(type, "checkbox")) {
        n->checked = !n->checked;
        dom_dirty = 1;
    }
    *ret = v_undef();
    return C_NORMAL;
}

NATIVE(dom_has_child_nodes) {
    UNUSED;
    Node *n;
    TRY(this_node(this_v, &n));
    *ret = v_bool(n->first != NULL);
    return C_NORMAL;
}

NATIVE(document_write) {
    UNUSED;
    Buf b = {0};
    for (int i = 0; i < argc; i++) {
        JsStr *s;
        TRY(to_string(args[i], &s));
        buf_add(&b, s->s, s->len);
    }
    if ((self->magic & 0xFFFF) == 1) buf_ch(&b, '\n');
    if (b.len) {
        Node *frag = parse_fragment(b.p, b.len);
        if (current_script && current_script->parent) insert_fragment(current_script->parent, frag, current_script->next);
        else insert_fragment(body_element, frag, NULL);
    }
    free(b.p);
    *ret = v_undef();
    return C_NORMAL;
}

/* ---------------------------------------------------------------- style, classList, dataset */

static JsObj *helper_object(JsObj *proto, int cls, Node *n) {
    JsObj *o = obj_new_raw(cls, proto);
    o->node = n;
    return o;
}

/* fontSize -> font-size; cssFloat -> float */
static void css_name(JsStr *key, char *out, int max) {
    int k = 0;
    if (key->len == 8 && !memcmp(key->s, "cssFloat", 8)) {
        strcpy(out, "float");
        return;
    }
    for (int i = 0; i < key->len && k < max - 2; i++) {
        char c = key->s[i];
        if (c >= 'A' && c <= 'Z') {
            out[k++] = '-';
            out[k++] = (char)(c + 32);
        } else {
            out[k++] = c;
        }
    }
    out[k] = 0;
    if (!strncmp(out, "webkit-", 7) || !strncmp(out, "moz-", 4) || !strncmp(out, "ms-", 3)) {
        memmove(out + 1, out, strlen(out) + 1);
        out[0] = '-';
    }
}

/* The value of `prop` in a style attribute's text (into out). */
static int style_lookup(const char *style, const char *prop, char *out, int max) {
    size_t pl = strlen(prop);
    out[0] = 0;
    int found = 0;
    for (const char *p = style; p && *p;) {
        while (is_space(*p) || *p == ';') p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        const char *name_end = p;
        while (name_end > name && is_space(name_end[-1])) name_end--;
        if (*p != ':') continue;
        p++;
        while (is_space(*p)) p++;
        const char *val = p;
        int depth = 0;
        while (*p && (*p != ';' || depth)) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
            p++;
        }
        const char *val_end = p;
        while (val_end > val && is_space(val_end[-1])) val_end--;
        if ((size_t)(name_end - name) == pl && strncasecmp(name, prop, pl) == 0) {
            int n = (int)(val_end - val) < max - 1 ? (int)(val_end - val) : max - 1;
            memcpy(out, val, (size_t)n);
            out[n] = 0;
            found = 1;
        }
    }
    return found;
}

static void style_put(Node *n, const char *prop, const char *value) {
    char style[4096] = "";
    attr(n, "style", style, sizeof(style));
    Buf b = {0};
    size_t pl = strlen(prop);
    for (const char *p = style; *p;) {
        while (is_space(*p) || *p == ';') p++;
        if (!*p) break;
        const char *decl = p;
        int depth = 0;
        while (*p && (*p != ';' || depth)) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
            p++;
        }
        const char *name_end = decl;
        while (name_end < p && *name_end != ':') name_end++;
        const char *ne = name_end;
        while (ne > decl && is_space(ne[-1])) ne--;
        if ((size_t)(ne - decl) == pl && strncasecmp(decl, prop, pl) == 0) continue;
        buf_add(&b, decl, (int)(p - decl));
        buf_c(&b, "; ");
    }
    if (value && value[0]) {
        buf_c(&b, prop);
        buf_c(&b, ": ");
        buf_c(&b, value);
        buf_ch(&b, ';');
    }
    set_attribute(n, "style", b.p ? b.p : "");
    free(b.p);
    dom_dirty = 1;
}

static const char *display_name(int d) {
    switch (d) {
    case D_NONE: return "none";
    case D_BLOCK: return "block";
    case D_INLINE_BLOCK: return "inline-block";
    case D_LIST_ITEM: return "list-item";
    case D_FLEX: return "flex";
    case D_INLINE_FLEX: return "inline-flex";
    case D_GRID: return "grid";
    case D_INLINE_GRID: return "inline-grid";
    case D_TABLE: return "table";
    case D_TABLE_ROW: return "table-row";
    case D_TABLE_CELL: return "table-cell";
    }
    return "inline";
}

static void color_text(unsigned c, char *out) {
    char t[3][8];
    for (int i = 0; i < 3; i++) number_to_text((c >> (16 - 8 * i)) & 255, t[i]);
    unsigned alpha = COLOR_ALPHA(c);
    strcpy(out, alpha < 255 ? "rgba(" : "rgb(");
    strcat(out, t[0]);
    strcat(out, ", ");
    strcat(out, t[1]);
    strcat(out, ", ");
    strcat(out, t[2]);
    if (alpha < 255) { /* ", 0.5" */
        char a[16];
        int hundredths = (int)((alpha * 100 + 127) / 255);
        strcat(out, ", 0.");
        a[0] = (char)('0' + hundredths / 10);
        a[1] = (char)('0' + hundredths % 10);
        a[2] = 0;
        if (a[1] == '0') a[1] = 0;
        strcat(out, a);
    }
    strcat(out, ")");
}

/* A computed style property (magic 1 on the style object: getComputedStyle). */
static int computed_value(Node *n, const char *prop, char *out) {
    ensure_layout();
    Style *s = &n->style;
    char t[32];
    out[0] = 0;
    if (!strcmp(prop, "display")) strcpy(out, display_name(s->display));
    else if (!strcmp(prop, "visibility")) strcpy(out, s->hidden ? "hidden" : "visible");
    else if (!strcmp(prop, "position")) strcpy(out, s->position == P_ABSOLUTE ? "absolute" : s->position == P_FIXED ? "fixed" : s->position == P_RELATIVE ? "relative" : "static");
    else if (!strcmp(prop, "width") || !strcmp(prop, "height")) {
        number_to_text(prop[0] == 'w' ? n->w : n->h, t);
        strcpy(out, t);
        strcat(out, "px");
    } else if (!strcmp(prop, "color")) color_text(s->color, out);
    else if (!strcmp(prop, "background-color")) {
        if (s->has_background) color_text(s->background, out);
        else strcpy(out, "rgba(0, 0, 0, 0)");
    } else if (!strcmp(prop, "font-size")) {
        number_to_text(s->size, t);
        strcpy(out, t);
        strcat(out, "px");
    } else if (!strcmp(prop, "font-weight")) strcpy(out, s->bold ? "700" : "400");
    else if (!strcmp(prop, "opacity")) {
        if (s->opacity >= 255) strcpy(out, "1");
        else {
            int hundredths = (s->opacity * 100 + 127) / 255;
            strcpy(out, "0.");
            out[2] = (char)('0' + hundredths / 10);
            out[3] = (char)('0' + hundredths % 10);
            out[4] = 0;
            if (out[3] == '0') out[3] = 0;
            if (!hundredths) strcpy(out, "0");
        }
    }
    else if (!strcmp(prop, "text-align")) strcpy(out, s->align == A_CENTER ? "center" : s->align == A_RIGHT ? "right" : "left");
    else if (!strcmp(prop, "overflow")) strcpy(out, s->overflow_hidden ? "hidden" : "visible");
    else if (!strncmp(prop, "margin-", 7) || !strncmp(prop, "padding-", 8)) strcpy(out, "0px");
    else return 0;
    return 1;
}

static int style_get(JsObj *o, JsStr *key, Value *out, int *handled) {
    Node *n = o->node;
    if (!n) {
        *handled = 0;
        return C_NORMAL;
    }
    if (key == K_length) {
        *out = v_num(0);
        return C_NORMAL;
    }
    if (key == K_cssText) {
        *out = attr_or_empty(n, "style");
        return C_NORMAL;
    }
    /* methods live on the prototype */
    if (own_prop(StyleProto, key)) {
        *handled = 0;
        return C_NORMAL;
    }
    char prop[64], val[512];
    css_name(key, prop, sizeof(prop));
    if (o->magic == 1) {
        if (computed_value(n, prop, val)) {
            *out = v_str(str_c(val));
            return C_NORMAL;
        }
    }
    char style[4096] = "";
    attr(n, "style", style, sizeof(style));
    style_lookup(style, prop, val, sizeof(val));
    *out = v_str(str_c(val));
    return C_NORMAL;
}

static int style_set(JsObj *o, JsStr *key, Value v, int *handled) {
    Node *n = o->node;
    if (!n || o->magic == 1) {
        *handled = o->magic == 1; /* computed styles are read-only */
        return C_NORMAL;
    }
    JsStr *s = S_empty;
    if (!is_nullish(v)) TRY(to_string(v, &s));
    if (key == K_cssText) {
        set_attribute(n, "style", s->s);
        return C_NORMAL;
    }
    char prop[64];
    css_name(key, prop, sizeof(prop));
    style_put(n, prop, s->s);
    return C_NORMAL;
}

/* style.setProperty / getPropertyValue / removeProperty: magic 0, 1, 2 */
NATIVE(style_method) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_STYLE || !this_v.u.o->node) {
        *ret = v_undef();
        return C_NORMAL;
    }
    Node *n = this_v.u.o->node;
    JsStr *name;
    TRY(arg_string(args, argc, 0, &name));
    int mode = self->magic & 0xFFFF;
    if (mode == 0) {
        JsStr *v;
        TRY(arg_string(args, argc, 1, &v));
        if (this_v.u.o->magic != 1) style_put(n, name->s, v->s);
        *ret = v_undef();
    } else if (mode == 1) {
        char val[512], style[4096] = "";
        if (this_v.u.o->magic == 1 && computed_value(n, name->s, val)) {
            *ret = v_str(str_c(val));
            return C_NORMAL;
        }
        attr(n, "style", style, sizeof(style));
        style_lookup(style, name->s, val, sizeof(val));
        *ret = v_str(str_c(val));
    } else {
        style_put(n, name->s, NULL);
        *ret = v_str(S_empty);
    }
    return C_NORMAL;
}

/* The class list as words. */
static int class_words(Node *n, char words[][64], int max) {
    char buf[1024] = "";
    attr(n, "class", buf, sizeof(buf));
    int count = 0;
    for (const char *p = buf; *p && count < max;) {
        while (is_space(*p)) p++;
        int k = 0;
        while (*p && !is_space(*p)) {
            if (k < 63) words[count][k++] = *p;
            p++;
        }
        words[count][k] = 0;
        if (k) count++;
    }
    return count;
}

static void set_class_words(Node *n, char words[][64], int count) {
    Buf b = {0};
    for (int i = 0; i < count; i++) {
        if (i) buf_ch(&b, ' ');
        buf_c(&b, words[i]);
    }
    set_attribute(n, "class", b.p ? b.p : "");
    free(b.p);
    dom_dirty = 1;
}

/* magic: 0 add, 1 remove, 2 toggle, 3 contains, 4 replace, 5 item, 6 forEach */
NATIVE(classlist_method) {
    UNUSED;
    Node *n = this_v.t == V_OBJ && this_v.u.o->cls == CL_CLASSLIST ? this_v.u.o->node : NULL;
    if (!n) return throw_type("Illegal invocation", NULL, NULL);
    static char words[64][64];
    int count = class_words(n, words, 64), mode = self->magic & 0xFFFF;
    if (mode == 5) {
        double i;
        TRY(arg_int(args, argc, 0, 0, &i));
        *ret = i >= 0 && i < count ? v_str(str_c(words[(int)i])) : v_null();
        return C_NORMAL;
    }
    if (mode == 6) {
        Value fn = ARG(0);
        if (!is_callable(fn)) return throw_type("callback is not a function", NULL, NULL);
        for (int i = 0; i < count; i++) {
            Value cb[2] = {v_str(str_c(words[i])), v_num(i)}, r;
            TRY(call_function(fn.u.o, v_undef(), cb, 2, &r));
        }
        *ret = v_undef();
        return C_NORMAL;
    }
    int changed = 0, result = 0;
    for (int a = 0; a < (mode == 2 || mode == 3 || mode == 4 ? 1 : argc); a++) {
        JsStr *w;
        TRY(arg_string(args, argc, a, &w));
        int at = -1;
        for (int i = 0; i < count; i++) {
            if ((int)strlen(words[i]) == w->len && !memcmp(words[i], w->s, (size_t)w->len)) at = i;
        }
        int want = mode == 0 ? 1 : mode == 1 ? 0 : mode == 2 ? (argc > 1 ? to_boolean(args[1]) : at < 0) : -1;
        if (mode == 3) {
            *ret = v_bool(at >= 0);
            return C_NORMAL;
        }
        if (mode == 4) {
            JsStr *with;
            TRY(arg_string(args, argc, 1, &with));
            if (at >= 0 && with->len < 64) {
                memcpy(words[at], with->s, (size_t)with->len);
                words[at][with->len] = 0;
                changed = result = 1;
            }
            break;
        }
        if (want == 1 && at < 0 && count < 64 && w->len < 64) {
            memcpy(words[count], w->s, (size_t)w->len);
            words[count++][w->len] = 0;
            changed = 1;
        } else if (want == 0 && at >= 0) {
            memmove(words[at], words[at + 1], sizeof(words[0]) * (size_t)(count - at - 1));
            count--;
            changed = 1;
        }
        result = want == 1;
    }
    if (changed) set_class_words(n, words, count);
    *ret = mode == 2 || mode == 4 ? v_bool(result) : v_undef();
    return C_NORMAL;
}

static int classlist_get(JsObj *o, JsStr *key, Value *out, int *handled) {
    Node *n = o->node;
    if (!n) {
        *handled = 0;
        return C_NORMAL;
    }
    char words[64][64];
    if (key == K_length) {
        *out = v_num(class_words(n, words, 64));
        return C_NORMAL;
    }
    if (key == K_value) {
        *out = attr_or_empty(n, "class");
        return C_NORMAL;
    }
    if (key->index >= 0) {
        int count = class_words(n, words, 64);
        *out = key->index < count ? v_str(str_c(words[key->index])) : v_undef();
        return C_NORMAL;
    }
    *handled = 0;
    return C_NORMAL;
}

/* data-foo-bar <-> fooBar */
static int dataset_get(JsObj *o, JsStr *key, Value *out, int *handled) {
    Node *n = o->node;
    if (!n || own_prop(ObjectProto, key)) {
        *handled = 0;
        return C_NORMAL;
    }
    char name[96] = "data-";
    int k = 5;
    for (int i = 0; i < key->len && k < 94; i++) {
        char c = key->s[i];
        if (c >= 'A' && c <= 'Z') {
            name[k++] = '-';
            name[k++] = (char)(c + 32);
        } else {
            name[k++] = c;
        }
    }
    name[k] = 0;
    Value v = attr_value(n, name);
    *out = v.t == V_NULL ? v_undef() : v;
    return C_NORMAL;
}

static int dataset_set(JsObj *o, JsStr *key, Value v, int *handled) {
    Node *n = o->node;
    if (!n) {
        *handled = 0;
        return C_NORMAL;
    }
    char name[96] = "data-";
    int k = 5;
    for (int i = 0; i < key->len && k < 94; i++) {
        char c = key->s[i];
        if (c >= 'A' && c <= 'Z') {
            name[k++] = '-';
            name[k++] = (char)(c + 32);
        } else {
            name[k++] = c;
        }
    }
    name[k] = 0;
    JsStr *s;
    TRY(to_string(v, &s));
    set_attribute(n, name, s->s);
    return C_NORMAL;
}

/* ---------------------------------------------------------------- storage */

/* localStorage / sessionStorage: kept for the browser's run (not on disk). */
NATIVE(storage_method) {
    UNUSED;
    JsObj *st = this_v.t == V_OBJ && this_v.u.o->cls == CL_STORAGE ? this_v.u.o : NULL;
    if (!st) return throw_type("Illegal invocation", NULL, NULL);
    JsObj *map = st->target;
    int mode = self->magic & 0xFFFF; /* 0 getItem, 1 setItem, 2 removeItem, 3 clear, 4 key */
    if (mode == 3) {
        map->extra_len = 0;
        *ret = v_undef();
        return C_NORMAL;
    }
    if (mode == 4) {
        double i;
        TRY(arg_int(args, argc, 0, 0, &i));
        Value *e = map->extra;
        int seen = 0;
        for (int k = 0; k < map->extra_len; k++) {
            if (e[2 * k].t == V_UNDEF && e[2 * k + 1].t == V_NULL) continue;
            if (seen++ == (int)i) {
                *ret = e[2 * k];
                return C_NORMAL;
            }
        }
        *ret = v_null();
        return C_NORMAL;
    }
    JsStr *key;
    TRY(arg_string(args, argc, 0, &key));
    Value kv = v_str(key);
    int at = map_find(map, kv);
    if (mode == 0) {
        *ret = at >= 0 ? ((Value *)map->extra)[2 * at + 1] : v_null();
    } else if (mode == 1) {
        JsStr *v;
        TRY(arg_string(args, argc, 1, &v));
        map_put(map, kv, v_str(v));
        *ret = v_undef();
    } else {
        if (at >= 0) {
            ((Value *)map->extra)[2 * at] = v_undef();
            ((Value *)map->extra)[2 * at + 1] = v_null();
        }
        *ret = v_undef();
    }
    return C_NORMAL;
}

static int storage_get(JsObj *o, JsStr *key, Value *out, int *handled) {
    if (key == K_length) {
        *out = v_num(map_size(o->target));
        return C_NORMAL;
    }
    if (own_prop(StorageProto, key) || own_prop(ObjectProto, key)) {
        *handled = 0;
        return C_NORMAL;
    }
    int at = map_find(o->target, v_str(key));
    *out = at >= 0 ? ((Value *)o->target->extra)[2 * at + 1] : v_undef();
    return C_NORMAL;
}

static JsObj *storage_new(void) {
    JsObj *st = obj_new_raw(CL_STORAGE, StorageProto);
    st->target = obj_new_raw(CL_MAP, MapProto);
    return st;
}

/* ---------------------------------------------------------------- node properties */

static int node_list(Node *n, int elements_only, Value *out) {
    JsObj *a = new_array(0);
    for (Node *c = n->first; c; c = c->next) {
        if (!elements_only || c->type == NODE_ELEMENT) arr_push(a, wrap_v(c));
    }
    *out = v_obj(a);
    return C_NORMAL;
}

static Node *element_sibling(Node *n, int forward) {
    for (Node *s = forward ? n->next : n->prev; s; s = forward ? s->next : s->prev) {
        if (s->type == NODE_ELEMENT) return s;
    }
    return NULL;
}

static Node *element_child(Node *n, int last) {
    for (Node *c = last ? n->last : n->first; c; c = last ? c->prev : c->next) {
        if (c->type == NODE_ELEMENT) return c;
    }
    return NULL;
}

static int collect_tag(const char *tag, Value *out) {
    JsObj *a = new_array(0);
    for (Node *d = document; d; d = next_in_tree(d, document)) {
        if (d->type == NODE_ELEMENT && is(d->tag, tag)) arr_push(a, wrap_v(d));
    }
    *out = v_obj(a);
    return C_NORMAL;
}

static JsObj *location_obj;

/* Attributes reflected as properties: property name -> attribute name. */
static const char *reflected(JsStr *key) {
    if (key == K_htmlFor) return "for";
    if (key == K_name) return "name";
    if (key == K_alt) return "alt";
    if (key == K_placeholder) return "placeholder";
    if (key == K_rel) return "rel";
    if (key == K_target) return "target";
    if (key == K_action) return "action";
    if (key == K_method) return "method";
    if (key == K_lang) return "lang";
    if (key == K_dir) return "dir";
    if (key == K_role) return "role";
    if (key == K_content) return "content";
    if (key == K_label) return "label";
    return NULL;
}

static Value resolved_attr(Node *n, const char *name) {
    char raw[2048];
    if (!attr(n, name, raw, sizeof(raw))) return v_str(S_empty);
    char full[2048];
    resolve_url(base_url, raw, full, sizeof(full));
    return v_str(str_c(full));
}

static int select_option(Node *select, int index, Node **out) {
    int i = 0;
    *out = NULL;
    for (Node *o = select; o; o = next_in_tree(o, select)) {
        if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
            if (i == index) {
                *out = o;
                return 1;
            }
            i++;
        }
    }
    return 0;
}

static int make_attributes(JsObj *o, Value *out);
static void set_url_parts(JsObj *o, const char *url);

static int is_url_part(JsStr *key) {
    return key == K_protocol || key == K_host || key == K_hostname || key == K_port || key == K_pathname || key == K_search ||
           key == K_hash || key == K_origin;
}

static int iframe_content(Node *n, int document_too, Value *out); /* (js_frames.c) */
static void frames_js_init(JsObj *w);
static void frames_js_mark(void);

static int node_get(JsObj *o, Node *n, JsStr *key, Value *out, int *handled) {
    if ((key == K_contentWindow || key == K_contentDocument) && n->type == NODE_ELEMENT && is(n->tag, "iframe"))
        return iframe_content(n, key == K_contentDocument, out);
    if (is_url_part(key) && n->type == NODE_ELEMENT && (is(n->tag, "a") || is(n->tag, "area"))) {
        /* a link's address in parts, as location has (a common way to take a URL apart) */
        Value href = resolved_attr(n, "href");
        JsStr *s = href.t == V_STR ? href.u.s : S_empty;
        JsObj *parts = new_object();
        set_url_parts(parts, s->s);
        Prop *p = own_prop(parts, key);
        *out = p ? p->v : v_str(S_empty);
        if (!s->len && key != K_pathname) *out = v_str(S_empty);
        return C_NORMAL;
    }
    int el = n->type == NODE_ELEMENT;
    int doc = n == document;
    if (key->len == 0 || key->s[0] == '_') {
        *handled = 0;
        return C_NORMAL;
    }
    if (key == K_nodeType) *out = v_num(doc ? 9 : el ? (is(n->tag, "#fragment") ? 11 : 1) : 3);
    else if (key == K_nodeName || key == K_tagName || key == K_localName) {
        if (!el) *out = key == K_nodeName ? v_str(intern_c("#text")) : v_undef();
        else if (doc) *out = v_str(intern_c("#document"));
        else {
            char up[16];
            int k = 0;
            for (; n->tag[k]; k++) up[k] = key == K_localName ? n->tag[k] : (char)toupper((unsigned char)n->tag[k]);
            up[k] = 0;
            *out = v_str(intern_c(up));
        }
    } else if (key == K_parentNode) *out = wrap_v(n->parent);
    else if (key == K_parentElement) *out = n->parent && n->parent != document ? wrap_v(n->parent) : v_null();
    else if (key == K_childNodes) return node_list(n, 0, out);
    else if (key == K_children) return node_list(n, 1, out);
    else if (key == K_firstChild) *out = wrap_v(n->first);
    else if (key == K_lastChild) *out = wrap_v(n->last);
    else if (key == K_firstElementChild) *out = wrap_v(element_child(n, 0));
    else if (key == K_lastElementChild) *out = wrap_v(element_child(n, 1));
    else if (key == K_nextSibling) *out = wrap_v(n->next);
    else if (key == K_previousSibling) *out = wrap_v(n->prev);
    else if (key == K_nextElementSibling) *out = wrap_v(element_sibling(n, 1));
    else if (key == K_previousElementSibling) *out = wrap_v(element_sibling(n, 0));
    else if (key == K_childElementCount) {
        int c = 0;
        for (Node *k = n->first; k; k = k->next) c += k->type == NODE_ELEMENT;
        *out = v_num(c);
    } else if (key == K_ownerDocument) *out = doc ? v_null() : wrap_v(document);
    else if (key == K_isConnected) *out = v_bool(contains_node(document, n));
    else if (key == K_textContent || key == K_innerText || key == K_nodeValue || key == K_data || key == K_text) {
        if (doc && key != K_text) {
            *out = v_null();
            return C_NORMAL;
        }
        if (el && (key == K_nodeValue || key == K_data)) {
            *out = v_null();
            return C_NORMAL;
        }
        if (key == K_text && el && !is(n->tag, "script") && !is(n->tag, "option") && !is(n->tag, "a") && !is(n->tag, "title")) {
            *handled = 0;
            return C_NORMAL;
        }
        if (key == K_innerText) ensure_layout();
        Buf b = {0};
        text_of(n, &b, key == K_innerText);
        *out = v_str(buf_done(&b));
    } else if (!el) {
        if (key == K_length) *out = v_num(n->text_len);
        else *handled = 0;
    } else if (doc) {
        if (key == K_body) *out = wrap_v(body_element);
        else if (key == K_head) *out = wrap_v(head_element);
        else if (key == K_documentElement) *out = wrap_v(html_element);
        else if (key == K_readyState) *out = v_str(intern_c(page_loaded ? "complete" : current_script ? "loading" : "interactive"));
        else if (key == K_location) *out = location_obj ? v_obj(location_obj) : v_null();
        else if (key == K_cookie) {
            static char jar[8192];
            cookie_header(current_url, CTX_SAME_SITE, 1, jar, sizeof(jar));
            *out = v_str(str_c(jar));
        }
        else if (key == K_URL || key == K_documentURI || key == K_baseURI) *out = v_str(str_c(current_url));
        else if (key == K_domain) {
            Value loc;
            TRY(get_value(v_obj(location_obj), intern_c("hostname"), &loc));
            *out = loc;
        } else if (key == K_referrer) *out = v_str(S_empty);
        else if (key == K_title) {
            Node *t = find_element(document, "title");
            Buf b = {0};
            if (t) text_of(t, &b, 0);
            *out = v_str(buf_done(&b));
        } else if (key == K_forms) return collect_tag("form", out);
        else if (key == K_images) return collect_tag("img", out);
        else if (key == K_scripts) return collect_tag("script", out);
        else if (key == K_links) return collect_tag("a", out);
        else if (key == K_activeElement) *out = wrap_v(focused_node ? focused_node : body_element);
        else if (key == K_defaultView) *out = v_obj(global_obj);
        else if (key == K_visibilityState) *out = v_str(intern_c("visible"));
        else if (key == K_hidden) *out = v_bool(0);
        else if (key == K_characterSet) *out = v_str(intern_c("UTF-8"));
        else if (key == K_compatMode) *out = v_str(intern_c("CSS1Compat"));
        else if (key == K_contentType) *out = v_str(intern_c("text/html"));
        else if (key == K_currentScript) *out = wrap_v(current_script);
        else if (key == K_fullscreenElement) *out = v_null();
        else if (key == K_scrollTop || key == K_scrollLeft) *out = v_num(key == K_scrollTop ? page_scroll_y : 0);
        else *handled = 0;
    } else if (key == K_id) *out = attr_or_empty(n, "id");
    else if (key == K_attributes && n->type == NODE_ELEMENT) return make_attributes(o, out);
    else if (key == K_className) *out = attr_or_empty(n, "class");
    else if (key == K_classList) {
        Value cached;
        Prop *p = own_prop(o, intern_c("__classList"));
        if (p) cached = p->v;
        else {
            cached = v_obj(helper_object(ClassListProto, CL_CLASSLIST, n));
            prop_set_raw(o, intern_c("__classList"), cached, P_HIDDEN);
        }
        *out = cached;
    } else if (key == K_style) {
        Prop *p = own_prop(o, intern_c("__style"));
        if (p) *out = p->v;
        else {
            *out = v_obj(helper_object(StyleProto, CL_STYLE, n));
            prop_set_raw(o, intern_c("__style"), *out, P_HIDDEN);
        }
    } else if (key == K_dataset) {
        Prop *p = own_prop(o, intern_c("__dataset"));
        if (p) *out = p->v;
        else {
            *out = v_obj(helper_object(DatasetProto, CL_DATASET, n));
            prop_set_raw(o, intern_c("__dataset"), *out, P_HIDDEN);
        }
    } else if (key == K_innerHTML || key == K_outerHTML) {
        Buf b = {0};
        if (key == K_outerHTML) {
            Node holder;
            memset(&holder, 0, sizeof(holder));
            holder.first = holder.last = n;
            Node *saved_next = n->next;
            n->next = NULL;
            serialize(&holder, &b, 0);
            n->next = saved_next;
        } else {
            serialize(n, &b, is(n->tag, "script") || is(n->tag, "style"));
        }
        *out = v_str(buf_done(&b));
    } else if (key == K_value) {
        if (is(n->tag, "select")) {
            Node *opt;
            if (select_option(n, n->selected_index, &opt)) {
                Value v = attr_value(opt, "value");
                if (v.t == V_NULL) {
                    Buf b = {0};
                    text_of(opt, &b, 0);
                    v = v_str(buf_done(&b));
                }
                *out = v;
            } else {
                *out = v_str(S_empty);
            }
        } else if (is(n->tag, "option")) {
            Value v = attr_value(n, "value");
            if (v.t == V_NULL) {
                Buf b = {0};
                text_of(n, &b, 0);
                v = v_str(buf_done(&b));
            }
            *out = v;
        } else if (is(n->tag, "input") || is(n->tag, "textarea")) {
            if (n->value) *out = v_str(str_c(n->value));
            else *out = attr_or_empty(n, "value");
        } else {
            *out = attr_or_empty(n, "value");
        }
    } else if (key == K_defaultValue) *out = attr_or_empty(n, "value");
    else if (key == K_checked) *out = v_bool(n->checked);
    else if (key == K_defaultChecked) *out = v_bool(has_attr(n, "checked"));
    else if (key == K_selected) {
        int is_sel = 0;
        if (is(n->tag, "option")) {
            Node *sel = n->parent;
            while (sel && !is(sel->tag, "select")) sel = sel->parent;
            Node *opt;
            is_sel = sel && select_option(sel, sel->selected_index, &opt) && opt == n;
        }
        *out = v_bool(is_sel);
    } else if (key == K_disabled || key == K_hidden || key == K_readOnly || key == K_required || key == K_multiple) {
        *out = v_bool(has_attr(n, key == K_readOnly ? "readonly" : key->s));
    } else if (key == K_selectedIndex) *out = v_num(n->selected_index);
    else if (key == K_options) {
        JsObj *a = new_array(0);
        for (Node *d = n; d; d = next_in_tree(d, n)) {
            if (d->type == NODE_ELEMENT && is(d->tag, "option")) arr_push(a, wrap_v(d));
        }
        prop_set_raw(a, K_selectedIndex, v_num(n->selected_index), 0);
        *out = v_obj(a);
    } else if (key == K_type) {
        Value v = attr_value(n, "type");
        if (v.t == V_NULL) v = v_str(intern_c(is(n->tag, "input") ? "text" : is(n->tag, "button") ? "submit" : is(n->tag, "select") ? "select-one" : ""));
        *out = v;
    } else if (key == K_href) *out = resolved_attr(n, "href");
    else if (key == K_src) *out = resolved_attr(n, "src");
    else if (key == K_title) *out = attr_or_empty(n, "title");
    else if (key == K_form) {
        Node *f = n->parent;
        while (f && !is(f->tag, "form")) f = f->parent;
        *out = wrap_v(f);
    } else if (key == K_elements) {
        JsObj *a = new_array(0);
        for (Node *d = n; d; d = next_in_tree(d, n)) {
            if (d->type == NODE_ELEMENT && (is(d->tag, "input") || is(d->tag, "select") || is(d->tag, "textarea") || is(d->tag, "button"))) {
                arr_push(a, wrap_v(d));
                char name[128];
                if (attr(d, "name", name, sizeof(name)) && name[0]) prop_set_raw(a, intern_c(name), wrap_v(d), P_HIDDEN);
            }
        }
        *out = v_obj(a);
    } else if (key == K_offsetWidth || key == K_clientWidth || key == K_scrollWidth) {
        ensure_layout();
        *out = v_num(n->style.display == D_NONE ? 0 : n->w);
    } else if (key == K_offsetHeight || key == K_clientHeight || key == K_scrollHeight) {
        ensure_layout();
        *out = v_num(n->style.display == D_NONE ? 0 : n == html_element || n == body_element ? (key == K_clientHeight ? 526 : page_height) : n->h);
    } else if (key == K_offsetTop || key == K_offsetLeft) {
        ensure_layout();
        *out = v_num(key == K_offsetTop ? n->y : n->x);
    } else if (key == K_offsetParent) *out = wrap_v(body_element);
    else if (key == K_scrollTop || key == K_scrollLeft) *out = v_num(key == K_scrollTop && (n == html_element || n == body_element) ? page_scroll_y : 0);
    else if (key == K_naturalWidth || key == K_naturalHeight) *out = v_num(n->img_state == 1 ? (key == K_naturalWidth ? n->img_w : n->img_h) : 0);
    else if (key == K_complete) *out = v_bool(n->img_state != 0);
    else if (key == K_width || key == K_height) {
        if (is(n->tag, "img") || is(n->tag, "canvas") || is(n->tag, "video")) {
            ensure_layout();
            *out = v_num(key == K_width ? n->w : n->h);
        } else {
            *handled = 0;
        }
    } else if (key == K_tabIndex) {
        Value v = attr_value(n, "tabindex");
        double d = 0;
        if (v.t == V_STR) d = str_to_number(v.u.s);
        *out = v_num(is_nan(d) ? 0 : d);
    } else if (key == K_colSpan || key == K_rowSpan) {
        Value v = attr_value(n, key == K_colSpan ? "colspan" : "rowspan");
        *out = v_num(v.t == V_STR ? str_to_number(v.u.s) : 1);
    } else if (key == K_maxLength) {
        Value v = attr_value(n, "maxlength");
        *out = v_num(v.t == V_STR ? str_to_number(v.u.s) : -1);
    } else {
        const char *a = reflected(key);
        if (a) *out = attr_or_empty(n, a);
        else *handled = 0;
    }
    return C_NORMAL;
}

static int navigate_to_value(Value v) {
    JsStr *s;
    TRY(to_string(v, &s));
    char full[2048];
    resolve_url(base_url, s->s, full, sizeof(full));
    free(js_navigate);
    js_navigate = copy_of(full, (int)strlen(full));
    return C_NORMAL;
}

static int node_set(JsObj *o, Node *n, JsStr *key, Value v, int *handled) {
    (void)o;
    int el = n->type == NODE_ELEMENT;
    JsStr *s = NULL;
    if (key->len == 0 || key->s[0] == '_' || (key->len > 2 && key->s[0] == 'o' && key->s[1] == 'n')) {
        *handled = 0; /* event handler properties are ordinary properties */
        return C_NORMAL;
    }
    if (n == document) {
        if (key == K_title) {
            TRY(to_string(v, &s));
            Node *t = find_element(document, "title");
            if (!t) {
                t = new_node(NODE_ELEMENT, "title");
                if (t) append_child(head_element, t);
            }
            if (t) set_text_children(t, s->s, s->len);
        } else if (key == K_cookie) {
            TRY(to_string(v, &s));
            cookie_set(current_url, s->s, 1);
        } else if (key == K_location) {
            return navigate_to_value(v);
        } else if (key == K_body) {
            *handled = 0;
        } else {
            *handled = 0;
        }
        return C_NORMAL;
    }
    if (key == K_textContent || key == K_innerText || key == K_nodeValue || key == K_data) {
        if (is_nullish(v)) s = S_empty;
        else TRY(to_string(v, &s));
        if (!el) {
            int len;
            char *t = page_text(s->s, s->len, &len);
            if (t) {
                free(n->text);
                n->text = t;
                n->text_len = len;
            }
            dom_dirty = 1;
        } else if (key == K_nodeValue || key == K_data) {
            *handled = 0;
        } else {
            set_text_children(n, s->s, s->len);
        }
        return C_NORMAL;
    }
    if (!el) {
        *handled = 0;
        return C_NORMAL;
    }
    if (key == K_innerHTML) {
        if (is_nullish(v)) s = S_empty;
        else TRY(to_string(v, &s));
        remove_all_children(n);
        if (is(n->tag, "script") || is(n->tag, "style") || is(n->tag, "textarea")) {
            set_text_children(n, s->s, s->len);
        } else {
            insert_fragment(n, parse_fragment(s->s, s->len), NULL);
        }
        dom_dirty = 1;
        return C_NORMAL;
    }
    if (key == K_outerHTML) {
        TRY(to_string(v, &s));
        if (n->parent) {
            Node *parent = n->parent, *next = n->next;
            insert_fragment(parent, parse_fragment(s->s, s->len), next);
            remove_child(n);
            orphan_add(n);
        }
        return C_NORMAL;
    }
    if (key == K_style) {
        TRY(to_string(v, &s));
        set_attribute(n, "style", s->s);
        return C_NORMAL;
    }
    if (key == K_value) {
        if (is_nullish(v)) s = S_empty;
        else TRY(to_string(v, &s));
        if (is(n->tag, "select")) {
            Node *opt;
            for (int i = 0; select_option(n, i, &opt); i++) {
                Value ov;
                TRY(node_get(wrap(opt), opt, K_value, &ov, handled));
                if (ov.t == V_STR && str_eq(ov.u.s, s)) {
                    n->selected_index = i;
                    break;
                }
            }
            *handled = 1;
        } else if (is(n->tag, "input") || is(n->tag, "textarea")) {
            int len;
            char *t = page_text(s->s, s->len, &len);
            if (t) {
                free(n->value);
                n->value = t;
            }
        } else {
            set_attribute(n, "value", s->s);
        }
        dom_dirty = 1;
        return C_NORMAL;
    }
    if (key == K_checked) {
        n->checked = to_boolean(v);
        dom_dirty = 1;
        return C_NORMAL;
    }
    if (key == K_selectedIndex) {
        double d;
        TRY(to_number(v, &d));
        n->selected_index = is_nan(d) || d < 0 ? 0 : (int)d;
        dom_dirty = 1;
        return C_NORMAL;
    }
    if (key == K_disabled || key == K_hidden || key == K_readOnly || key == K_required || key == K_multiple) {
        const char *name = key == K_readOnly ? "readonly" : key->s;
        if (to_boolean(v)) set_attribute(n, name, "");
        else remove_attribute(n, name);
        return C_NORMAL;
    }
    if (key == K_selected) {
        if (to_boolean(v)) {
            Node *sel = n->parent;
            while (sel && !is(sel->tag, "select")) sel = sel->parent;
            Node *opt;
            for (int i = 0; sel && select_option(sel, i, &opt); i++) {
                if (opt == n) sel->selected_index = i;
            }
            dom_dirty = 1;
        }
        return C_NORMAL;
    }
    const char *a = key == K_id ? "id" : key == K_className ? "class" : key == K_href ? "href" : key == K_src ? "src" : key == K_title ? "title"
                  : key == K_type ? "type" : key == K_tabIndex ? "tabindex" : key == K_colSpan ? "colspan" : key == K_rowSpan ? "rowspan"
                  : (key == K_width || key == K_height) && (is(n->tag, "img") || is(n->tag, "canvas") || is(n->tag, "video") || is(n->tag, "iframe")) ? key->s
                                                                                                                                                       : reflected(key);
    if (a) {
        TRY(to_string(v, &s));
        set_attribute(n, a, s->s);
        return C_NORMAL;
    }
    *handled = 0;
    return C_NORMAL;
}

/* element.attributes: a list of {name, value} (also by name). A snapshot,
   made at each use, rather than live. */
/* attributes.getNamedItem(name) (magic 0), attributes.item(i) (magic 1) */
NATIVE(attributes_method) {
    UNUSED;
    *ret = v_null();
    if (this_v.t != V_OBJ) return C_NORMAL;
    if ((self->magic & 0xFFFF) == 1) {
        Value v;
        TRY(get_index(this_v, ARG(0), &v));
        if (v.t == V_OBJ) *ret = v;
        return C_NORMAL;
    }
    JsStr *k;
    TRY(to_key(ARG(0), &k));
    Prop *p = own_prop(this_v.u.o, k);
    if (p && p->v.t == V_OBJ) *ret = p->v;
    return C_NORMAL;
}

static int make_attributes(JsObj *o, Value *out) {
    Value names_fn, getter, names;
    TRY(get_value(v_obj(o), intern_c("getAttributeNames"), &names_fn));
    TRY(get_value(v_obj(o), intern_c("getAttribute"), &getter));
    JsObj *list = new_array(0);
    *out = v_obj(list);
    define_method(list, "getNamedItem", attributes_method, 1, 0);
    define_method(list, "item", attributes_method, 1, 1);
    if (!is_callable(names_fn) || !is_callable(getter)) return C_NORMAL;
    TRY(call_function(names_fn.u.o, v_obj(o), NULL, 0, &names));
    for (int i = 0; names.t == V_OBJ && i < names.u.o->alen; i++) {
        Value name = names.u.o->arr[i], value;
        TRY(call_function(getter.u.o, v_obj(o), &name, 1, &value));
        JsObj *a = new_object();
        prop_set_raw(a, S_name, name, 0);
        prop_set_raw(a, intern_c("value"), value, 0);
        prop_set_raw(a, intern_c("nodeName"), name, 0);
        prop_set_raw(a, intern_c("localName"), name, 0);
        prop_set_raw(a, intern_c("nodeValue"), value, 0);
        prop_set_raw(a, intern_c("nodeType"), v_num(2), 0);
        prop_set_raw(a, intern_c("specified"), v_bool(1), 0);
        prop_set_raw(a, intern_c("ownerElement"), v_obj(o), 0);
        arr_push(list, v_obj(a));
        JsStr *k;
        TRY(to_key(name, &k));
        if (k->index < 0) prop_set_raw(list, k, v_obj(a), P_HIDDEN);
    }
    return C_NORMAL;
}

static int dom_get(JsObj *o, JsStr *key, Value *out, int *handled) {
    *handled = 1;
    switch (o->cls) {
    case CL_NODE:
        if (o->node) return node_get(o, o->node, key, out, handled);
        break;
    case CL_STYLE: return style_get(o, key, out, handled);
    case CL_CLASSLIST: return classlist_get(o, key, out, handled);
    case CL_DATASET: return dataset_get(o, key, out, handled);
    case CL_STORAGE: return storage_get(o, key, out, handled);
    }
    *handled = 0;
    return C_NORMAL;
}

static int dom_set(JsObj *o, JsStr *key, Value v, int *handled);

/* The get / set (magic 1) half of a DOM property's accessor: self->this_val
   is the property's name. */
NATIVE(dom_accessor) {
    UNUSED;
    JsStr *key = self->this_val.u.s;
    *ret = v_undef();
    if (this_v.t != V_OBJ) return C_NORMAL;
    int handled = 0;
    if (self->magic & 0xFFFF) {
        TRY(dom_set(this_v.u.o, key, ARG(0), &handled));
        if (!handled) prop_set_raw(this_v.u.o, key, ARG(0), 0);
        return C_NORMAL;
    }
    return dom_get(this_v.u.o, key, ret, &handled);
}

static int is_dom_name(JsStr *k) {
#define IS_DOM_NAME(n) if (k == K_##n) return 1;
    DOM_NAMES(IS_DOM_NAME)
    return 0;
}

/* DOM properties are worked out per node rather than stored, so the
   prototypes have no properties for them. Code that asks for one's
   descriptor (Object.getOwnPropertyDescriptor(HTMLElement.prototype,
   "innerHTML").get, often to call the original getter) gets an accessor,
   made the first time and kept. */
static int dom_accessor_prepare(JsObj *o, JsStr *key) {
    int dom_proto = o == NodeProto || o == DocumentProto || o == ElementProto || o == HTMLElementProto;
    for (int i = 0; i < IFACE_COUNT && !dom_proto; i++) dom_proto = o == tag_protos[i];
    if (!dom_proto || !is_dom_name(key)) return 0;
    JsObj *g = make_native(key->s, dom_accessor, 0, 0), *s = make_native(key->s, dom_accessor, 1, 1);
    g->this_val = s->this_val = v_str(key);
    Prop *p = prop_add(o, key);
    p->flags = P_ACCESSOR | P_HIDDEN;
    p->get = g;
    p->set = s;
    return 1;
}

static int dom_set(JsObj *o, JsStr *key, Value v, int *handled) {
    *handled = 1;
    switch (o->cls) {
    case CL_NODE:
        if (o->node) return node_set(o, o->node, key, v, handled);
        break;
    case CL_STYLE: return style_set(o, key, v, handled);
    case CL_DATASET: return dataset_set(o, key, v, handled);
    case CL_STORAGE: {
        if (own_prop(StorageProto, key)) break;
        JsStr *s;
        TRY(to_string(v, &s));
        map_put(o->target, v_str(key), v_str(s));
        return C_NORMAL;
    }
    }
    *handled = 0;
    return C_NORMAL;
}

/* ---------------------------------------------------------------- events */

static JsObj *new_event(const char *type, int bubbles, int cancelable) {
    JsObj *e = obj_new_raw(CL_EVENT, EventProto);
    prop_set_raw(e, K_type, v_str(intern_c(type)), 0);
    prop_set_raw(e, K_bubbles, v_bool(bubbles), 0);
    prop_set_raw(e, K_cancelable, v_bool(cancelable), 0);
    prop_set_raw(e, K_defaultPrevented, v_bool(0), 0);
    prop_set_raw(e, intern_c("timeStamp"), v_num((double)uptime_ms()), 0);
    prop_set_raw(e, intern_c("isTrusted"), v_bool(1), 0);
    return e;
}

/* The listeners object of an event target (created on demand). */
static JsObj *listeners_of(JsObj *target, int create) {
    Prop *p = own_prop(target, K_listeners);
    if (p && p->v.t == V_OBJ) return p->v.u.o;
    if (!create) return NULL;
    JsObj *l = new_object();
    prop_set_raw(target, K_listeners, v_obj(l), P_HIDDEN);
    return l;
}

/* magic: 0 add, 1 remove */
NATIVE(event_listener) {
    UNUSED;
    JsObj *target = this_v.t == V_OBJ ? this_v.u.o : global_obj;
    JsStr *type;
    TRY(arg_string(args, argc, 0, &type));
    type = interned(type);
    Value fn = ARG(1);
    *ret = v_undef();
    if (fn.t != V_OBJ) return C_NORMAL;
    int once = 0;
    if (ARG(2).t == V_OBJ) {
        Value o;
        TRY(get_value(ARG(2), intern_c("once"), &o));
        once = to_boolean(o);
    }
    JsObj *l = listeners_of(target, (self->magic & 0xFFFF) == 0);
    if (!l) return C_NORMAL;
    Prop *p = own_prop(l, type);
    JsObj *list;
    if (p && p->v.t == V_OBJ) list = p->v.u.o;
    else {
        if (self->magic & 0xFFFF) return C_NORMAL;
        list = new_array(0);
        prop_set_raw(l, type, v_obj(list), 0);
    }
    for (int i = 0; i < list->alen; i++) {
        JsObj *entry = list->arr[i].u.o;
        if (strict_equals(entry->arr[0], fn)) {
            if (self->magic & 0xFFFF) {
                memmove(list->arr + i, list->arr + i + 1, sizeof(Value) * (size_t)(list->alen - i - 1));
                list->alen--;
            }
            return C_NORMAL;
        }
    }
    if (self->magic & 0xFFFF) return C_NORMAL;
    JsObj *entry = new_array(2);
    arr_push(entry, fn);
    arr_push(entry, v_bool(once));
    arr_push(list, v_obj(entry));
    return C_NORMAL;
}

/* Call one handler; errors are reported, not passed on (unless the script must stop). */
static int call_handler(Value fn, Value this_v, JsObj *event) {
    Value ev = v_obj(event), r;
    int code;
    if (is_callable(fn)) {
        code = call_function(fn.u.o, this_v, &ev, 1, &r);
    } else if (fn.t == V_OBJ) { /* an object with handleEvent */
        Value h;
        code = get_value(fn, S_handleEvent, &h);
        if (code == C_NORMAL && is_callable(h)) code = call_function(h.u.o, fn, &ev, 1, &r);
        else return 0;
    } else {
        return 0;
    }
    if (code == C_THROW) {
        if (js_abort) return -1;
        describe_exception(js_last_error, sizeof(js_last_error));
        js_report(js_last_error);
        js_errors++;
        js_exception = v_undef();
        return 0;
    }
    if (code == C_NORMAL && r.t == V_BOOL && !r.u.b) { /* "return false" from an onxxx handler */
        prop_set_raw(event, K_defaultPrevented, v_bool(1), 0);
    }
    return 0;
}

/* The handler from an attribute such as onclick="...", compiled once. */
static Value attribute_handler(JsObj *target, Node *n, JsStr *type) {
    if (!n || n->type != NODE_ELEMENT) return v_undef();
    char name[64] = "on";
    strncat_safe(name, type->s, 60);
    static char code[8192];
    if (!attr(n, name, code, sizeof(code)) || !code[0]) return v_undef();
    char cache_name[80] = "__attr_";
    strncat_safe(cache_name, name, 70);
    JsStr *ck = intern_c(cache_name);
    Prop *cached = own_prop(target, ck);
    if (cached) return cached->v;
    Buf src = {0};
    buf_c(&src, "(function(event){");
    buf_c(&src, code);
    buf_c(&src, "\n})");
    Ast *program = parse_script(src.p, src.len);
    free(src.p);
    Value fn = v_undef();
    if (program && program->list && program->list->type == N_EXPR) {
        Frame f;
        memset(&f, 0, sizeof(f));
        f.env = global_lex;
        f.var_env = global_env;
        f.this_v = v_obj(global_obj);
        if (eval(program->list->a, &f, &fn) != C_NORMAL) fn = v_undef();
    } else if (!program) {
        js_report(parse_error);
        js_errors++;
    }
    prop_set_raw(target, ck, fn, P_HIDDEN);
    return fn;
}

/* Dispatch an event at a target, bubbling up to document and window.
   Returns 1 if a handler called preventDefault(), 0 if not, -1 if the
   scripts must stop. */
static int dispatch_event(JsObj *target, JsObj *event) {
    Value type_v;
    if (get_value(v_obj(event), K_type, &type_v) != C_NORMAL || type_v.t != V_STR) return 0;
    JsStr *type = interned(type_v.u.s);
    Value bubbles;
    get_value(v_obj(event), K_bubbles, &bubbles);
    prop_set_raw(event, K_target, v_obj(target), 0);
    JsObj *path[256];
    int count = 0;
    path[count++] = target;
    if (target->cls == CL_NODE && target->node && to_boolean(bubbles)) {
        for (Node *p = target->node->parent; p && count < 255; p = p->parent) path[count++] = wrap(p);
        if (contains_node(document, target->node) && count < 256) path[count++] = global_obj;
    }
    char on_name[64] = "on";
    strncat_safe(on_name, type->s, 60);
    JsStr *on_key = intern_c(on_name);
    for (int i = 0; i < count; i++) {
        JsObj *t = path[i];
        prop_set_raw(event, K_currentTarget, v_obj(t), 0);
        prop_set_raw(event, intern_c("eventPhase"), v_num(i == 0 ? 2 : 3), 0);
        JsObj *l = listeners_of(t, 0);
        Prop *p = l ? own_prop(l, type) : NULL;
        if (p && p->v.t == V_OBJ) {
            JsObj *list = p->v.u.o;
            /* a copy: handlers may add or remove listeners */
            JsObj *copy = new_array(list->alen);
            for (int k = 0; k < list->alen; k++) arr_push(copy, list->arr[k]);
            for (int k = 0; k < copy->alen; k++) {
                JsObj *entry = copy->arr[k].u.o;
                if (to_boolean(entry->arr[1])) { /* once: remove before calling */
                    for (int q = 0; q < list->alen; q++) {
                        if (list->arr[q].u.o == entry) {
                            memmove(list->arr + q, list->arr + q + 1, sizeof(Value) * (size_t)(list->alen - q - 1));
                            list->alen--;
                            break;
                        }
                    }
                }
                if (call_handler(entry->arr[0], v_obj(t), event) < 0) return -1;
                if (own_prop(event, K_stop_now)) break;
            }
        }
        /* on<type> property, or attribute */
        Prop *handler = own_prop(t, on_key);
        Value h = handler ? handler->v : attribute_handler(t, t->cls == CL_NODE ? t->node : NULL, type);
        if (is_callable(h) && call_handler(h, v_obj(t), event) < 0) return -1;
        if (own_prop(event, K_stop)) break;
    }
    Value prevented;
    get_value(v_obj(event), K_defaultPrevented, &prevented);
    return to_boolean(prevented);
}

/* magic: 0 preventDefault, 1 stopPropagation, 2 stopImmediatePropagation */
NATIVE(event_method) {
    UNUSED;
    if (this_v.t != V_OBJ) {
        *ret = v_undef();
        return C_NORMAL;
    }
    JsObj *e = this_v.u.o;
    int mode = self->magic & 0xFFFF;
    if (mode == 0) {
        Value c;
        get_value(this_v, K_cancelable, &c);
        if (to_boolean(c)) prop_set_raw(e, K_defaultPrevented, v_bool(1), 0);
    } else {
        prop_set_raw(e, K_stop, v_bool(1), P_HIDDEN);
        if (mode == 2) prop_set_raw(e, K_stop_now, v_bool(1), P_HIDDEN);
    }
    *ret = v_undef();
    return C_NORMAL;
}

/* new Event(type, {bubbles, cancelable, detail}) / new CustomEvent(...) */
NATIVE(event_ctor) {
    UNUSED;
    JsStr *type;
    TRY(arg_string(args, argc, 0, &type));
    Value opts = ARG(1), b = v_undef(), c = v_undef(), d = v_null();
    if (opts.t == V_OBJ) {
        TRY(get_value(opts, K_bubbles, &b));
        TRY(get_value(opts, K_cancelable, &c));
        TRY(get_value(opts, intern_c("detail"), &d));
    }
    JsObj *e = new_event(type->s, to_boolean(b), to_boolean(c));
    prop_set_raw(e, intern_c("detail"), d, 0);
    prop_set_raw(e, intern_c("isTrusted"), v_bool(0), 0);
    if (opts.t == V_OBJ) { /* KeyboardEvent / MouseEvent fields */
        static const char *extra[] = {"key", "code", "keyCode", "which", "clientX", "clientY", "button", "ctrlKey", "shiftKey", "altKey", "metaKey", NULL};
        for (int i = 0; extra[i]; i++) {
            Value v;
            TRY(get_value(opts, intern_c(extra[i]), &v));
            if (v.t != V_UNDEF) prop_set_raw(e, intern_c(extra[i]), v, 0);
        }
    }
    *ret = v_obj(e);
    return C_NORMAL;
}

NATIVE(dispatch_event_native) {
    UNUSED;
    JsObj *target = this_v.t == V_OBJ ? this_v.u.o : global_obj;
    if (ARG(0).t != V_OBJ) return throw_type("parameter 1 is not of type 'Event'", NULL, NULL);
    int r = dispatch_event(target, ARG(0).u.o);
    if (r < 0) return C_THROW;
    *ret = v_bool(!r);
    return C_NORMAL;
}

/* ---------------------------------------------------------------- timers */

typedef struct {
    int id;
    u64 due;
    int interval; /* ms; 0: once */
    Value fn;     /* a function, or code as a string */
    JsObj *args;
} Timer;

static Timer *timers;
static int timer_count, timer_cap, next_timer_id = 1;

/* magic: 0 setTimeout, 1 setInterval, 2 requestAnimationFrame */
NATIVE(set_timer) {
    UNUSED;
    int mode = self->magic & 0xFFFF;
    Value fn = ARG(0);
    double delay = mode == 2 ? 16 : 0;
    if (mode != 2 && argc > 1) TRY(to_number(args[1], &delay));
    if (is_nan(delay) || delay < 0) delay = 0;
    if (mode == 1 && delay < 4) delay = 4;
    if (delay > 2147483647.0) delay = 2147483647.0;
    if (!grow((void **)&timers, &timer_cap, timer_count + 1, sizeof(Timer))) return throw_error(RangeErrorProto, "out of memory");
    Timer *t = &timers[timer_count++];
    t->id = next_timer_id++;
    t->due = uptime_ms() + (u64)delay;
    t->interval = mode == 1 ? (int)delay : 0;
    t->fn = fn;
    t->args = new_array(0);
    for (int i = 2; mode != 2 && i < argc; i++) arr_push(t->args, args[i]);
    if (mode == 2) arr_push(t->args, v_num((double)uptime_ms()));
    *ret = v_num(t->id);
    return C_NORMAL;
}

NATIVE(clear_timer) {
    UNUSED;
    double id;
    TRY(arg_number(args, argc, 0, &id));
    for (int i = 0; i < timer_count; i++) {
        if (timers[i].id == (int)id) {
            timers[i] = timers[--timer_count];
            break;
        }
    }
    *ret = v_undef();
    return C_NORMAL;
}

/* Milliseconds until the next timer (-1: none). */
static int js_next_timer(void) {
    if (!timer_count && !refresh_due) return -1;
    u64 now = uptime_ms(), soonest = refresh_due ? refresh_due : timers[0].due;
    for (int i = 0; i < timer_count; i++) {
        if (timers[i].due < soonest) soonest = timers[i].due;
    }
    return soonest <= now ? 0 : (int)(soonest - now > 60000 ? 60000 : soonest - now);
}

/* Run the timers that are due. Returns how many ran. */
static int js_run_timers(void) {
    u64 now = uptime_ms();
    int ran = 0;
    if (refresh_due && now >= refresh_due && !js_navigate) { /* <meta http-equiv=refresh> */
        refresh_due = 0;
        js_navigate = copy_of(refresh_url, (int)strlen(refresh_url));
        return 1;
    }
    for (int pass = 0; pass < 64; pass++) {
        int found = -1;
        for (int i = 0; i < timer_count; i++) {
            if (timers[i].due <= now && (found < 0 || timers[i].due < timers[found].due)) found = i;
        }
        if (found < 0) break;
        Timer t = timers[found];
        if (t.interval) {
            timers[found].due = now + (u64)t.interval;
        } else {
            timers[found] = timers[--timer_count];
        }
        if (t.fn.t == V_STR) {
            js_run_script(t.fn.u.s->s, t.fn.u.s->len, "timer");
        } else if (js_call_toplevel(t.fn, v_obj(global_obj), t.args ? t.args->arr : NULL, t.args ? t.args->alen : 0, NULL) != 0) {
            js_errors++;
        }
        run_microtasks();
        ran++;
    }
    return ran;
}

/* ---------------------------------------------------------------- window, location, fetch */

NATIVE(window_alert) {
    UNUSED;
    JsStr *s = S_empty;
    if (argc) TRY(to_string(args[0], &s));
    int mode = self->magic & 0xFFFF; /* 0 alert, 1 confirm, 2 prompt */
    strcpy(js_alert, mode == 0 ? "Alert: " : mode == 1 ? "Confirm (answered OK): " : "Prompt (answered nothing): ");
    strncat_safe(js_alert, s->s, sizeof(js_alert) - strlen(js_alert) - 1);
    buf_c(&console_text, js_alert);
    buf_ch(&console_text, '\n');
    if (console_echo) printf("[%s]\n", js_alert);
    *ret = mode == 0 ? v_undef() : mode == 1 ? v_bool(1) : v_null();
    return C_NORMAL;
}

NATIVE(window_scroll) {
    UNUSED;
    double x = 0, y = 0;
    Value a = ARG(0);
    if (a.t == V_OBJ) {
        Value top;
        TRY(get_value(a, intern_c("top"), &top));
        if (top.t != V_UNDEF) TRY(to_number(top, &y));
    } else {
        TRY(arg_number(args, argc, 0, &x));
        TRY(arg_number(args, argc, 1, &y));
    }
    if ((self->magic & 0xFFFF) == 1) y += page_scroll_y; /* scrollBy */
    if (!is_nan(y)) js_scroll_to = y < 0 ? 0 : (int)y;
    *ret = v_undef();
    return C_NORMAL;
}

NATIVE(window_get_computed_style) {
    UNUSED;
    Node *n = node_of(ARG(0));
    if (!n) return throw_type("parameter 1 is not of type 'Element'", NULL, NULL);
    JsObj *s = helper_object(StyleProto, CL_STYLE, n);
    s->magic = 1;
    *ret = v_obj(s);
    return C_NORMAL;
}

NATIVE(window_match_media) {
    UNUSED;
    JsStr *q;
    TRY(arg_string(args, argc, 0, &q));
    JsObj *m = new_object();
    prop_set_raw(m, intern_c("matches"), v_bool(media_applies(q->s, q->len)), 0);
    prop_set_raw(m, intern_c("media"), v_str(q), 0);
    define_method(m, "addListener", noop, 1, 0);
    define_method(m, "removeListener", noop, 1, 0);
    define_method(m, "addEventListener", noop, 2, 0);
    define_method(m, "removeEventListener", noop, 2, 0);
    *ret = v_obj(m);
    return C_NORMAL;
}

NATIVE(performance_now) {
    UNUSED;
    *ret = v_num((double)uptime_ms());
    return C_NORMAL;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* magic: 0 btoa, 1 atob */
NATIVE(base64) {
    UNUSED;
    JsStr *s;
    TRY(arg_string(args, argc, 0, &s));
    Buf b = {0};
    if ((self->magic & 0xFFFF) == 0) {
        const unsigned char *p = (const unsigned char *)s->s;
        for (int i = 0; i < s->len; i += 3) {
            unsigned v = p[i] << 16 | (i + 1 < s->len ? p[i + 1] << 8 : 0) | (i + 2 < s->len ? p[i + 2] : 0);
            buf_ch(&b, B64[v >> 18 & 63]);
            buf_ch(&b, B64[v >> 12 & 63]);
            buf_ch(&b, i + 1 < s->len ? B64[v >> 6 & 63] : '=');
            buf_ch(&b, i + 2 < s->len ? B64[v & 63] : '=');
        }
    } else {
        unsigned acc = 0;
        int bits = 0;
        for (int i = 0; i < s->len; i++) {
            const char *at = strchr(B64, s->s[i]);
            if (!at || !s->s[i]) {
                if (s->s[i] == '=' || is_space(s->s[i])) continue;
                free(b.p);
                return throw_error(ErrorProto, "InvalidCharacterError: The string to be decoded is not correctly encoded.");
            }
            acc = acc << 6 | (unsigned)(at - B64);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                buf_ch(&b, (char)(acc >> bits & 255));
            }
        }
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

/* Split a URL into location-like parts on object o. */
static void set_url_parts(JsObj *o, const char *url) {
    char protocol[16] = "", host[256] = "", hostname[256] = "", port[16] = "", path[2048] = "/", search[2048] = "", hash[1024] = "";
    const char *p = url, *colon = strchr(url, ':');
    if (colon && colon - url < 15) {
        memcpy(protocol, url, (size_t)(colon - url + 1));
        protocol[colon - url + 1] = 0;
        p = colon + 1;
    }
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        int k = 0;
        while (*p && *p != '/' && *p != '?' && *p != '#' && k < 255) host[k++] = *p++;
        host[k] = 0;
        strcpy(hostname, host);
        char *pc = strchr(hostname, ':');
        if (pc) {
            strncpy(port, pc + 1, sizeof(port) - 1);
            *pc = 0;
        }
    }
    int k = 0;
    if (*p && *p != '?' && *p != '#') {
        while (*p && *p != '?' && *p != '#' && k < 2047) path[k++] = *p++;
        path[k] = 0;
    }
    k = 0;
    if (*p == '?') {
        while (*p && *p != '#' && k < 2047) search[k++] = *p++;
        search[k] = 0;
    }
    k = 0;
    if (*p == '#') {
        while (*p && k < 1023) hash[k++] = *p++;
        hash[k] = 0;
    }
    char origin[300] = "";
    if (host[0]) {
        strcpy(origin, protocol);
        strcat(origin, "//");
        strncat_safe(origin, host, 255);
    } else {
        strcpy(origin, "null");
    }
    prop_set_raw(o, K_href, v_str(str_c(url)), 0);
    prop_set_raw(o, intern_c("protocol"), v_str(str_c(protocol)), 0);
    prop_set_raw(o, intern_c("host"), v_str(str_c(host)), 0);
    prop_set_raw(o, intern_c("hostname"), v_str(str_c(hostname)), 0);
    prop_set_raw(o, intern_c("port"), v_str(str_c(port)), 0);
    prop_set_raw(o, intern_c("pathname"), v_str(str_c(path)), 0);
    prop_set_raw(o, intern_c("search"), v_str(str_c(search)), 0);
    prop_set_raw(o, intern_c("hash"), v_str(str_c(hash)), 0);
    prop_set_raw(o, intern_c("origin"), v_str(str_c(origin)), 0);
}

/* location.assign / replace (magic 0), reload (1), toString (2) */
NATIVE(location_method) {
    UNUSED;
    int mode = self->magic & 0xFFFF;
    if (mode == 2) {
        Value href;
        TRY(get_value(this_v, K_href, &href));
        *ret = href;
        return C_NORMAL;
    }
    *ret = v_undef();
    if (mode == 1) return navigate_to_value(v_str(str_c(current_url)));
    return navigate_to_value(ARG(0));
}

NATIVE(window_location_get) {
    UNUSED;
    *ret = location_obj ? v_obj(location_obj) : v_null();
    return C_NORMAL;
}

NATIVE(window_location_set) {
    UNUSED;
    *ret = v_undef();
    return navigate_to_value(ARG(0));
}

/* Setting location.href (or location itself) navigates. */
NATIVE(location_href_set) {
    UNUSED;
    *ret = v_undef();
    return navigate_to_value(ARG(0));
}

NATIVE(location_href_get) {
    UNUSED;
    *ret = v_str(str_c(current_url));
    return C_NORMAL;
}

NATIVE(history_method) {
    UNUSED;
    int mode = self->magic & 0xFFFF; /* 0 pushState/replaceState, 1 back, 2 forward */
    if (mode == 0 && argc >= 3 && !is_nullish(args[2])) {
        JsStr *u;
        TRY(to_string(args[2], &u));
        char full[1024];
        resolve_url(base_url, u->s, full, sizeof(full));
        strcpy(current_url, full); /* the address bar shows it; nothing is loaded */
        set_url_parts(location_obj, full);
        define_getter(location_obj, "href", location_href_get, location_href_set, 0);
    } else if (mode == 1) {
        free(js_navigate);
        js_navigate = copy_of("rdx-back:", 9);
    }
    *ret = v_undef();
    return C_NORMAL;
}

/* Is a script on this page allowed to fetch `url`? Pages from the web may
   not read files on this machine. */
static int fetch_allowed(const char *url) {
    int page_web = !strncasecmp(current_url, "http://", 7) || !strncasecmp(current_url, "https://", 8);
    int target_web = !strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8) || !strncasecmp(url, "blob:", 5);
    return target_web || !page_web;
}

static JsObj *response_new(Fetched *f, const char *url, int failed);

static int random_uuid(JsObj *self, Value this_v, Value *args, int argc, Value *ret);

/* The bytes of an ArrayBuffer, typed array or DataView (or a string's). */
static int bytes_of(Value v, JsStr **out) {
    if (v.t == V_OBJ && v.u.o->cls == CL_BUFFER) {
        *out = str_new(v.u.o->extra, v.u.o->extra_len);
        return C_NORMAL;
    }
    if (v.t == V_OBJ && v.u.o->cls == CL_ARRAY && v.u.o->fn_kind) {
        JsObj *a = v.u.o, *buf = typed_buffer_of(a);
        int size = TYPED_BYTES[a->fn_kind];
        *out = str_new((char *)buf->extra + a->magic, a->alen * size);
        return C_NORMAL;
    }
    if (v.t == V_OBJ && v.u.o->proto == DataViewProto && v.u.o->target) {
        *out = str_new((char *)v.u.o->target->extra + v.u.o->magic, v.u.o->extra_len);
        return C_NORMAL;
    }
    return to_string(v, out);
}

/* (for the prelude) __rdxBytes(x): x's bytes as a string; __rdxBuffer(s): an ArrayBuffer of a string's bytes */
NATIVE(rdx_bytes) {
    UNUSED;
    JsStr *s;
    TRY(bytes_of(ARG(0), &s));
    if ((self->magic & 0xFFFF) == 0) {
        *ret = v_str(s);
        return C_NORMAL;
    }
    JsObj *b = buffer_new(s->len);
    if (b->extra_len == s->len && s->len) memcpy(b->extra, s->s, (size_t)s->len);
    *ret = v_obj(b);
    return C_NORMAL;
}

static unsigned crc32_of(const unsigned char *p, int n) {
    unsigned c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* (for the prelude's CompressionStream) __rdxZip(bytes, format, 0 compress
   / 1 decompress): the result's bytes, or null if they could not be
   unpacked. Packing stores the data in "stored" deflate blocks: valid
   gzip / zlib / deflate that any reader accepts, just not smaller. */
NATIVE(rdx_zip) {
    UNUSED;
    JsStr *in, *format;
    TRY(to_string(ARG(0), &in));
    TRY(to_string(ARG(1), &format));
    int gzip = !strcmp(format->s, "gzip"), zlib = !strcmp(format->s, "deflate");
    const unsigned char *p = (const unsigned char *)in->s;
    int n = in->len;
    if (to_boolean(ARG(2))) {
        unsigned char *out = NULL;
        size_t out_len = 0;
        int r = gzip ? gzip_decompress(p, (size_t)n, &out, &out_len, 0)
                : zlib ? zlib_decompress(p, (size_t)n, &out, &out_len, 0)
                       : inflate_raw(p, (size_t)n, &out, &out_len, 0);
        if (r != 0) {
            *ret = v_null();
            return C_NORMAL;
        }
        *ret = v_str(str_new((const char *)out, (int)out_len));
        free(out);
        return C_NORMAL;
    }
    Buf b = {0};
    if (gzip) buf_add(&b, "\x1f\x8b\x08\0\0\0\0\0\0\xff", 10);
    if (zlib) buf_add(&b, "\x78\x01", 2);
    int at = 0;
    do {
        int chunk = n - at > 65535 ? 65535 : n - at;
        unsigned char head[5] = {(unsigned char)(at + chunk >= n), (unsigned char)chunk, (unsigned char)(chunk >> 8),
                                 (unsigned char)~chunk, (unsigned char)(~chunk >> 8)};
        buf_add(&b, (const char *)head, 5);
        buf_add(&b, (const char *)p + at, chunk);
        at += chunk;
    } while (at < n);
    if (gzip) {
        unsigned tail[2] = {crc32_of(p, n), (unsigned)n};
        buf_add(&b, (const char *)tail, 8);
    }
    if (zlib) {
        unsigned a = 1, c = 0;
        for (int i = 0; i < n; i++) {
            a = (a + p[i]) % 65521;
            c = (c + a) % 65521;
        }
        unsigned adler = (c << 16) | a;
        unsigned char be[4] = {(unsigned char)(adler >> 24), (unsigned char)(adler >> 16), (unsigned char)(adler >> 8), (unsigned char)adler};
        buf_add(&b, (const char *)be, 4);
    }
    *ret = v_str(b.p ? str_new(b.p, b.len) : S_empty);
    free(b.p);
    return C_NORMAL;
}

/* URL.createObjectURL(blob) (magic 0) / URL.revokeObjectURL(url) (1) */
NATIVE(object_url) {
    UNUSED;
    if ((self->magic & 0xFFFF) == 1) {
        JsStr *u;
        TRY(to_string(ARG(0), &u));
        blob_revoke(u->s);
        *ret = v_undef();
        return C_NORMAL;
    }
    Value text, type;
    if (ARG(0).t != V_OBJ) return throw_type("Failed to execute 'createObjectURL' on 'URL': Overload resolution failed.", NULL, NULL);
    TRY(get_value(ARG(0), intern_c("_text"), &text));
    TRY(get_value(ARG(0), intern_c("type"), &type));
    JsStr *data = S_empty, *t = S_empty;
    if (text.t == V_STR) data = text.u.s;
    if (type.t == V_STR) t = type.u.s;
    /* blob:<the page's origin>/<a random id> */
    char url[300] = "blob:";
    const char *p = strstr(current_url, "://");
    if (p && (!strncasecmp(current_url, "http", 4))) {
        p += 3;
        while (*p && *p != '/' && *p != '?' && *p != '#') p++;
        size_t n = (size_t)(p - current_url);
        if (n > 200) n = 200;
        memcpy(url + 5, current_url, n);
        url[5 + n] = 0;
    } else {
        strcat(url, "null");
    }
    Value id;
    TRY(random_uuid(self, v_undef(), NULL, 0, &id));
    strcat(url, "/");
    if (id.t == V_STR) strncat_safe(url, id.u.s->s, 60);
    blob_put(url, data->s, (size_t)data->len, t->s);
    *ret = v_str(str_c(url));
    return C_NORMAL;
}

NATIVE(response_body) {
    UNUSED;
    /* magic 0 text, 1 json, 2 arrayBuffer; this.__body has the text */
    Value body;
    TRY(get_value(this_v, intern_c("__body"), &body));
    JsObj *p = promise_new();
    if ((self->magic & 0xFFFF) == 2) {
        JsStr *s = body.t == V_STR ? body.u.s : S_empty;
        JsObj *b = buffer_new(s->len);
        if (b->extra_len == s->len && s->len) memcpy(b->extra, s->s, (size_t)s->len);
        promise_resolve(p, v_obj(b));
    } else if ((self->magic & 0xFFFF) == 1) {
        Value parsed;
        int code = json_parse(self, v_undef(), &body, 1, &parsed);
        if (code == C_NORMAL) promise_resolve(p, parsed);
        else {
            promise_settle(p, 2, js_exception);
            js_exception = v_undef();
        }
    } else {
        promise_resolve(p, body);
    }
    *ret = v_obj(p);
    return C_NORMAL;
}

NATIVE(headers_get) {
    UNUSED;
    JsStr *name;
    TRY(arg_string(args, argc, 0, &name));
    Value t;
    TRY(get_value(this_v, intern_c("__type"), &t));
    *ret = !strncasecmp(name->s, "content-type", 12) ? t : v_null();
    return C_NORMAL;
}

static JsObj *response_new(Fetched *f, const char *url, int failed) {
    JsObj *r = new_object();
    int status = failed ? 0 : f->status ? f->status : 200;
    prop_set_raw(r, K_status, v_num(status), 0);
    prop_set_raw(r, intern_c("ok"), v_bool(status >= 200 && status < 300), 0);
    prop_set_raw(r, K_statusText, v_str(intern_c(status == 200 ? "OK" : "")), 0);
    prop_set_raw(r, intern_c("url"), v_str(str_c(url)), 0);
    prop_set_raw(r, intern_c("__body"), v_str(failed ? S_empty : str_new(f->body, (int)f->len)), P_HIDDEN);
    JsObj *h = new_object();
    prop_set_raw(h, intern_c("__type"), v_str(str_c(failed ? "" : f->type)), P_HIDDEN);
    define_method(h, "get", headers_get, 1, 0);
    define_method(h, "has", headers_get, 1, 0);
    prop_set_raw(r, intern_c("headers"), v_obj(h), 0);
    define_method(r, "text", response_body, 0, 0);
    define_method(r, "json", response_body, 0, 1);
    define_method(r, "arrayBuffer", response_body, 0, 2);
    return r;
}

static JsObj *SearchParamsProto;

/* A request header a script may set (not the ones the browser itself
   vouches for: cookies, where the request comes from, ...). */
static int header_allowed(const char *name) {
    static const char *forbidden[] = {"cookie", "cookie2", "host", "content-length", "connection", "keep-alive", "referer",
                                      "origin", "accept-encoding", "te", "trailer", "transfer-encoding", "upgrade", "via",
                                      "date", "expect", "dnt", "content-type", NULL};
    if (!name[0] || !strncasecmp(name, "proxy-", 6) || !strncasecmp(name, "sec-", 4)) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == ':' || *p == '\r' || *p == '\n' || *p == ' ') return 0;
    }
    for (int i = 0; forbidden[i]; i++) {
        if (same_text_nocase(name, forbidden[i])) return 0;
    }
    return 1;
}

/* Add "name: value" to a list of request headers (Content-Type goes to `type`). */
static void add_request_header(char *headers, size_t max, char *type, size_t type_max, JsStr *name, JsStr *value) {
    for (int i = 0; i < value->len; i++) {
        if (value->s[i] == '\r' || value->s[i] == '\n') return;
    }
    if (same_text_nocase(name->s, "content-type")) {
        if ((size_t)value->len < type_max) strcpy(type, value->s);
        return;
    }
    if (!header_allowed(name->s) || strlen(headers) + (size_t)name->len + (size_t)value->len + 5 >= max) return;
    strcat(headers, name->s);
    strcat(headers, ": ");
    strcat(headers, value->s);
    strcat(headers, "\r\n");
}

/* The headers of fetch(url, {headers}): an object, or a list of pairs. */
static int collect_headers(Value h, char *headers, size_t max, char *type, size_t type_max) {
    if (h.t != V_OBJ) return C_NORMAL;
    JsObj *o = h.u.o;
    if (o->cls == CL_ARRAY) {
        for (int i = 0; i < o->alen; i++) {
            Value pair = o->arr[i], k, v;
            if (pair.t != V_OBJ) continue;
            TRY(get_index(pair, v_num(0), &k));
            TRY(get_index(pair, v_num(1), &v));
            JsStr *ks, *vs;
            TRY(to_string(k, &ks));
            TRY(to_string(v, &vs));
            add_request_header(headers, max, type, type_max, ks, vs);
        }
        return C_NORMAL;
    }
    for (int i = 0; i < o->nprops; i++) {
        Prop *pr = &o->props[i];
        if (pr->flags & (P_HIDDEN | P_ACCESSOR)) continue;
        JsStr *vs;
        TRY(to_string(pr->v, &vs));
        add_request_header(headers, max, type, type_max, pr->key, vs);
    }
    return C_NORMAL;
}

/* The body a script sends, and its type: text, or a URLSearchParams form. */
static int request_body(Value b, JsStr **body, char *type, size_t type_max) {
    *body = NULL;
    if (is_nullish(b)) return C_NORMAL;
    if (b.t == V_OBJ && b.u.o->proto == SearchParamsProto) {
        if (!type[0]) strcpy(type, "application/x-www-form-urlencoded;charset=UTF-8");
    } else if (!type[0] && type_max > 30) {
        strcpy(type, "text/plain;charset=UTF-8");
    }
    return to_string(b, body);
}

static void method_name(Value m, char *method) {
    if (m.t != V_STR || m.u.s->len <= 0 || m.u.s->len >= 15) return;
    for (int i = 0; i <= m.u.s->len; i++) method[i] = m.u.s->s[i] >= 'a' && m.u.s->s[i] <= 'z' ? (char)(m.u.s->s[i] - 32) : m.u.s->s[i];
}

/* fetch(url, {method, body, headers}): done at once (the page waits), as a promise. */
NATIVE(window_fetch) {
    UNUSED;
    JsStr *u;
    Value target = ARG(0);
    if (target.t == V_OBJ) TRY(get_value(target, intern_c("url"), &target));
    TRY(to_string(target, &u));
    char url[2048];
    resolve_url(base_url, u->s, url, sizeof(url));
    JsObj *p = promise_new();
    *ret = v_obj(p);
    if (!fetch_allowed(url)) {
        promise_settle(p, 2, v_obj(make_error(TypeErrorProto, "Failed to fetch (a web page may not read local files)")));
        return C_NORMAL;
    }
    static char headers[4096];
    char type[128] = "", method[16] = "GET";
    headers[0] = 0;
    JsStr *body = NULL;
    if (ARG(1).t == V_OBJ) {
        Value m, b, h;
        TRY(get_value(ARG(1), intern_c("method"), &m));
        TRY(get_value(ARG(1), intern_c("body"), &b));
        TRY(get_value(ARG(1), intern_c("headers"), &h));
        method_name(m, method);
        TRY(collect_headers(h, headers, sizeof(headers), type, sizeof(type)));
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) TRY(request_body(b, &body, type, sizeof(type)));
    }
    Request rq;
    memset(&rq, 0, sizeof(rq));
    rq.method = method;
    rq.body = body ? body->s : NULL;
    rq.body_len = body ? (size_t)body->len : 0;
    rq.content_type = type[0] ? type : NULL;
    rq.headers = headers;
    Fetched f;
    char error[400];
    if (fetch_request(url, &rq, &f, error) != 0) {
        promise_settle(p, 2, v_obj(make_error(TypeErrorProto, "Failed to fetch")));
        return C_NORMAL;
    }
    promise_resolve(p, v_obj(response_new(&f, url, 0)));
    free(f.body);
    return C_NORMAL;
}

/* XMLHttpRequest: open() then send(); the request is made in send, and
   the handlers run from a timer, as if it had taken a moment. */
NATIVE(xhr_ctor) {
    UNUSED;
    JsObj *x = obj_new_raw(CL_OBJECT, XhrProto);
    prop_set_raw(x, K_readyState, v_num(0), 0);
    prop_set_raw(x, K_status, v_num(0), 0);
    prop_set_raw(x, K_responseText, v_str(S_empty), 0);
    /* (pages test for these to see what requests can do: cross-site with
       cookies, timeouts, typed responses) */
    prop_set_raw(x, intern_c("withCredentials"), v_bool(0), 0);
    prop_set_raw(x, intern_c("timeout"), v_num(0), 0);
    prop_set_raw(x, intern_c("responseType"), v_str(S_empty), 0);
    prop_set_raw(x, intern_c("upload"), v_obj(new_object()), 0);
    *ret = v_obj(x);
    return C_NORMAL;
}

NATIVE(xhr_fire) {
    UNUSED;
    /* self->target: the request */
    JsObj *x = self->target;
    /* (each runs both the on... handler and the listeners) */
    static const char *events[] = {"readystatechange", "load", "loadend"};
    for (int i = 0; i < 3; i++) {
        JsObj *e = new_event(events[i], 0, 0);
        if (dispatch_event(x, e) < 0) return C_THROW;
    }
    *ret = v_undef();
    return C_NORMAL;
}

/* magic: 0 open, 1 send, 2 setRequestHeader / abort (nothing), 3 getResponseHeader, 4 getAllResponseHeaders */
NATIVE(xhr_method) {
    UNUSED;
    if (this_v.t != V_OBJ) return throw_type("Illegal invocation", NULL, NULL);
    JsObj *x = this_v.u.o;
    int mode = self->magic & 0xFFFF;
    *ret = v_undef();
    if (mode == 2 && argc >= 2) { /* setRequestHeader(name, value) */
        JsStr *name, *value;
        TRY(to_string(ARG(0), &name));
        TRY(to_string(ARG(1), &value));
        Value list;
        TRY(get_value(this_v, intern_c("__headers"), &list));
        if (list.t != V_OBJ) {
            list = v_obj(new_array(0));
            prop_set_raw(x, intern_c("__headers"), list, P_HIDDEN);
        }
        JsObj *pair = new_array(0);
        arr_push(pair, v_str(name));
        arr_push(pair, v_str(value));
        arr_push(list.u.o, v_obj(pair));
        return C_NORMAL;
    }
    if (mode == 0) {
        prop_set_raw(x, intern_c("__method"), ARG(0), P_HIDDEN);
        prop_set_raw(x, intern_c("__url"), ARG(1), P_HIDDEN);
        prop_set_raw(x, intern_c("__headers"), v_undef(), P_HIDDEN);
        prop_set_raw(x, K_readyState, v_num(1), 0);
        return C_NORMAL;
    }
    if (mode == 3 || mode == 4) {
        *ret = mode == 3 ? v_null() : v_str(S_empty);
        return C_NORMAL;
    }
    if (mode != 1) return C_NORMAL;
    Value uv, mv;
    TRY(get_value(this_v, intern_c("__url"), &uv));
    TRY(get_value(this_v, intern_c("__method"), &mv));
    JsStr *u;
    TRY(to_string(uv, &u));
    char url[2048];
    resolve_url(base_url, u->s, url, sizeof(url));
    static char headers[4096];
    char type[128] = "", method[16] = "GET";
    headers[0] = 0;
    method_name(mv, method);
    Value hv;
    TRY(get_value(this_v, intern_c("__headers"), &hv));
    TRY(collect_headers(hv, headers, sizeof(headers), type, sizeof(type)));
    JsStr *body = NULL;
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) TRY(request_body(ARG(0), &body, type, sizeof(type)));
    Request rq;
    memset(&rq, 0, sizeof(rq));
    rq.method = method;
    rq.body = body ? body->s : NULL;
    rq.body_len = body ? (size_t)body->len : 0;
    rq.content_type = type[0] ? type : NULL;
    rq.headers = headers;
    Fetched f;
    char error[400];
    int failed = !fetch_allowed(url) || fetch_request(url, &rq, &f, error) != 0;
    prop_set_raw(x, K_readyState, v_num(4), 0);
    prop_set_raw(x, K_status, v_num(failed ? 0 : f.status), 0);
    prop_set_raw(x, K_statusText, v_str(intern_c(failed ? "" : "OK")), 0);
    prop_set_raw(x, K_responseURL, v_str(str_c(url)), 0);
    Value text = v_str(failed ? S_empty : str_new(f.body, (int)f.len));
    prop_set_raw(x, K_responseText, text, 0);
    Value rt;
    TRY(get_value(this_v, intern_c("responseType"), &rt));
    if (rt.t == V_STR && rt.u.s->len == 4 && !memcmp(rt.u.s->s, "json", 4)) {
        Value parsed;
        if (json_parse(self, v_undef(), &text, 1, &parsed) == C_NORMAL) prop_set_raw(x, K_response, parsed, 0);
        else prop_set_raw(x, K_response, v_null(), 0);
        js_exception = v_undef();
    } else {
        prop_set_raw(x, K_response, text, 0);
    }
    if (!failed) free(f.body);
    /* the handlers, a moment later */
    JsObj *fire = make_native(NULL, xhr_fire, 0, 0);
    fire->target = x;
    Value timer_args[2] = {v_obj(fire), v_num(0)};
    JsObj *st = make_native(NULL, set_timer, 2, 0);
    return set_timer(st, v_undef(), timer_args, 2, ret);
}

/* URLSearchParams over a query string: get, has, getAll, set, append, delete, toString, forEach */
static void parse_query(JsObj *list, const char *q) {
    if (*q == '?') q++;
    while (*q) {
        const char *amp = strchr(q, '&');
        int len = amp ? (int)(amp - q) : (int)strlen(q);
        if (len) {
            const char *eq = memchr_safe(q, len, '=') ? strchr(q, '=') : NULL;
            int kl = eq && eq < q + len ? (int)(eq - q) : len;
            Buf k = {0}, v = {0};
            for (int pass = 0; pass < 2; pass++) {
                const char *s = pass ? q + kl + 1 : q;
                int n = pass ? len - kl - 1 : kl;
                Buf *b = pass ? &v : &k;
                for (int i = 0; i < n; i++) {
                    if (s[i] == '+') buf_ch(b, ' ');
                    else if (s[i] == '%' && i + 2 < n && hex_digit(s[i + 1]) >= 0 && hex_digit(s[i + 2]) >= 0) {
                        buf_ch(b, (char)(hex_digit(s[i + 1]) * 16 + hex_digit(s[i + 2])));
                        i += 2;
                    } else {
                        buf_ch(b, s[i]);
                    }
                }
                if (!pass && kl >= len) break;
            }
            JsObj *pair = new_array(2);
            arr_push(pair, v_str(buf_done(&k)));
            arr_push(pair, v_str(buf_done(&v)));
            arr_push(list, v_obj(pair));
        }
        if (!amp) break;
        q = amp + 1;
    }
}

static JsObj *SearchParamsProto, *UrlProto;

NATIVE(search_params_ctor) {
    UNUSED;
    JsObj *o = obj_new_raw(CL_OBJECT, SearchParamsProto);
    JsObj *list = new_array(0);
    Value init = ARG(0);
    if (init.t == V_STR) parse_query(list, init.u.s->s);
    else if (init.t == V_OBJ) {
        JsObj *keys = new_array(0);
        enumerable_keys(init.u.o, keys, 1);
        for (int i = 0; i < keys->alen; i++) {
            Value v;
            TRY(get_value(init, interned(keys->arr[i].u.s), &v));
            JsStr *vs;
            TRY(to_string(v, &vs));
            JsObj *pair = new_array(2);
            arr_push(pair, keys->arr[i]);
            arr_push(pair, v_str(vs));
            arr_push(list, v_obj(pair));
        }
    }
    prop_set_raw(o, intern_c("__list"), v_obj(list), P_HIDDEN);
    *ret = v_obj(o);
    return C_NORMAL;
}

NATIVE(search_params_method) {
    UNUSED;
    Value lv;
    TRY(get_value(this_v, intern_c("__list"), &lv));
    if (lv.t != V_OBJ) return throw_type("Illegal invocation", NULL, NULL);
    JsObj *list = lv.u.o;
    int mode = self->magic & 0xFFFF; /* 0 get, 1 has, 2 getAll, 3 set, 4 append, 5 delete, 6 toString, 7 forEach */
    if (mode == 6) {
        Buf b = {0};
        for (int i = 0; i < list->alen; i++) {
            JsObj *pair = list->arr[i].u.o;
            if (i) buf_ch(&b, '&');
            for (int k = 0; k < 2; k++) {
                if (k) buf_ch(&b, '=');
                JsStr *s = pair->arr[k].u.s;
                for (int j = 0; j < s->len; j++) {
                    unsigned char c = (unsigned char)s->s[j];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.*", c)) buf_ch(&b, (char)c);
                    else if (c == ' ') buf_ch(&b, '+');
                    else {
                        char t[4] = {'%', "0123456789ABCDEF"[c >> 4], "0123456789ABCDEF"[c & 15], 0};
                        buf_c(&b, t);
                    }
                }
            }
        }
        *ret = v_str(buf_done(&b));
        return C_NORMAL;
    }
    if (mode == 7) {
        if (!is_callable(ARG(0))) return throw_type("callback is not a function", NULL, NULL);
        for (int i = 0; i < list->alen; i++) {
            JsObj *pair = list->arr[i].u.o;
            Value cb[2] = {pair->arr[1], pair->arr[0]}, r;
            TRY(call_function(ARG(0).u.o, v_undef(), cb, 2, &r));
        }
        *ret = v_undef();
        return C_NORMAL;
    }
    JsStr *name;
    TRY(arg_string(args, argc, 0, &name));
    JsObj *all = new_array(0);
    *ret = mode == 0 ? v_null() : mode == 1 ? v_bool(0) : v_undef();
    for (int i = 0; i < list->alen; i++) {
        JsObj *pair = list->arr[i].u.o;
        if (!str_eq(pair->arr[0].u.s, name)) continue;
        if (mode == 0) {
            *ret = pair->arr[1];
            return C_NORMAL;
        }
        if (mode == 1) {
            *ret = v_bool(1);
            return C_NORMAL;
        }
        if (mode == 2) arr_push(all, pair->arr[1]);
        if (mode == 3 || mode == 5) {
            memmove(list->arr + i, list->arr + i + 1, sizeof(Value) * (size_t)(list->alen - i - 1));
            list->alen--;
            i--;
        }
    }
    if (mode == 2) *ret = v_obj(all);
    if (mode == 3 || mode == 4) {
        JsStr *v;
        TRY(arg_string(args, argc, 1, &v));
        JsObj *pair = new_array(2);
        arr_push(pair, v_str(name));
        arr_push(pair, v_str(v));
        arr_push(list, v_obj(pair));
    }
    return C_NORMAL;
}

NATIVE(url_ctor) {
    UNUSED;
    JsStr *u, *base = NULL;
    TRY(arg_string(args, argc, 0, &u));
    if (argc > 1 && !is_nullish(args[1])) TRY(to_string(args[1], &base));
    char full[2048];
    if (!strstr(u->s, "://") && !base && strncasecmp(u->s, "data:", 5) && strncasecmp(u->s, "blob:", 5) && strncasecmp(u->s, "mailto:", 7))
        return throw_type("Failed to construct 'URL': Invalid URL", NULL, NULL);
    resolve_url(base ? base->s : current_url, u->s, full, sizeof(full));
    /* keep a #fragment from the input */
    const char *hash = strchr(u->s, '#');
    if (hash && !strchr(full, '#')) strncat_safe(full, hash, sizeof(full) - strlen(full) - 1);
    JsObj *o = obj_new_raw(CL_OBJECT, UrlProto);
    set_url_parts(o, full);
    Value search;
    TRY(get_value(v_obj(o), intern_c("search"), &search));
    Value sp;
    TRY(search_params_ctor(self, v_undef(), &search, 1, &sp));
    prop_set_raw(o, intern_c("searchParams"), sp, 0);
    *ret = v_obj(o);
    return C_NORMAL;
}

NATIVE(url_to_string) {
    UNUSED;
    return get_value(this_v, K_href, ret);
}

/* Observers: IntersectionObserver reports everything as visible at once
   (pages use it to load pictures lazily); the others do nothing. */
NATIVE(observer_ctor) {
    UNUSED;
    JsObj *o = new_object();
    if (native_new_target) { /* an instance of what `new` was given (so it has observe, disconnect ...) */
        Value proto;
        TRY(get_value(v_obj(native_new_target), S_prototype, &proto));
        if (proto.t == V_OBJ) o->proto = proto.u.o;
    }
    prop_set_raw(o, intern_c("__callback"), ARG(0), P_HIDDEN);
    prop_set_raw(o, intern_c("__kind"), v_num(self->magic & 0xFFFF), P_HIDDEN);
    *ret = v_obj(o);
    return C_NORMAL;
}

NATIVE(observer_observe) {
    UNUSED;
    Value kind, cb;
    TRY(get_value(this_v, intern_c("__kind"), &kind));
    TRY(get_value(this_v, intern_c("__callback"), &cb));
    *ret = v_undef();
    if (kind.t != V_NUM || kind.u.n != 1 || !is_callable(cb) || !node_of(ARG(0))) return C_NORMAL;
    /* one entry, "intersecting", delivered from a timer */
    JsObj *entry = new_object();
    prop_set_raw(entry, K_target, ARG(0), 0);
    prop_set_raw(entry, intern_c("isIntersecting"), v_bool(1), 0);
    prop_set_raw(entry, intern_c("intersectionRatio"), v_num(1), 0);
    JsObj *rect = new_object();
    make_rect(rect, 0, 0, 0, 0);
    prop_set_raw(entry, intern_c("boundingClientRect"), v_obj(rect), 0);
    JsObj *entries = new_array(1);
    arr_push(entries, v_obj(entry));
    Value targs[4] = {cb, v_num(0), v_obj(entries), this_v};
    JsObj *st = make_native(NULL, set_timer, 2, 0);
    Value id;
    return set_timer(st, v_undef(), targs, 4, &id);
}

/* ---------------------------------------------------------------- scripts in the page */

static int script_runs(Node *n) {
    char type[64] = "";
    if (!attr(n, "type", type, sizeof(type)) || !type[0]) return 1;
    return !strcasecmp_word(type, "text/javascript") || !strcasecmp_word(type, "application/javascript") ||
           !strcasecmp_word(type, "text/ecmascript") || !strcasecmp_word(type, "application/x-javascript") ||
           !strcasecmp_word(type, "javascript");
}

#define SCRIPT_DONE 3 /* img_state of a <script> that has run */

/* What a page's scripts may use while it loads; beyond it, the rest are skipped. */
#define PAGE_SCRIPT_MS 25000
#define PAGE_SCRIPT_BYTES (24 * 1024 * 1024)
#define SCRIPT_TREE_FACTOR 4             /* parsed (function bodies wait until called), script takes about this many times its size */
#define MEMORY_RESERVE (48ull << 20)     /* kept free for laying out and drawing the page, and the stack */
static u64 page_script_start;
static size_t page_script_bytes;
static int page_scripts_skipped;

/* A page's scripts, fetched in parallel (then run in order). */
static PrefetchSet *script_prefetch;

static void prefetch_scripts(void) {
    int count = 0;
    char src[2048], url[2048];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type == NODE_ELEMENT && is(n->tag, "script") && script_runs(n) && attr(n, "src", src, sizeof(src)) && src[0]) count++;
    }
    if (count < 2) return;
    script_prefetch = prefetch_begin(count);
    if (!script_prefetch) return;
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || !is(n->tag, "script") || !script_runs(n) || !attr(n, "src", src, sizeof(src)) || !src[0]) continue;
        resolve_url(base_url, src, url, sizeof(url));
        if (fetch_allowed(url)) prefetch_add(script_prefetch, url);
    }
    prefetch_go(script_prefetch);
}

static int script_budget_left(void) {
    return uptime_ms() - page_script_start < PAGE_SCRIPT_MS && page_script_bytes < PAGE_SCRIPT_BYTES && !js_oom;
}

static int memory_low; /* a script was skipped: not enough memory to run it */

/* BROWSER_TIMING: where the memory is */
static void memory_report(const char *when) {
    size_t objects = 0, strings = 0, envs = 0, n_obj = 0, props = 0, arrays = 0;
    for (Gc *g = heap_list; g; g = g->next) {
        size_t sz = (size_t)1 << 5;
        while (sz < g->size + 16) sz <<= 1; /* (the allocator's block) */
        if (g->kind == K_OBJ) {
            JsObj *o = (JsObj *)g;
            objects += sz;
            n_obj++;
            if (o->pcap) {
                size_t p = 32;
                while (p < sizeof(Prop) * (size_t)o->pcap + 16) p <<= 1;
                props += p;
            }
            if (o->arr) arrays += sizeof(Value) * (size_t)o->alen;
        } else if (g->kind == K_STR) {
            strings += sz;
        } else {
            envs += sz;
        }
    }
    printf("[memory] %s: heap %d KiB (%d KiB free blocks); objects %d KiB (%d of them) + their properties %d KiB + array items %d KiB, "
           "strings %d KiB, scopes %d KiB, trees %d KiB, kept source %d KiB\n",
           when, (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024), (int)(heap_free_bytes() / 1024), (int)(objects / 1024), (int)n_obj,
           (int)(props / 1024), (int)(arrays / 1024), (int)(strings / 1024), (int)(envs / 1024), arena_chunks * 64, (int)(kept_bytes / 1024));
}
static u64 page_script_fetch_ms;
static int page_script_fetches;

/* Is there memory to run a script of `len` bytes, with some to spare? */
static int memory_for_script(size_t len) {
    sys_info si;
    if (sysinfo(&si) != 0 || si.mem_total <= si.mem_used) return 1; /* (unknown: try) */
    u64 free_bytes = si.mem_total - si.mem_used + heap_free_bytes() / 2; /* (half: free blocks are of fixed sizes) */
    if (free_bytes >= (u64)len * SCRIPT_TREE_FACTOR + MEMORY_RESERVE) return 1;
    js_gc_during(); /* short: collect garbage first (safe even with scripts on the stack), then look again */
    if (sysinfo(&si) == 0 && si.mem_total > si.mem_used) {
        free_bytes = si.mem_total - si.mem_used + heap_free_bytes() / 2;
        if (free_bytes >= (u64)len * SCRIPT_TREE_FACTOR + MEMORY_RESERVE) return 1;
    }
    if (!memory_low && getenv("BROWSER_TIMING")) {
        size_t in_free_lists = heap_free_bytes();
        printf("[memory] skipping: %d KiB free; heap %d KiB (%d KiB of it free blocks), trees %d KiB, kept source %d KiB, js heap %d objects\n",
               (int)(free_bytes / 1024), (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024), (int)(in_free_lists / 1024),
               arena_chunks * 64, (int)(kept_bytes / 1024), (int)heap_live);
    }
    memory_low = 1;
    return 0;
}

static void run_script_element(Node *n) {
    if (!js_enabled || n->img_state == SCRIPT_DONE || !script_runs(n)) return;
    n->img_state = SCRIPT_DONE;
    if (!script_budget_left()) {
        page_scripts_skipped++;
        return;
    }
    Node *saved = current_script;
    current_script = n;
    char src[2048];
    if (attr(n, "src", src, sizeof(src)) && src[0]) {
        char url[2048], error[400];
        resolve_url(base_url, src, url, sizeof(url));
        Fetched f;
        u64 fetch_t0 = uptime_ms();
        int taken = prefetch_take(script_prefetch, url, &f);
        /* (fetched in the background, or now; one that failed there is tried again here) */
        error[0] = 0;
        int fetched_ok = taken == 1 || (taken != 1 && fetch_allowed(url) && fetch_url(url, NULL, &f, error) == 0);
        page_script_fetch_ms += uptime_ms() - fetch_t0;
        page_script_fetches++;
        int ran = 0;
        if (fetched_ok) {
            page_script_bytes += f.len;
            if (f.status >= 400) {
                char msg[600] = "could not load a script (HTTP error) from ";
                strncat_safe(msg, url, 500);
                js_report(msg);
            } else if (!memory_for_script(f.len)) {
                page_scripts_skipped++;
            } else {
                ran = 1;
                if (js_run_script(f.body, (int)f.len, url) != 0) js_errors++;
            }
            free(f.body);
        } else {
            char msg[1000] = "could not load a script from ";
            strncat_safe(msg, url, 500);
            if (error[0]) {
                strncat_safe(msg, ": ", 3);
                strncat_safe(msg, error, 400);
            }
            js_report(msg);
            js_errors++;
        }
        JsObj *e = new_event(ran ? "load" : "error", 0, 0);
        dispatch_event(wrap(n), e);
    } else if (n->first && n->first->type == NODE_TEXT) {
        page_script_bytes += (size_t)n->first->text_len;
        if (!memory_for_script((size_t)n->first->text_len)) page_scripts_skipped++;
        else if (js_run_script(n->first->text, n->first->text_len, "inline") != 0) js_errors++;
    }
    current_script = saved;
    run_microtasks();
}

/* Scripts put into the page by other scripts run when inserted. The
   scripts are listed first (and given wrappers, so that nothing frees them
   while we hold them): a script may change the page around it. */
static void run_inserted_scripts(Node *root) {
    if (!contains_node(document, root)) return;
    int count = 0;
    for (Node *d = root; d; d = next_in_tree(d, root)) count += d->type == NODE_ELEMENT && is(d->tag, "script") && d->img_state != SCRIPT_DONE;
    if (!count) return;
    Node **list = malloc(sizeof(Node *) * (size_t)count);
    if (!list) return;
    int k = 0;
    for (Node *d = root; d && k < count; d = next_in_tree(d, root)) {
        if (d->type == NODE_ELEMENT && is(d->tag, "script") && d->img_state != SCRIPT_DONE) {
            wrap(d);
            list[k++] = d;
        }
    }
    for (int i = 0; i < k; i++) {
        if (contains_node(document, list[i])) run_script_element(list[i]);
    }
    free(list);
}

/* ---------------------------------------------------------------- the garbage collector */

static Gc **gc_stack;
static int gc_top, gc_cap;

static int gc_overflow; /* the mark stack could not grow: this collection frees nothing */

static void gc_push(Gc *g) {
    if (!g || g->mark || g->perm) return;
    g->mark = 1;
    if (gc_top == gc_cap) {
        int cap = gc_cap ? gc_cap * 2 : 4096;
        Gc **s = realloc(gc_stack, sizeof(Gc *) * (size_t)cap);
        if (!s) {
            gc_overflow = 1;
            return;
        }
        gc_stack = s;
        gc_cap = cap;
    }
    gc_stack[gc_top++] = g;
}

static void gc_value(Value v) {
    if (v.t == V_STR || v.t == V_BIG) gc_push(&v.u.s->gc);
    else if (v.t == V_OBJ) gc_push(&v.u.o->gc);
}

static void coro_free(void *p); /* (js_coro.c) */
static void coro_gc_mark(void *p);
static int coro_any(void);
static int gc_index_fresh; /* gc_index lists the heap as it is now (set while collecting) */

static void gc_children(Gc *g) {
    if (g->kind == K_OBJ) {
        JsObj *o = (JsObj *)g;
        if (o->proto) gc_push(&o->proto->gc);
        for (int i = 0; i < o->nprops; i++) {
            gc_value(o->props[i].v);
            if (o->props[i].get) gc_push(&o->props[i].get->gc);
            if (o->props[i].set) gc_push(&o->props[i].set->gc);
        }
        for (int i = 0; i < o->alen; i++) gc_value(o->arr[i]);
        if (o->scope) gc_push(&o->scope->gc);
        gc_value(o->this_val);
        if (o->target) gc_push(&o->target->gc);
        if (o->home) gc_push(&o->home->gc);
        if (o->cls != CL_BUFFER) /* (a buffer's list of views is weak) */
            for (int i = 0; i < o->nbound; i++) gc_value(o->bound[i]);
        if (o->cls == CL_GENERATOR && o->extra) coro_gc_mark(o->extra); /* (its stack too; magic 2: an await's hold on it) */
        if ((o->cls == CL_MAP || o->cls == CL_SET) && o->extra) {
            Value *e = o->extra;
            for (int i = 0; i < 2 * o->extra_len; i++) gc_value(e[i]);
        }
    } else if (g->kind == K_ENV) {
        Env *e = (Env *)g;
        if (e->parent) gc_push(&e->parent->gc);
        if (e->object) gc_push(&e->object->gc);
        for (int i = 0; i < e->n; i++) gc_value(e->vals[i]);
    }
}

static void gc_free(Gc *g) {
    if (g->kind == K_OBJ) {
        JsObj *o = (JsObj *)g;
        free(o->props);
        free(o->hidx);
        free(o->arr);
        free(o->bound);
        if (o->cls == CL_REGEXP) regex_free(o->extra);
        else if (o->cls == CL_MAP || o->cls == CL_SET || o->cls == CL_BUFFER) free(o->extra);
        else if (o->cls == CL_GENERATOR && o->magic != 2) coro_free(o->extra); /* (magic 2: an await's holder, not the owner) */
        if (o->node && o->cls == CL_NODE && o->node->js == o) o->node->js = NULL;
    } else if (g->kind == K_ENV) {
        Env *e = (Env *)g;
        free(e->names);
        free(e->vals);
        free(e->consts);
    }
    free(g);
}

static void mark_nodes(Node *root) {
    for (Node *d = root; d; d = next_in_tree(d, root)) {
        if (d->js) gc_push(&((JsObj *)d->js)->gc);
    }
}

/* Mark from the roots: the global scope, prototypes, timers, the page's nodes. */
static void coro_gc_roots(int scan_stacks); /* (js_coro.c) */
static void gc_scan_range(const char *lo, const char *hi);
static int gc_build_index(void);
static char *js_stack_top;

static void gc_mark_roots(void) {
    gc_top = 0;
    gc_overflow = 0;
    gc_push(&global_obj->gc);
    gc_push(&global_env->gc);
    gc_push(&global_lex->gc);
    JsObj *roots[] = {ObjectProto, FunctionProto, ArrayProto, StringProto, NumberProto, BooleanProto, ErrorProto, TypeErrorProto,
                      RangeErrorProto, SyntaxErrorProto, ReferenceErrorProto, DateProto, RegExpProto, MapProto, SetProto,
                      PromiseProto, NodeProto, EventProto, DocumentProto, StyleProto, ClassListProto, DatasetProto, StorageProto,
                      XhrProto, microtasks, location_obj, local_storage, session_storage, SearchParamsProto, UrlProto,
                      TypedArrayProto, IteratorProto, SymbolProto, ArrayBufferProto, BigIntProto, DataViewProto, TextEncoderProto, TextDecoderProto,
                      GeneratorProto, AsyncGeneratorProto};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) gc_push(&roots[i]->gc);
    }
    for (int i = 0; i < 10; i++)
        if (TypedProtos[i]) gc_push(&TypedProtos[i]->gc);
    frames_js_mark();
    JsObj *dom_roots[] = {ElementProto, HTMLElementProto, SVGElementProto, CharacterDataProto, TextProto, CommentProto, FragmentProto,
                          ShadowRootProto, EventTargetProto};
    for (size_t i = 0; i < sizeof(dom_roots) / sizeof(dom_roots[0]); i++)
        if (dom_roots[i]) gc_push(&dom_roots[i]->gc);
    for (int i = 0; i < IFACE_COUNT; i++)
        if (tag_protos[i]) gc_push(&tag_protos[i]->gc);
    gc_value(js_exception);
    for (int i = 0; i < timer_count; i++) {
        gc_value(timers[i].fn);
        if (timers[i].args) gc_push(&timers[i].args->gc);
    }
    if (document) mark_nodes(document);
    for (int i = 0; i < orphan_count; i++) mark_nodes(orphans[i]);
    if (native_new_target) gc_push(&native_new_target->gc);
}

/* Trace from what is marked, then free what is not. */
static void gc_finish(void) {
    while (gc_top > 0) gc_children(gc_stack[--gc_top]);
    if (buffers_made && !gc_overflow) { /* buffers forget their views that are dying */
        for (Gc *g = heap_list; g; g = g->next) {
            JsObj *b = (JsObj *)g;
            if (g->kind != K_OBJ || b->cls != CL_BUFFER || !g->mark) continue;
            int n = 0;
            for (int i = 0; i < b->nbound; i++)
                if (b->bound[i].u.o->gc.mark || b->bound[i].u.o->gc.perm) b->bound[n++] = b->bound[i];
            b->nbound = n;
        }
    }
    size_t live = 0;
    Gc **link = &heap_list;
    while (*link) {
        Gc *g = *link;
        if (g->mark || gc_overflow) {
            g->mark = 0;
            link = &g->next;
            live++;
        } else {
            *link = g->next;
            gc_free(g);
        }
    }
    heap_live = live;
    heap_since_gc = 0;
    gc_index_fresh = 0;
}

/* Collect garbage. Only call when no script is running. */
static void js_gc(void) {
    if (!global_obj) return;
    /* stopped generators and async functions hold values on their stacks:
       scanning them needs the index (no index, no collecting) */
    if (coro_any() && !gc_build_index()) return;
    gc_mark_roots();
    coro_gc_roots(1);
    gc_finish();
}

/* ---- collecting while a script runs ----
   Values the running code holds are on the C stack (and in registers):
   any word there that points into a heap block keeps that block alive
   (conservatively). */

/* (js_stack_top: the top of the main stack, set by main) */
static int gc_running;
static Gc **gc_index;        /* every heap block, by address */
static int gc_index_n, gc_index_cap;

static void gc_sift(Gc **a, int n, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && a[l] > a[m]) m = l;
        if (r < n && a[r] > a[m]) m = r;
        if (m == i) return;
        Gc *t = a[i];
        a[i] = a[m];
        a[m] = t;
        i = m;
    }
}

static int gc_build_index(void) {
    int n = 0;
    for (Gc *g = heap_list; g; g = g->next) n++;
    if (n > gc_index_cap) {
        Gc **p = realloc(gc_index, sizeof(Gc *) * (size_t)(n + 1024));
        if (!p) return 0;
        gc_index = p;
        gc_index_cap = n + 1024;
    }
    n = 0;
    for (Gc *g = heap_list; g; g = g->next) gc_index[n++] = g;
    for (int i = n / 2 - 1; i >= 0; i--) gc_sift(gc_index, n, i); /* heap sort */
    for (int end = n - 1; end > 0; end--) {
        Gc *t = gc_index[0];
        gc_index[0] = gc_index[end];
        gc_index[end] = t;
        gc_sift(gc_index, end, 0);
    }
    gc_index_n = n;
    gc_index_fresh = 1;
    return 1;
}

static void gc_scan_range(const char *lo, const char *hi) {
    if (!gc_index_n) return;
    unsigned long long first = (unsigned long long)gc_index[0];
    unsigned long long last = (unsigned long long)gc_index[gc_index_n - 1] + gc_index[gc_index_n - 1]->size;
    for (const char *p = (const char *)(((unsigned long long)lo + 7) & ~7ull); p + 8 <= hi; p += 8) {
        unsigned long long w = *(const unsigned long long *)p;
        if (w < first || w >= last) continue;
        int a = 0, b = gc_index_n - 1;
        while (a < b) { /* the last block starting at or below w */
            int m = (a + b + 1) / 2;
            if ((unsigned long long)gc_index[m] <= w) a = m;
            else b = m - 1;
        }
        Gc *g = gc_index[a];
        if (w >= (unsigned long long)g && w < (unsigned long long)g + g->size) gc_push(g);
    }
}

static char *current_coro_top(void); /* (js_coro.c) */

__attribute__((noinline)) static void js_gc_during(void) {
    if (gc_running || !js_stack_top || !global_obj) {
        heap_since_gc = 0; /* (not now; ask again after more allocation) */
        return;
    }
    gc_running = 1;
    /* the registers that may hold pointers, onto the stack */
    unsigned long long regs[8 + 20] __attribute__((aligned(16)));
    __asm__ volatile("mov %%rbx, 0(%0)\n\tmov %%rbp, 8(%0)\n\tmov %%rsi, 16(%0)\n\tmov %%rdi, 24(%0)\n\t"
                     "mov %%r12, 32(%0)\n\tmov %%r13, 40(%0)\n\tmov %%r14, 48(%0)\n\tmov %%r15, 56(%0)\n\t"
                     "movdqu %%xmm6, 64(%0)\n\tmovdqu %%xmm7, 80(%0)\n\tmovdqu %%xmm8, 96(%0)\n\tmovdqu %%xmm9, 112(%0)\n\t"
                     "movdqu %%xmm10, 128(%0)\n\tmovdqu %%xmm11, 144(%0)\n\tmovdqu %%xmm12, 160(%0)\n\t"
                     "movdqu %%xmm13, 176(%0)\n\tmovdqu %%xmm14, 192(%0)\n\tmovdqu %%xmm15, 208(%0)"
                     :
                     : "r"(regs)
                     : "memory");
    if (gc_build_index()) {
        gc_mark_roots();
        gc_scan_range((const char *)regs, current_coro_top()); /* the stack this runs on */
        coro_gc_roots(1);
        for (int i = 0; i < gc_extra_n; i++) gc_scan_range(gc_extra_lo[i], gc_extra_hi[i]);
        gc_finish();
        if (getenv("BROWSER_TIMING")) printf("[gc] while a script runs: %d blocks alive\n", (int)heap_live);
    } else {
        heap_since_gc = 0;
    }
    gc_running = 0;
}

static int gc_runs;

static void js_maybe_gc(void) {
    if (heap_since_gc > 8 * 1024 * 1024) {
        js_gc();
        gc_runs++;
        if (getenv("BROWSER_TIMING")) printf("[gc] collection %d: %d objects alive\n", gc_runs, (int)heap_live);
    }
}

/* ---------------------------------------------------------------- the page's life */

static void define_dom_methods(void) {
    define_method(NodeProto, "getElementById", dom_get_element_by_id, 1, 0);
    define_method(NodeProto, "querySelector", dom_query, 1, 0);
    define_method(NodeProto, "querySelectorAll", dom_query, 1, 1);
    define_method(NodeProto, "getElementsByTagName", dom_get_elements, 1, 0);
    define_method(NodeProto, "getElementsByClassName", dom_get_elements, 1, 1);
    define_method(NodeProto, "getElementsByName", dom_get_elements, 1, 2);
    define_method(NodeProto, "appendChild", dom_child_op, 1, 0);
    define_method(NodeProto, "insertBefore", dom_child_op, 2, 1);
    define_method(NodeProto, "removeChild", dom_child_op, 1, 2);
    define_method(NodeProto, "replaceChild", dom_child_op, 2, 3);
    define_method(NodeProto, "append", dom_insert_many, 1, 0);
    define_method(NodeProto, "prepend", dom_insert_many, 1, 1);
    define_method(NodeProto, "before", dom_insert_many, 1, 2);
    define_method(NodeProto, "after", dom_insert_many, 1, 3);
    define_method(NodeProto, "replaceWith", dom_insert_many, 1, 4);
    define_method(NodeProto, "replaceChildren", dom_insert_many, 1, 5);
    define_method(NodeProto, "remove", dom_remove, 0, 0);
    define_method(NodeProto, "cloneNode", dom_clone, 1, 0);
    define_method(NodeProto, "contains", dom_contains, 1, 0);
    define_method(NodeProto, "compareDocumentPosition", dom_compare_position, 1, 0);
    /* the window's event handler properties (pages test for them: "onsubmit" in window).
       Not the touch ones: those would say this is a phone. */
    static const char *window_events[] = {"onload", "onunload", "onbeforeunload", "onerror", "onresize", "onscroll", "onclick",
                                          "ondblclick", "onsubmit", "onreset", "onchange", "oninput", "onfocus", "onblur",
                                          "onkeydown", "onkeyup", "onkeypress", "onmousedown", "onmouseup", "onmousemove",
                                          "onmouseover", "onmouseout", "onmouseenter", "onmouseleave", "onwheel", "onhashchange",
                                          "onpopstate", "onmessage", "onstorage", "onpageshow", "onpagehide", "oncontextmenu",
                                          "onanimationend", "ontransitionend", "onselect", "onabort", "onpointerdown",
                                          "onpointerup", "onpointermove", NULL};
    for (int i = 0; window_events[i]; i++) {
        if (!own_prop(global_obj, intern_c(window_events[i]))) define_hidden(global_obj, window_events[i], v_null());
    }
    static const char *positions[] = {"DOCUMENT_POSITION_DISCONNECTED", "DOCUMENT_POSITION_PRECEDING", "DOCUMENT_POSITION_FOLLOWING",
                                      "DOCUMENT_POSITION_CONTAINS", "DOCUMENT_POSITION_CONTAINED_BY", "DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC"};
    for (int i = 0; i < 6; i++) define_hidden(NodeProto, positions[i], v_num(1 << i));
    define_method(NodeProto, "matches", dom_matches, 1, 0);
    define_method(NodeProto, "webkitMatchesSelector", dom_matches, 1, 0);
    define_method(NodeProto, "closest", dom_matches, 1, 1);
    define_method(NodeProto, "getAttribute", dom_attribute, 1, 0);
    define_method(NodeProto, "setAttribute", dom_attribute, 2, 1);
    define_method(NodeProto, "removeAttribute", dom_attribute, 1, 2);
    define_method(NodeProto, "hasAttribute", dom_attribute, 1, 3);
    define_method(NodeProto, "toggleAttribute", dom_attribute, 1, 4);
    define_method(NodeProto, "getAttributeNames", dom_attribute, 0, 5);
    define_method(NodeProto, "insertAdjacentHTML", dom_insert_adjacent, 2, 0);
    define_method(NodeProto, "insertAdjacentElement", dom_insert_adjacent, 2, 1);
    define_method(NodeProto, "insertAdjacentText", dom_insert_adjacent, 2, 2);
    define_method(NodeProto, "getBoundingClientRect", dom_rect, 0, 0);
    define_method(NodeProto, "getClientRects", dom_rect, 0, 1);
    define_method(NodeProto, "focus", dom_focus, 0, 0);
    define_method(NodeProto, "blur", dom_focus, 0, 1);
    define_method(NodeProto, "scrollIntoView", dom_focus, 0, 2);
    define_method(NodeProto, "select", dom_focus, 0, 3);
    define_method(NodeProto, "click", dom_click, 0, 0);
    define_method(NodeProto, "hasChildNodes", dom_has_child_nodes, 0, 0);
    define_method(NodeProto, "addEventListener", event_listener, 2, 0);
    define_method(NodeProto, "removeEventListener", event_listener, 2, 1);
    define_method(NodeProto, "dispatchEvent", dispatch_event_native, 1, 0);
    define_method(NodeProto, "createElement", dom_create_element, 1, 0);
    define_method(NodeProto, "createElementNS", dom_create_element, 2, 4);
    define_method(NodeProto, "createTextNode", dom_create_element, 1, 1);
    define_method(NodeProto, "createDocumentFragment", dom_create_element, 0, 2);
    define_method(NodeProto, "createComment", dom_create_element, 1, 3);
    define_method(NodeProto, "write", document_write, 1, 0);
    define_method(NodeProto, "writeln", document_write, 1, 1);
    define_method(NodeProto, "submit", noop, 0, 0);
    define_method(NodeProto, "reset", noop, 0, 0);
    define_method(NodeProto, "attachShadow", noop, 1, 0);
    define_method(NodeProto, "play", noop, 0, 0);
    define_method(NodeProto, "pause", noop, 0, 0);
    define_method(NodeProto, "hasFocus", noop, 0, 0);
    define_method(StyleProto, "setProperty", style_method, 2, 0);
    define_method(StyleProto, "getPropertyValue", style_method, 1, 1);
    define_method(StyleProto, "removeProperty", style_method, 1, 2);
    static const char *cl[] = {"add", "remove", "toggle", "contains", "replace", "item", "forEach"};
    for (int i = 0; i < 7; i++) define_method(ClassListProto, cl[i], classlist_method, 1, i);
    static const char *st[] = {"getItem", "setItem", "removeItem", "clear", "key"};
    for (int i = 0; i < 5; i++) define_method(StorageProto, st[i], storage_method, 1, i);
    define_method(EventProto, "preventDefault", event_method, 0, 0);
    define_method(EventProto, "stopPropagation", event_method, 0, 1);
    define_method(EventProto, "stopImmediatePropagation", event_method, 0, 2);
    define_method(EventProto, "composedPath", noop, 0, 0);
    static const char *xm[] = {"open", "send", "setRequestHeader", "getResponseHeader", "getAllResponseHeaders", "abort", "overrideMimeType"};
    static const int xmagic[] = {0, 1, 2, 3, 4, 2, 2};
    for (int i = 0; i < 7; i++) define_method(XhrProto, xm[i], xhr_method, 1, xmagic[i]);
    define_method(XhrProto, "addEventListener", event_listener, 2, 0);
    define_method(XhrProto, "removeEventListener", event_listener, 2, 1);
    static const char *sp[] = {"get", "has", "getAll", "set", "append", "delete", "toString", "forEach"};
    for (int i = 0; i < 8; i++) define_method(SearchParamsProto, sp[i], search_params_method, 1, i);
    define_method(UrlProto, "toString", url_to_string, 0, 0);
    define_method(UrlProto, "toJSON", url_to_string, 0, 0);
}

/* A constructor-like function for instanceof checks (HTMLElement, Node...). */
static void dom_interface(const char *name, JsObj *proto) {
    JsObj *f = make_native(name, noop, 0, 0);
    f->fn_kind = FN_NORMAL;
    prop_set_raw(f, S_prototype, v_obj(proto), P_HIDDEN);
    if (proto != global_obj && !own_prop(proto, S_constructor)) define_hidden(proto, "constructor", v_obj(f));
    define_hidden(global_obj, name, v_obj(f));
}

/* Set up the scripting world for the page now in `document`. */
/* crypto.randomUUID(): a random (version 4) UUID */
NATIVE(random_uuid) {
    UNUSED;
    unsigned char b[16];
    getrandom(b, sizeof(b));
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    static const char hex[] = "0123456789abcdef";
    char text[37];
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[k++] = '-';
        text[k++] = hex[b[i] >> 4];
        text[k++] = hex[b[i] & 15];
    }
    text[k] = 0;
    *ret = v_str(str_c(text));
    return C_NORMAL;
}

static void js_page_start(void) {
    js_init_builtins();
    init_dom_names();
    NodeProto = obj_new_raw(CL_OBJECT, ObjectProto);
    DocumentProto = obj_new_raw(CL_OBJECT, NodeProto);
    EventProto = obj_new_raw(CL_OBJECT, ObjectProto);
    StyleProto = obj_new_raw(CL_OBJECT, ObjectProto);
    ClassListProto = obj_new_raw(CL_OBJECT, ObjectProto);
    DatasetProto = obj_new_raw(CL_OBJECT, ObjectProto);
    StorageProto = obj_new_raw(CL_OBJECT, ObjectProto);
    XhrProto = obj_new_raw(CL_OBJECT, ObjectProto);
    SearchParamsProto = obj_new_raw(CL_OBJECT, ObjectProto);
    UrlProto = obj_new_raw(CL_OBJECT, ObjectProto);
    define_dom_methods();
    JsObj *w = global_obj;
    define_hidden(w, "window", v_obj(w));
    define_hidden(w, "self", v_obj(w));
    define_hidden(w, "top", v_obj(w));
    define_hidden(w, "parent", v_obj(w));
    define_hidden(w, "frames", v_obj(w));
    define_hidden(w, "document", wrap_v(document));
    define_hidden(w, "name", v_str(S_empty));
    define_hidden(w, "closed", v_bool(0));
    define_hidden(w, "isSecureContext", v_bool(!strncasecmp(current_url, "https://", 8)));
    define_hidden(w, "origin", v_str(S_empty));
    define_hidden(w, "innerWidth", v_num(PAGE_VIEW_W));
    define_hidden(w, "innerHeight", v_num(PAGE_VIEW_H));
    define_hidden(w, "outerWidth", v_num(PAGE_VIEW_W));
    define_hidden(w, "outerHeight", v_num(PAGE_VIEW_H + 54));
    define_hidden(w, "devicePixelRatio", v_num(1));
    define_hidden(w, "scrollX", v_num(0));
    define_hidden(w, "pageXOffset", v_num(0));
    define_hidden(w, "onload", v_null());
    location_obj = new_object();
    set_url_parts(location_obj, current_url);
    define_getter(location_obj, "href", location_href_get, location_href_set, 0);
    define_method(location_obj, "assign", location_method, 1, 0);
    define_method(location_obj, "replace", location_method, 1, 0);
    define_method(location_obj, "reload", location_method, 0, 1);
    define_method(location_obj, "toString", location_method, 0, 2);
    define_getter(w, "location", window_location_get, window_location_set, 0);
    JsObj *nav = new_object();
    define_hidden(w, "navigator", v_obj(nav));
    Prop *doc_prop = own_prop(w, intern_c("document"));
    missing_document = doc_prop && doc_prop->v.t == V_OBJ ? doc_prop->v.u.o : NULL;
    prop_set_raw(nav, intern_c("userAgent"), v_str(intern_c("Mozilla/5.0 (RDX; x86_64) RDX-Browser/0.4")), 0);
    prop_set_raw(nav, intern_c("appName"), v_str(intern_c("Netscape")), 0);
    prop_set_raw(nav, intern_c("appVersion"), v_str(intern_c("5.0 (RDX)")), 0);
    prop_set_raw(nav, intern_c("platform"), v_str(intern_c("RDX x86_64")), 0);
    prop_set_raw(nav, intern_c("vendor"), v_str(S_empty), 0);
    prop_set_raw(nav, intern_c("language"), v_str(intern_c("en-US")), 0);
    JsObj *langs = new_array(2);
    arr_push(langs, v_str(intern_c("en-US")));
    arr_push(langs, v_str(intern_c("en")));
    prop_set_raw(nav, intern_c("languages"), v_obj(langs), 0);
    prop_set_raw(nav, intern_c("cookieEnabled"), v_bool(1), 0);
    prop_set_raw(nav, intern_c("onLine"), v_bool(1), 0);
    prop_set_raw(nav, intern_c("hardwareConcurrency"), v_num(1), 0);
    prop_set_raw(nav, intern_c("maxTouchPoints"), v_num(0), 0);
    prop_set_raw(nav, intern_c("webdriver"), v_bool(0), 0);
    define_method(nav, "sendBeacon", noop, 2, 0);
    define_method(nav, "vibrate", noop, 1, 0);
    JsObj *screen = new_object();
    define_hidden(w, "screen", v_obj(screen));
    prop_set_raw(screen, intern_c("width"), v_num(1280), 0);
    prop_set_raw(screen, intern_c("height"), v_num(800), 0);
    prop_set_raw(screen, intern_c("availWidth"), v_num(1280), 0);
    prop_set_raw(screen, intern_c("availHeight"), v_num(770), 0);
    prop_set_raw(screen, intern_c("colorDepth"), v_num(24), 0);
    JsObj *hist = new_object();
    define_hidden(w, "history", v_obj(hist));
    define_method(hist, "pushState", history_method, 3, 0);
    define_method(hist, "replaceState", history_method, 3, 0);
    define_method(hist, "back", history_method, 0, 1);
    define_method(hist, "go", history_method, 1, 2);
    define_method(hist, "forward", history_method, 0, 2);
    prop_set_raw(hist, K_length, v_num(1), 0);
    prop_set_raw(hist, intern_c("state"), v_null(), 0);
    JsObj *perf = new_object();
    define_hidden(w, "performance", v_obj(perf));
    define_method(perf, "now", performance_now, 0, 0);
    define_method(perf, "mark", noop, 1, 0);
    define_method(perf, "measure", noop, 1, 0);
    define_method(perf, "getEntriesByType", array_of, 0, 0);
    if (!local_storage) local_storage = storage_new();
    session_storage = storage_new();
    define_hidden(w, "localStorage", v_obj(local_storage));
    define_hidden(w, "sessionStorage", v_obj(session_storage));
    define_method(w, "setTimeout", set_timer, 2, 0);
    define_method(w, "setInterval", set_timer, 2, 1);
    define_method(w, "requestAnimationFrame", set_timer, 1, 2);
    define_method(w, "setImmediate", set_timer, 1, 0);
    define_method(w, "clearTimeout", clear_timer, 1, 0);
    define_method(w, "clearInterval", clear_timer, 1, 0);
    define_method(w, "cancelAnimationFrame", clear_timer, 1, 0);
    define_method(w, "cancelIdleCallback", clear_timer, 1, 0);
    define_method(w, "addEventListener", event_listener, 2, 0);
    define_method(w, "removeEventListener", event_listener, 2, 1);
    define_method(w, "dispatchEvent", dispatch_event_native, 1, 0);
    define_method(w, "alert", window_alert, 1, 0);
    define_method(w, "confirm", window_alert, 1, 1);
    define_method(w, "prompt", window_alert, 1, 2);
    define_method(w, "scrollTo", window_scroll, 2, 0);
    define_method(w, "scroll", window_scroll, 2, 0);
    define_method(w, "scrollBy", window_scroll, 2, 1);
    define_method(w, "getComputedStyle", window_get_computed_style, 1, 0);
    define_method(w, "matchMedia", window_match_media, 1, 0);
    define_method(w, "fetch", window_fetch, 2, 0);
    define_method(w, "btoa", base64, 1, 0);
    define_method(w, "atob", base64, 1, 1);
    define_method(w, "focus", noop, 0, 0);
    define_method(w, "blur", noop, 0, 0);
    define_method(w, "open", noop, 1, 0);
    define_method(w, "close", noop, 0, 0);
    define_method(w, "print", noop, 0, 0);
    frames_js_init(w);
    JsObj *xhr = make_ctor("XMLHttpRequest", xhr_ctor, 0, 0, XhrProto);
    define_hidden(xhr, "DONE", v_num(4));
    make_ctor("URLSearchParams", search_params_ctor, 1, 0, SearchParamsProto);
    JsObj *url_c = make_ctor("URL", url_ctor, 2, 0, UrlProto);
    define_method(url_c, "createObjectURL", object_url, 1, 0);
    define_method(url_c, "revokeObjectURL", object_url, 1, 1);
    define_method(w, "__rdxBytes", rdx_bytes, 1, 0);
    define_method(w, "__rdxBuffer", rdx_bytes, 1, 1);
    define_method(w, "__rdxZip", rdx_zip, 3, 0);
    make_ctor("Event", event_ctor, 2, 0, EventProto);
    make_ctor("CustomEvent", event_ctor, 2, 0, EventProto);
    make_ctor("KeyboardEvent", event_ctor, 2, 0, EventProto);
    make_ctor("MouseEvent", event_ctor, 2, 0, EventProto);
    JsObj *ObsProto = new_object();
    define_method(ObsProto, "observe", observer_observe, 1, 0);
    define_method(ObsProto, "unobserve", noop, 1, 0);
    define_method(ObsProto, "disconnect", noop, 0, 0);
    define_method(ObsProto, "takeRecords", array_of, 0, 0);
    JsObj *io = make_ctor("IntersectionObserver", observer_ctor, 2, 1, NULL);
    prop_set_raw(io, S_prototype, v_obj(ObsProto), P_HIDDEN);
    JsObj *mo = make_ctor("MutationObserver", observer_ctor, 1, 0, NULL);
    prop_set_raw(mo, S_prototype, v_obj(ObsProto), P_HIDDEN);
    JsObj *ro = make_ctor("ResizeObserver", observer_ctor, 1, 0, NULL);
    prop_set_raw(ro, S_prototype, v_obj(ObsProto), P_HIDDEN);
    JsObj *po = make_ctor("PerformanceObserver", observer_ctor, 1, 0, NULL);
    prop_set_raw(po, S_prototype, v_obj(ObsProto), P_HIDDEN);
    define_hidden(po, "supportedEntryTypes", v_obj(new_array(0)));
    EventTargetProto = obj_new_raw(CL_OBJECT, ObjectProto);
    NodeProto->proto = EventTargetProto;
    ElementProto = obj_new_raw(CL_OBJECT, NodeProto);
    HTMLElementProto = obj_new_raw(CL_OBJECT, ElementProto);
    SVGElementProto = obj_new_raw(CL_OBJECT, ElementProto);
    CharacterDataProto = obj_new_raw(CL_OBJECT, NodeProto);
    TextProto = obj_new_raw(CL_OBJECT, CharacterDataProto);
    CommentProto = obj_new_raw(CL_OBJECT, CharacterDataProto);
    FragmentProto = obj_new_raw(CL_OBJECT, NodeProto);
    ShadowRootProto = obj_new_raw(CL_OBJECT, FragmentProto);
    dom_interface("EventTarget", EventTargetProto);
    dom_interface("Node", NodeProto);
    dom_interface("Element", ElementProto);
    dom_interface("HTMLElement", HTMLElementProto);
    dom_interface("SVGElement", SVGElementProto);
    dom_interface("SVGSVGElement", SVGElementProto);
    dom_interface("CharacterData", CharacterDataProto);
    dom_interface("Text", TextProto);
    dom_interface("Comment", CommentProto);
    dom_interface("DocumentFragment", FragmentProto);
    dom_interface("ShadowRoot", ShadowRootProto);
    define_method(ElementProto, "attachShadow", dom_attach_shadow, 1, 0);
    define_hidden(ElementProto, "shadowRoot", v_null());
    for (int i = 0; i < IFACE_COUNT; i++) {
        tag_protos[i] = HTMLElementProto;
        if (!strcmp(IFACE_TAGS[i][1], "HTMLElement")) continue;
        for (int k = 0; k < i; k++) /* (one prototype per interface: h1..h6, td/th share) */
            if (!strcmp(IFACE_TAGS[k][1], IFACE_TAGS[i][1])) tag_protos[i] = tag_protos[k];
        if (tag_protos[i] != HTMLElementProto) continue;
        tag_protos[i] = obj_new_raw(CL_OBJECT, HTMLElementProto);
        dom_interface(IFACE_TAGS[i][1], tag_protos[i]);
    }
    dom_interface("Document", DocumentProto);
    dom_interface("HTMLDocument", DocumentProto);
    dom_interface("Window", global_obj);
    JsObj *ce = new_object();
    define_hidden(w, "customElements", v_obj(ce));
    define_method(ce, "define", noop, 2, 0);
    define_method(ce, "get", noop, 1, 0);
    define_method(ce, "whenDefined", promise_static_resolve, 1, 0);
    JsObj *crypto = new_object();
    define_hidden(w, "crypto", v_obj(crypto));
    define_method(crypto, "randomUUID", random_uuid, 0, 0);
    define_method(crypto, "getRandomValues", get_random_values, 1, 0);
    current_script = NULL;
    page_loaded = 0;
    /* the features written in JavaScript (web/prelude.js) */
    missing_on = 0;
    if (js_enabled) js_run_script(JS_PRELUDE, (int)sizeof(JS_PRELUDE) - 1, "the browser's prelude");
    if (getenv("BROWSER_MISSING")) { /* everything made so far is the browser's own */
        for (Gc *g = heap_list; g; g = g->next) g->builtin = 1;
        missing_on = 1;
    }
    js_errors = 0;
    dom_dirty = 0;
    free(js_navigate);
    js_navigate = NULL;
    js_alert[0] = 0;
}

/* Free everything scripts made: called before the next page. */
static void js_page_end(void) {
    while (heap_list) {
        Gc *g = heap_list;
        heap_list = g->next;
        if (g->kind == K_OBJ) {
            JsObj *o = (JsObj *)g;
            if (o->cls == CL_NODE && o->node) o->node->js = NULL;
        }
        gc_free(g);
    }
    heap_since_gc = 0;
    local_storage = NULL; /* (kept per page only, since it lives in the heap) */
    global_obj = NULL;
    location_obj = NULL;
    ElementProto = NULL; /* (the next page makes its own) */
    timer_count = 0;
    arena_free_all();
    /* free the nodes scripts took out of the page */
    for (int i = 0; i < orphan_count; i++) {
        Node *n = orphans[i];
        if (!n || n->parent || n == document) continue;
        int dup = 0;
        for (int k = 0; k < i && !dup; k++) dup = orphans[k] == n;
        if (dup) continue;
        free_tree(n);
        for (int k = i + 1; k < orphan_count; k++) {
            if (orphans[k] == n) orphans[k] = NULL;
        }
    }
    orphan_count = 0;
    js_oom = 0;
    console_text.len = 0;
    if (console_text.p) console_text.p[0] = 0;
}

/* Run the page's scripts in order, then DOMContentLoaded. */
static void js_run_page_scripts(void) {
    if (!js_enabled) return;
    page_script_start = uptime_ms();
    page_script_bytes = 0;
    page_scripts_skipped = 0;
    memory_low = 0;
    page_script_fetch_ms = 0;
    page_script_fetches = 0;
    /* a snapshot of the <script> elements: scripts may add more (those run when inserted) */
    int count = 0;
    for (Node *d = document; d; d = next_in_tree(d, document)) count += d->type == NODE_ELEMENT && is(d->tag, "script");
    if (!count) return;
    Node **list = malloc(sizeof(Node *) * (size_t)count);
    if (!list) return;
    int k = 0;
    for (Node *d = document; d && k < count; d = next_in_tree(d, document)) {
        if (d->type == NODE_ELEMENT && is(d->tag, "script")) {
            wrap(d); /* (pinned: a script removing it cannot free it) */
            list[k++] = d;
        }
    }
    prefetch_scripts();
    for (int i = 0; i < k; i++) {
        if (contains_node(document, list[i])) run_script_element(list[i]);
        if (js_navigate) break;
    }
    prefetch_end(script_prefetch);
    script_prefetch = NULL;
    free(list);
    if (getenv("BROWSER_TIMING")) memory_report("after the page's scripts");
    if (getenv("BROWSER_TIMING"))
        printf("[timing] %d scripts fetched in %d ms, %d KiB of script\n", page_script_fetches, (int)page_script_fetch_ms, (int)(page_script_bytes / 1024));
    if (getenv("BROWSER_TIMING"))
        printf("[timing] connections: %d new (%d ms), %d reused; waiting for replies %d ms, unpacking %d ms\n", stat_conn_new, (int)stat_connect_ms, stat_conn_reused, (int)stat_wait_ms, (int)stat_unpack_ms);
    if (page_scripts_skipped) {
        char msg[120] = "the page has more script than RDX can run: ";
        char n[12];
        number_to_text(page_scripts_skipped, n);
        strncat_safe(msg, n, 10);
        strncat_safe(msg, " scripts were skipped", 30);
        strncat_safe(msg, js_oom || memory_low ? " (not enough memory)" : uptime_ms() - page_script_start >= PAGE_SCRIPT_MS ? " (out of time)" : " (too much script)", 30);
        js_report(msg);
        js_errors++;
    }
}

/* Dispatch an event from the browser (a click, a key...): returns 1 if a
   script prevented the default action. */
static int js_dispatch(Node *target, JsObj *event) {
    if (!js_enabled || !global_obj) return 0;
    char here;
    js_set_stack_base(&here);
    JsRun run;
    js_run_begin(&run, 1000);
    int r = dispatch_event(target ? wrap(target) : global_obj, event);
    if (r < 0) {
        describe_exception(js_last_error, sizeof(js_last_error));
        js_report(js_last_error);
        js_errors++;
        js_abort = 0;
        r = 0;
    }
    js_run_end(&run);
    run_microtasks();
    return r;
}

static int js_fire(Node *target, const char *type, int bubbles, int cancelable) {
    if (!js_enabled || !global_obj) return 0;
    return js_dispatch(target, new_event(type, bubbles, cancelable));
}
