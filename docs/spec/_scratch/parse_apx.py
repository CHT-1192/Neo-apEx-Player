import os
import re, sys, collections
p = os.path.expanduser("~/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx")
line_re = re.compile(r'^(\s*)<([A-Za-z_][\w:.-]*)((?:[^>"]|"[^"]*")*?)(/?)>')
attr_re = re.compile(r'([\w:.-]+)\s*=\s*"([^"]*)"')
stack = []           # (name, indent)
obj_types = collections.Counter()
filt_types = collections.Counter()
filt_under = collections.Counter()
obj_filter_types = collections.Counter()   # filters inside Object
other_filter_types = collections.Counter()
obj_filter_params = collections.defaultdict(list)
obj_count_in_scene = 0
curname = collections.Counter()
total_obj = 0
cur_obj = None
for lineno, line in enumerate(open(p, encoding='utf-8', errors='replace'), 1):
    m = line_re.match(line)
    if m:
        indent, name, attrs, selfclose = m.group(1), m.group(2), m.group(3), m.group(4)
        attrs_d = dict(attr_re.findall(attrs))
        while stack and len(stack[-1][1]) >= len(indent):
            stack.pop()
        parent = stack[-1][0] if stack else None
        if name == 'Object':
            total_obj += 1
            t = attrs_d.get('Type')
            obj_types[t] += 1
            cur_obj = {'type': t, 'line': lineno, 'filters': [], 'name': None}
        elif name == 'Filter' and 'Type' in attrs_d:
            t = attrs_d['Type']
            filt_types[t] += 1
            filt_under[parent] += 1
            if parent == 'Object' and cur_obj is not None:
                cur_obj['filters'].append(t)
                obj_filter_types[t] += 1
            else:
                other_filter_types[t] += 1
        elif name == 'Name' and parent == 'Object' and cur_obj is not None and cur_obj['name'] is None:
            # next text line is the name
            cur_obj['pending_name'] = True
        if not selfclose:
            stack.append((name, indent))
        if name == 'Object' and selfclose:
            cur_obj = None
print("total <Object> nodes:", total_obj)
print("Object Type counts:", dict(sorted(obj_types.items(), key=lambda kv: -kv[1])))
print()
print("Filter Type counts (ALL):", dict(sorted(filt_types.items(), key=lambda kv: -kv[1])))
print("Filter parent element:", dict(filt_under))
print()
print("Filter inside <Object>:", dict(sorted(obj_filter_types.items(), key=lambda kv: -kv[1])))
print("Filter elsewhere      :", dict(sorted(other_filter_types.items(), key=lambda kv: -kv[1])))
