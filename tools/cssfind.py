"""cssfind.py CSSFILE SELECTOR_REGEX [PROP_REGEX]: rules whose selector
matches, with the @media / @supports they sit in."""
import re, sys

css = open(sys.argv[1], encoding='utf-8').read()
sel_re = re.compile(sys.argv[2])
prop_re = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
stack, i, start = [], 0, 0
while i < len(css):
    ch = css[i]
    if ch == '{':
        head = css[start:i].strip()
        if head.startswith('@'):
            stack.append(head)
            start = i + 1
        else:
            j = css.index('}', i)
            body = css[i + 1:j]
            if sel_re.search(head) and (not prop_re or prop_re.search(body)):
                ctx = ' / '.join(stack)
                print((ctx + ' :: ' if ctx else '') + head[-160:] + ' { ' + body[:240] + ' }')
            i = j
            start = j + 1
    elif ch == '}':
        if stack: stack.pop()
        start = i + 1
    i += 1
