#!/usr/bin/env python3
"""Minimal SPIR-V reflection: dump descriptor bindings and interface variables.

Used during development to verify what glslang/libshaderc did with our HLSL
preprocessing ([[vk::binding]] assignment).  The same logic is implemented in
C++ in src/d3d11/spirv_reflect.cpp.
"""
import struct
import sys
import collections

# SpvOp
OP_NAME = 5
OP_MEMBER_NAME = 6
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_VARIABLE = 59
OP_TYPE_POINTER = 32
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_STRUCT = 30
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_CONSTANT = 43
OP_TYPE_FLOAT = 22
OP_TYPE_INT = 21
OP_TYPE_VECTOR = 23
OP_TYPE_MATRIX = 24

STORAGE = {
    0: "UniformConstant", 1: "Input", 2: "Uniform", 3: "Output", 4: "Workgroup",
    5: "CrossWorkgroup", 6: "Private", 7: "Function", 8: "Generic",
    9: "PushConstant", 10: "AtomicCounter", 11: "Image", 12: "StorageBuffer",
}

# SpvDecoration (note: Binding=33, DescriptorSet=34, Location=30, Offset=35)
DECORATION = {
    0: "RelaxedPrecision", 1: "SpecId", 2: "Block", 3: "BufferBlock", 11: "BuiltIn",
    14: "Flat", 30: "Location", 33: "Binding", 34: "DescriptorSet", 35: "Offset",
}
BINDING = 33
DESCRIPTOR_SET = 34
LOCATION = 30
BUILTIN = 11
OFFSET = 35


