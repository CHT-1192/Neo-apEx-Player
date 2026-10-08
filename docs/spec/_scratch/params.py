import os
import re, collections, json
p=os.path.expanduser("~/Repos/apEx-public/Projects/Clean Slate/cleanslate.apx")
lines=open(p,encoding='utf-8',errors='replace').read().split('\n')
el=re.compile(r'^(\s*)<([A-Za-z_][\w:.-]*)((?:[^>"]|"[^"]*")*?)(/?)>')
at=re.compile(r'([\w:.-]+)\s*=\s*"([^"]*)"')
stack=[]; cur=None; objs=[]; curfilter=None
text=lambda i: lines[i].strip()
for i,line in enumerate(lines):
    m=el.match(line)
    if m:
        ind,name,attrs,sc=m.group(1),m.group(2),m.group(3),m.group(4)
        a=dict(at.findall(attrs))
        while stack and len(stack[-1][1])>=len(ind): stack.pop()
        parent=stack[-1][0] if stack else None
        if name=='Object':
            cur={'line':i+1,'type':int(a['Type']),'params':{},'filters':[],'name':'','parents':[]}
            objs.append(cur)
        elif name=='Filter' and 'Type' in a and parent=='Object' and cur is not None:
            curfilter={'type':int(a['Type']),'params':{},'srt':{}}
            cur['filters'].append(curfilter)
        elif name=='parameter' and cur is not None:
            if parent=='Object': cur['params'][int(a['index'])]=int(a['value'])
            elif parent=='Filter' and curfilter is not None: curfilter['params'][int(a['index'])]=int(a['value'])
        elif name=='transformation' and cur is not None:
            if parent=='Filter' and curfilter is not None: curfilter['srt'][int(a['index'])]=int(a['value'])
        if not sc: stack.append((name,ind))
    elif cur is not None and line.strip() and not line.strip().startswith('<'):
        pass
# names: <Name>text</Name>
name_re=re.compile(r'<Name>([^<]*)</Name>')
# recompute names by scanning object blocks
import itertools
idx=[o['line'] for o in objs]
for k,o in enumerate(objs):
    end=objs[k+1]['line'] if k+1<len(objs) else len(lines)
    for j in range(o['line']-1,end):
        mm=name_re.search(lines[j])
        if mm: o['name']=mm.group(1); break
print("== objects per type ==")
c=collections.Counter(o['type'] for o in objs)
for t,n in sorted(c.items(), key=lambda kv:-kv[1]): print(f"  Mesh_{t}: {n}")
print("== filters per type (inside objects) ==")
cf=collections.Counter(f['type'] for o in objs for f in o['filters'])
for t,n in sorted(cf.items(), key=lambda kv:-kv[1]): print(f"  ModelFilter_{t}: {n}")
print("== parameter distributions per object type ==")
for t in sorted(c, key=lambda t:-c[t]):
    ps=collections.defaultdict(collections.Counter)
    for o in objs:
        if o['type']==t:
            for k,v in o['params'].items(): ps[k][v]+=1
    print(f"Mesh_{t} (n={c[t]}):", {k:dict(v.most_common(6)) for k,v in sorted(ps.items())})
print("== text objects (type 16) ==")
for o in objs:
    if o['type']==16: print("  line",o['line'],"name",repr(o['name']),"params",o['params'],"filters",[f['type'] for f in o['filters']])
print("== filter param distributions ==")
for t in sorted(cf, key=lambda t:-cf[t]):
    ps=collections.defaultdict(collections.Counter); srts=collections.Counter()
    for o in objs:
        for f in o['filters']:
            if f['type']==t:
                for k,v in f['params'].items(): ps[k][v]+=1
                srts[tuple(sorted(f['srt'].items()))]+=1
    print(f"ModelFilter_{t} (n={cf[t]}):", {k:dict(v.most_common(6)) for k,v in sorted(ps.items())})
print("== filter chain lengths ==", dict(collections.Counter(len(o['filters']) for o in objs)))