class Module:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.words = struct.unpack_from("<%dI" % (len(data) // 4), data, 0)
        if self.words[0] != 0x07230203:
            raise ValueError("not a SPIR-V module")
        self.names = {}
        self.member_names = collections.defaultdict(dict)
        self.decorations = collections.defaultdict(dict)
        self.member_decorations = collections.defaultdict(dict)
        self.variables = {}
        self.pointer_types = {}
        self.type_defs = {}
        self.constants = {}
        self.entry_points = []
        self._parse()

    def _parse(self):
        w = self.words
        i = 5
        n = len(w)
        while i < n:
            word = w[i]
            op = word & 0xFFFF
            cnt = word >> 16
            if cnt == 0:
                break
            a = w[i + 1:i + cnt]
            if op == OP_NAME:
                self.names[a[0]] = self._string(a, 1)
            elif op == OP_MEMBER_NAME:
                self.member_names[a[0]][a[1]] = self._string(a, 2)
            elif op == OP_DECORATE:
                self.decorations[a[0]][a[1]] = tuple(a[2:])
            elif op == OP_MEMBER_DECORATE:
                self.member_decorations[a[0]].setdefault(a[1], {})[a[2]] = tuple(a[3:])
            elif op == OP_VARIABLE:
                self.variables[a[1]] = {"type": a[0], "storage": a[2], "initializer": a[3] if len(a) > 3 else None}
            elif op == OP_TYPE_POINTER:
                self.pointer_types[a[0]] = {"storage": a[1], "pointee": a[2]}
            elif op == OP_ENTRY_POINT:
                self.entry_points.append({"execution_model": a[0], "function": a[1], "name": self._string(a, 2)})
            elif op == OP_TYPE_IMAGE:
                self.type_defs[a[0]] = ("image", a[1:])
            elif op == OP_TYPE_SAMPLER:
                self.type_defs[a[0]] = ("sampler", ())
            elif op == OP_TYPE_SAMPLED_IMAGE:
                self.type_defs[a[0]] = ("sampled_image", (a[1],))
            elif op == OP_TYPE_STRUCT:
                self.type_defs[a[0]] = ("struct", tuple(a[1:]))
            elif op == OP_TYPE_ARRAY:
                self.type_defs[a[0]] = ("array", (a[1], a[2]))
            elif op == OP_TYPE_VECTOR:
                self.type_defs[a[0]] = ("vector", (a[1], a[2]))
            elif op == OP_TYPE_MATRIX:
                self.type_defs[a[0]] = ("matrix", (a[1], a[2]))
            elif op == OP_TYPE_FLOAT:
                self.type_defs[a[0]] = ("float", (a[1],))
            elif op == OP_TYPE_INT:
                self.type_defs[a[0]] = ("int", (a[1], a[2]))
            elif op == OP_CONSTANT:
                self.constants[a[1]] = (a[0], tuple(a[2:]))
            i += cnt

    def _string(self, a, start):
        raw = b"".join(struct.pack("<I", x) for x in a[start:])
        return raw.split(b"\0")[0].decode("utf8", "replace")

    def describe_type(self, tid, depth=0):
        if depth > 4:
            return "..."
        t = self.type_defs.get(tid)
        if not t:
            return f"type{tid}"
        kind, args = t
        if kind == "image":
            return "image"
        if kind == "sampler":
            return "sampler"
        if kind == "sampled_image":
            return "sampled_image<%s>" % self.describe_type(args[0], depth + 1)
        if kind == "struct":
            return "struct{%s}" % ",".join(self.member_names.get(tid, {}).get(i, self.describe_type(m, depth + 1))
                                            for i, m in enumerate(args))
        if kind == "array":
            n = self.constants.get(args[1], (None, (0,)))[1][0]
            return "%s[%d]" % (self.describe_type(args[0], depth + 1), n)
        if kind == "vector":
            return "%s%d" % (self.describe_type(args[0], depth + 1), args[1])
        if kind == "matrix":
            return "mat%d" % args[1]
        if kind == "float":
            return "float%d" % args[0]
        if kind == "int":
            return "int%d" % args[0]
        return kind

    def descriptors(self):
        out = []
        for vid, v in self.variables.items():
            if v["storage"] not in (0, 2, 12):  # UniformConstant, Uniform, StorageBuffer
                continue
            d = self.decorations.get(vid, {})
            binding = d.get(BINDING)
            out.append({
                "id": vid,
                "name": self.names.get(vid, "?"),
                "binding": binding[0] if binding else None,
                "set": (d.get(DESCRIPTOR_SET) or (0,))[0],
                "type": self.describe_type(self.pointer_types[v["type"]]["pointee"]),
                "storage": STORAGE[v["storage"]],
            })
        return sorted(out, key=lambda x: (x["binding"] is None, x["binding"]))

    def interfaces(self):
        out = []
        for vid, v in self.variables.items():
            if v["storage"] not in (1, 3):
                continue
            d = self.decorations.get(vid, {})
            loc = d.get(LOCATION)
            bi = d.get(BUILTIN)
            out.append({
                "id": vid,
                "name": self.names.get(vid, "?"),
                "location": loc[0] if loc else None,
                "builtin": bi[0] if bi else None,
                "storage": STORAGE[v["storage"]],
                "type": self.describe_type(self.pointer_types[v["type"]]["pointee"]),
                "storage": STORAGE[v["storage"]],
            })
        return sorted(out, key=lambda x: (x["location"] is None, x["location"]))


def dump_structs(m):
    """Print the member offsets of every Block/BufferBlock struct: this is how we
    verify that glslang emitted D3D (not std140) constant-buffer packing."""
    for tid, t in sorted(m.type_defs.items()):
        if t[0] != "struct":
            continue
        md = m.member_decorations.get(tid)
        if not md:
            continue
        is_block = 2 in m.decorations.get(tid, {}) or 3 in m.decorations.get(tid, {})
        print("   struct %s%s" % (m.names.get(tid, "type%d" % tid), " (cbuffer)" if is_block else ""))
        for i, member in enumerate(t[1]):
            off = md.get(i, {}).get(35, (None,))[0]
            print("     +%-5s %-24s %s" % (off, m.member_names.get(tid, {}).get(i, "?"),
                                           m.describe_type(member)))


def main(argv):
    for path in argv[1:]:
        m = Module(path)
        print("==", path)
        for ep in m.entry_points:
            print("   entry point: %s (model %d)" % (ep["name"], ep["execution_model"]))
        print("   descriptors:")
        for d in m.descriptors():
            print("     binding=%-4s set=%s %-9s %-24s %s" % (d["binding"], d["set"], d["storage"], d["name"], d["type"]))
        dump_structs(m)
        print("   interface:")
        for v in m.interfaces():
            print("     %-9s loc=%-4s builtin=%-6s %-16s %s" % (v["storage"], v["location"], v["builtin"], v["name"], v["type"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
