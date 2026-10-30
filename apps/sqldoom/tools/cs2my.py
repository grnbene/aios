#!/usr/bin/env python3
# Scratch: SQLDoom's CedarScript runtime (sql/runtime/functions/*.sql) -> MySQL stored functions for ASQL.
# The SQL inside is left as written (ASQL now reads its PostgreSQL); what changes is the procedural layer:
#   let [mut] x = e;            -> DECLARE v_x T (hoisted to the function's start); SET v_x = e;
#   x = e;                      -> SET v_x = e;
#   if c { .. }                 -> IF c THEN .. END IF;
#   return [e];                 -> RETURN e;  (RETURN 0 for a function with no RETURNS)
#   f(..);                      -> DO f(..);
#   SELECT .. AS a, .. { .. }   -> a cursor over the SELECT, the block run for each row with v_a bound
# Parameters become a_<name> and locals v_<name>, replaced in expression positions of the SQL (not in column lists,
# SET targets, aliases or qualified names); any such name met in a position it is not replaced in is reported.
import re, sys, glob, os, json
CUR_BODY = ''

TYPEMAP = {
    "integer": "INT", "int": "INT", "int4": "INT", "bigint": "BIGINT", "int8": "BIGINT", "smallint": "SMALLINT",
    "double precision": "DOUBLE", "float8": "DOUBLE", "real": "DOUBLE", "float4": "DOUBLE", "numeric": "DOUBLE",
    "boolean": "BOOLEAN", "bool": "BOOLEAN", "text": "TEXT", "varchar": "TEXT", "bytea": "LONGBLOB",
}
def my_type(pg):
    pg = pg.strip().lower()
    pg = re.sub(r"\(.*\)", "", pg).strip()
    if pg in TYPEMAP: return TYPEMAP[pg]
    if pg.startswith("character varying") or pg.startswith("char"): return "TEXT"
    return "VARCHAR(32)"   # an enum type: its label

# ---- lexer ----------------------------------------------------------------------------------------------------------
TOK = re.compile(r"""
  (?P<ws>\s+) | (?P<comment>--[^\n]*) | (?P<str>[Ee]?'(?:[^']|'')*') | (?P<num>\d+(?:\.\d+)?(?:[eE][-+]?\d+)?) |
  (?P<id>[A-Za-z_][A-Za-z_0-9$]*|"[^"]*") | (?P<op>::|<>|<=|>=|!=|\|\||<<|>>|[-+*/%=<>(),;.{}\[\]|&#^~!:@])
""", re.X)
def lex(s):
    out = []
    pos = 0
    while pos < len(s):
        m = TOK.match(s, pos)
        if not m: raise SyntaxError("lex at %r" % s[pos:pos+40])
        k = m.lastgroup
        out.append((k, m.group(), m.start()))
        pos = m.end()
    return out

SQL_START = {"SELECT", "WITH", "INSERT", "UPDATE", "DELETE"}

# ---- schema: column types by name, for typing a SELECT's columns ----------------------------------------------------
def schema_columns(path):
    cols = {}
    text = open(path).read()
    for m in re.finditer(r"CREATE TABLE IF NOT EXISTS (\w+) \((.*?)\n\);", text, re.S):
        for line in m.group(2).split("\n"):
            line = re.sub(r"--.*", "", line).strip().rstrip(",")
            mm = re.match(r"(\w+)\s+(.+)", line)
            if not mm or mm.group(1).upper() in ("PRIMARY", "FOREIGN", "UNIQUE", "KEY", "CHECK", "INDEX", "CONSTRAINT"): continue
            ty = mm.group(2).upper()
            if ty.startswith("ENUM") or ty.startswith("VARCHAR") or ty.startswith("TEXT") or ty.startswith("CHAR"): t = "TEXT"
            elif ty.startswith("BOOLEAN"): t = "BOOLEAN"
            elif ty.startswith("REAL") or ty.startswith("DOUBLE") or ty.startswith("NUMERIC") or ty.startswith("DECIMAL"): t = "DOUBLE"
            elif ty.startswith("BIGINT") or ty.startswith("INT") or ty.startswith("SMALLINT"): t = "BIGINT"
            elif ty.startswith("LONGBLOB"): t = "LONGBLOB"
            elif ty.startswith("DATETIME") or ty.startswith("TIMESTAMP"): t = "DATETIME(6)"
            else: t = None
            cols.setdefault(mm.group(1).lower(), set()).add(t)
    return {k: (list(v)[0] if len(v) == 1 else None) for k, v in cols.items()}

# ---- parsing a CedarScript body into nodes --------------------------------------------------------------------------
class P:
    def __init__(self, toks): self.t = [x for x in toks if x[0] != "ws"]; self.i = 0
    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else ("end", "", -1)
    def next(self): x = self.peek(); self.i += 1; return x
    def at(self, txt, k=0): return self.peek(k)[1].upper() == txt.upper() and self.peek(k)[0] in ("id", "op")

def take_until(p, stops):
    """Tokens up to (not including) a stop punct at depth 0."""
    out, depth = [], 0
    while p.peek()[0] != "end":
        k, v, _ = p.peek()
        if k == "op" and v in ("(", "["): depth += 1
        elif k == "op" and v in (")", "]"): depth -= 1
        if depth == 0 and k == "op" and v in stops: break
        out.append(p.next())
    return out

def parse_block(p, closing):
    nodes = []
    pending_comments = []
    while True:
        k, v, _ = p.peek()
        if k == "end": break
        if closing and k == "op" and v == "}": p.next(); break
        if k == "comment": pending_comments.append(p.next()[1]); continue
        node = None
        if k == "id" and v == "let":
            p.next()
            mut = False
            if p.at("mut"): p.next(); mut = True
            name = p.next()[1]
            if p.next()[1] != "=": raise SyntaxError("let without =")
            expr = take_until(p, (";",)); p.next()
            node = {"k": "let", "name": name, "expr": expr}
        elif k == "id" and v == "if" and not p.at("(", 1) or (k == "id" and v == "if" and p.at("(", 1) and True):
            p.next()
            cond = take_until(p, ("{",)); p.next()
            body = parse_block(p, True)
            node = {"k": "if", "cond": cond, "body": body}
        elif k == "id" and v == "return":
            p.next()
            expr = take_until(p, (";",)); p.next()
            node = {"k": "return", "expr": expr}
        elif k == "id" and v.upper() in SQL_START or (k == "op" and v == "("):
            stmt = take_until(p, (";", "{"))
            k2, v2, _ = p.next()
            if v2 == "{":
                body = parse_block(p, True)
                if p.at(";"): p.next()
                node = {"k": "select_block", "sql": stmt, "body": body}
            else:
                node = {"k": "sql", "sql": stmt}
        elif k == "id" and p.at("=", 1):
            name = p.next()[1]; p.next()
            expr = take_until(p, (";",)); p.next()
            node = {"k": "set", "name": name, "expr": expr}
        elif k == "id" and p.at("(", 1):
            call = take_until(p, (";",)); p.next()
            node = {"k": "call", "expr": call}
        elif k == "op" and v == ";":
            p.next(); continue
        else:
            raise SyntaxError("statement at %r %r" % (v, [x[1] for x in p.t[p.i:p.i+8]]))
        node["comments"] = pending_comments
        pending_comments = []
        nodes.append(node)
    if pending_comments: nodes.append({"k": "comments", "comments": pending_comments})
    return nodes

# ---- emitting -------------------------------------------------------------------------------------------------------
NAME_BEFORE = {"AS", "FROM", "JOIN", "INTO", "UPDATE", "TABLE"}
class Fn:
    def __init__(self, name, params, returns, body_toks, comments):
        self.name, self.params, self.returns, self.comments = name, params, returns, comments
        self.body = parse_block(P(body_toks), False)

def emit_tokens(toks, scope, report, where):
    """SQL / expression tokens as text, CedarScript names in expression positions as their MySQL names."""
    out = []
    t = [x for x in toks if x[0] != "ws"]
    in_insert_cols = False
    stmt_kind = t[0][1].upper() if t else ""
    paren_after_into = -1
    depth = 0
    in_conflict_cols = False
    for i, (k, v, _) in enumerate(t):
        prev = t[i - 1] if i > 0 else ("", "", 0)
        nxt = t[i + 1] if i + 1 < len(t) else ("", "", 0)
        if k == "op" and v == "(":
            depth += 1
            # INSERT INTO tbl ( cols ) / ON CONFLICT ( cols )
            if i >= 2 and t[i - 2][1].upper() == "INTO" and stmt_kind in ("INSERT", "WITH"): in_insert_cols = True; paren_after_into = depth
            if prev[1].upper() == "CONFLICT": in_conflict_cols = True; paren_after_into = depth
        if k == "op" and v == ")":
            if (in_insert_cols or in_conflict_cols) and depth == paren_after_into: in_insert_cols = in_conflict_cols = False
            depth -= 1
        txt = v
        if k == "id" and v.lower() in scope:
            lv = v.lower()
            position_ok = True
            if prev[1] == "." or nxt[1] == "." or nxt[1] == "(": position_ok = False
            elif prev[1].upper() in NAME_BEFORE: position_ok = False
            elif in_insert_cols or in_conflict_cols: position_ok = False
            elif nxt[1] == "=" and (prev[1].upper() == "SET" or prev[1] == ",") and stmt_kind in ("UPDATE", "WITH", "INSERT") and depth == 0:
                position_ok = False   # a SET target
            elif prev[0] == "id" and prev[1].upper() not in KEYWORDS and i >= 2 and t[i - 2][1].upper() in ("FROM", "JOIN", ","):
                position_ok = False   # a table's alias
            if position_ok: txt = scope[lv]["my"]
            elif prev[1] != "." and prev[1].upper() != "AS":   # a qualified column or an alias is plainly one
                report.append("%s: '%s' left as a name at %r" % (where, v, " ".join(x[1] for x in t[max(0, i - 4):i + 3])))
        out.append((k, txt))
    # the source as written, each renamed token replaced in place
    if not t: return ""
    a0 = t[0][2]
    a1 = t[-1][2] + len(t[-1][1])
    s = CUR_BODY[a0:a1]
    for (k, v, pos), (k2, txt) in reversed(list(zip(t, out))):
        if txt != v: s = s[:pos - a0] + txt + s[pos - a0 + len(v):]
    return s

KEYWORDS = {"SELECT","FROM","WHERE","AND","OR","NOT","JOIN","LEFT","RIGHT","INNER","CROSS","ON","AS","GROUP","BY","ORDER",
            "LIMIT","UNION","ALL","CASE","WHEN","THEN","ELSE","END","IN","IS","NULL","EXISTS","SET","VALUES","INTO",
            "INSERT","UPDATE","DELETE","WITH","RECURSIVE","DISTINCT","HAVING","LATERAL","USING","ASC","DESC","BETWEEN",
            "LIKE","OVER","PARTITION","ROWS","CONFLICT","DO","NOTHING","FILTER","TRUE","FALSE","OFFSET"}

def infer(toks, scope, fns, cols):
    """A MySQL type for an expression, or None."""
    t = [x for x in toks if x[0] not in ("ws", "comment")]
    while len(t) >= 2 and t[0][1] == "(" and t[-1][1] == ")":
        depth = 0; ok = True
        for j, x in enumerate(t):
            if x[1] == "(": depth += 1
            elif x[1] == ")":
                depth -= 1
                if depth == 0 and j < len(t) - 1: ok = False; break
        if not ok: break
        t = t[1:-1]
    if not t: return None
    up = [x[1].upper() for x in t]
    # a cast at the end
    if len(t) >= 3 and t[-2][1] == "::": return my_type(t[-1][1])
    if len(t) >= 6 and t[-4][1] == "::" and t[-1][1] == ")": return my_type(t[-3][1])   # ::numeric(4,1)
    if len(t) >= 4 and t[-3][1] == "::" and t[-2][1].lower() == "double": return "DOUBLE"
    depth = 0
    top = []
    for x in t:
        if x[1] in ("(", "["): depth += 1
        elif x[1] in (")", "]"): depth -= 1
        elif depth == 0: top.append(x)
    topv = [x[1].upper() for x in top]
    if any(v in ("=", "<>", "<", ">", "<=", ">=", "!=", "AND", "OR", "NOT", "IS", "EXISTS", "IN", "LIKE", "BETWEEN") for v in topv): return "BOOLEAN"
    if any(v in ("&", "|", "<<", ">>", "#") for v in topv): return "BIGINT"
    if any(v == "||" for v in topv): return "TEXT"
    if len(t) == 1:
        k, v, _ = t[0]
        if v.upper() in ("TRUE", "FALSE"): return "BOOLEAN"
        if k == "num": return "BIGINT" if re.fullmatch(r"\d+", v) else "DOUBLE"
        if k == "str": return "TEXT"
        if k == "id" and v.lower() in scope and scope[v.lower()].get("type"): return scope[v.lower()]["type"]
        if k == "id" and v.lower() in cols and cols[v.lower()]: return cols[v.lower()]
        if v.upper() == "NULL": return None
    if len(t) == 3 and t[1][1] == "." and t[2][1].lower() in cols and cols[t[2][1].lower()]: return cols[t[2][1].lower()]
    if len(t) == 2 and t[0][1] == "-": return infer(t[1:], scope, fns, cols)
    if t[0][0] == "id" and len(t) > 1 and t[1][1] == "(":
        f = t[0][1].lower()
        # the call is the whole expression?
        depth = 0; whole = True
        for j, x in enumerate(t[1:], 1):
            if x[1] == "(": depth += 1
            elif x[1] == ")":
                depth -= 1
                if depth == 0 and j < len(t) - 1: whole = False; break
        if whole:
            if f in fns: return fns[f]
            if f in ("count",): return "BIGINT"
            if f in ("upper", "lower", "substring", "trim", "concat", "lpad", "rpad", "left", "right", "chr", "string_agg"): return "TEXT"
            if f in ("bool_or", "bool_and", "every"): return "BOOLEAN"
            if f in ("sqrt", "sin", "cos", "tan", "atan2", "atan", "asin", "radians", "degrees", "power", "doom_const"): return "DOUBLE"
            if f in ("abs", "greatest", "least", "coalesce", "max", "min", "sum", "nullif", "floor", "ceil", "round", "trunc"):
                # the type of the first argument
                depth = 0; arg = []
                for x in t[2:]:
                    if x[1] == "(": depth += 1
                    elif x[1] == ")":
                        if depth == 0: break
                        depth -= 1
                    elif x[1] == "," and depth == 0: break
                    arg.append(x)
                ty = infer(arg, scope, fns, cols)
                if f in ("floor", "ceil", "round", "trunc") and ty == "DOUBLE": return "DOUBLE"
                if f == "sum" and ty in ("BIGINT", "INT", "SMALLINT"): return "BIGINT"
                return ty
            if f in ("length", "ascii", "row_number"): return "BIGINT"
    if t[0][1].upper() == "CASE":
        # THEN branches
        for j, x in enumerate(t):
            if x[1].upper() == "THEN":
                depth = 0; arg = []
                for y in t[j + 1:]:
                    if y[1] in ("(",): depth += 1
                    elif y[1] in (")",): depth -= 1
                    if depth == 0 and y[1].upper() in ("WHEN", "ELSE", "END"): break
                    arg.append(y)
                ty = infer(arg, scope, fns, cols)
                if ty: return ty
        return None
    if any(v in ("+", "-", "*", "/", "%") for v in topv):
        tys = set()
        cur = []
        for x in top + [("op", "+", 0)]:
            pass
        # operands at depth 0
        operands, cur, depth = [], [], 0
        for x in t:
            if x[1] in ("(", "["): depth += 1
            elif x[1] in (")", "]"): depth -= 1
            if depth == 0 and x[0] == "op" and x[1] in ("+", "-", "*", "/", "%"):
                if cur: operands.append(cur)
                cur = []
                continue
            cur.append(x)
        if cur: operands.append(cur)
        for o in operands: tys.add(infer(o, scope, fns, cols))
        if "DOUBLE" in tys: return "DOUBLE"
        if tys <= {"BIGINT", "INT", "SMALLINT"}: return "BIGINT"
        return None
    return None

def select_items(toks):
    """(expression tokens, alias) of each item of a SELECT's list (the first SELECT at depth 0 after any WITH)."""
    t = [x for x in toks if x[0] not in ("ws", "comment")]
    depth = 0; start = None
    for j, x in enumerate(t):
        if x[1] in ("(",): depth += 1
        elif x[1] in (")",): depth -= 1
        elif depth == 0 and x[1].upper() == "SELECT": start = j + 1; break
    if start is None: return []
    if t[start][1].upper() == "DISTINCT": start += 1
    items, cur, depth = [], [], 0
    for x in t[start:]:
        if x[1] == "(": depth += 1
        elif x[1] == ")": depth -= 1
        if depth == 0 and (x[1].upper() in ("FROM", "WHERE", "GROUP", "ORDER", "LIMIT", "UNION") or x[1] == ","):
            items.append(cur); cur = []
            if x[1] != ",": break
            continue
        cur.append(x)
    else:
        if cur: items.append(cur)
    out = []
    for it in items:
        if len(it) >= 3 and it[-2][1].upper() == "AS": out.append((it[:-2], it[-1][1]))
        elif len(it) >= 3 and it[-2][1] == ".": out.append((it, it[-1][1]))
        elif len(it) == 1: out.append((it, it[0][1]))
        else: out.append((it, None))
    return out

def arg_name(n):
    return "a_" + (n[2:] if n.lower().startswith("p_") else n)

def indented(text, ind):
    """A statement's later lines under its first, at ind."""
    lines = text.split("\n")
    return lines[0] + "".join("\n" + (ind + l if l.strip() else l) for l in lines[1:])

class Emitter:
    def __init__(self, fn, fns, cols, report):
        self.fn, self.fns, self.cols, self.report = fn, fns, cols, report
        self.decls = []          # (myname, type)
        self.scope = {}
        self.nblk = 0
        for pname, ptype in fn.params:
            self.scope[pname.lower()] = {"my": arg_name(pname), "type": my_type(ptype)}
    def declare(self, name, ty, where):
        my = "v_" + name
        taken = {d[0].lower() for d in self.decls}
        n = 2
        while my.lower() in taken: my = "v_%s_%d" % (name, n); n += 1
        if ty is None:
            self.report.append("%s: the type of '%s' is not known; DOUBLE" % (where, name))
            ty = "DOUBLE"
        self.decls.append((my, ty))
        return my
    def stmts(self, nodes, ind, scope):
        out = []
        where = self.fn.name
        for n in nodes:
            for c in n.get("comments", []): out.append(ind + c)
            k = n["k"]
            if k == "comments": continue
            if k == "let":
                ty = infer(n["expr"], scope, self.fns, self.cols)
                my = self.declare(n["name"], ty, where)
                rhs = emit_tokens(n["expr"], scope, self.report, where)
                scope = dict(scope)
                scope[n["name"].lower()] = {"my": my, "type": ty or "DOUBLE"}
                out.append("%sSET %s = %s;" % (ind, my, rhs))
            elif k == "set":
                if n["name"].lower() not in scope: raise SyntaxError("%s: assignment to unknown %s" % (where, n["name"]))
                out.append("%sSET %s = %s;" % (ind, scope[n["name"].lower()]["my"], emit_tokens(n["expr"], scope, self.report, where)))
            elif k == "if":
                out.append("%sIF %s THEN" % (ind, emit_tokens(n["cond"], scope, self.report, where)))
                body, _ = self.stmts(n["body"], ind + "  ", scope)
                out += body if body else [ind + "  DO 0;"]
                out.append(ind + "END IF;")
            elif k == "return":
                e = emit_tokens(n["expr"], scope, self.report, where).strip()
                out.append("%sRETURN %s;" % (ind, e if e else "0"))
            elif k == "call":
                out.append("%sDO %s;" % (ind, emit_tokens(n["expr"], scope, self.report, where)))
            elif k == "sql":
                out.append(ind + indented(emit_tokens(n["sql"], scope, self.report, where).strip(), ind) + ";")
            elif k == "select_block":
                self.nblk += 1
                b = "b%d" % self.nblk
                items = select_items(n["sql"])
                inner = dict(scope)
                binds = []
                for ix, (expr, alias) in enumerate(items):
                    if alias is None: alias = "unused_%d" % ix   # a column the block does not read
                    ty = infer(expr, scope, self.fns, self.cols)
                    my = self.declare(alias, ty, where)
                    inner[alias.lower()] = {"my": my, "type": ty or "DOUBLE"}
                    binds.append(my)
                sel = indented(emit_tokens(n["sql"], scope, self.report, where).strip(), ind + "    ")
                out.append("%sBEGIN" % ind)
                out.append("%s  DECLARE %s_done INT DEFAULT 0;" % (ind, b))
                out.append("%s  DECLARE %s_rows CURSOR FOR %s;" % (ind, b, sel))
                out.append("%s  DECLARE CONTINUE HANDLER FOR NOT FOUND SET %s_done = 1;" % (ind, b))
                out.append("%s  OPEN %s_rows;" % (ind, b))
                out.append("%s  %s_each: LOOP" % (ind, b))
                out.append("%s    FETCH %s_rows INTO %s;" % (ind, b, ", ".join(binds)))
                out.append("%s    IF %s_done THEN LEAVE %s_each; END IF;" % (ind, b, b))
                body, _ = self.stmts(n["body"], ind + "    ", inner)
                out += body
                out.append("%s  END LOOP;" % ind)
                out.append("%s  CLOSE %s_rows;" % (ind, b))
                out.append("%sEND;" % ind)
        return out, scope

def convert_fn(fn, fns, cols, report):
    em = Emitter(fn, fns, cols, report)
    body, _ = em.stmts(fn.body, "  ", em.scope)
    params = ", ".join("%s %s" % (arg_name(n), my_type(t)) for n, t in fn.params)
    ret = my_type(fn.returns) if fn.returns else "INT"
    lines = ["\n".join(fn.comments)] if fn.comments else []
    lines.append("DROP FUNCTION IF EXISTS %s;;" % fn.name)
    lines.append("CREATE FUNCTION %s(%s) RETURNS %s" % (fn.name, params, ret))
    lines.append("BEGIN")
    for my, ty in em.decls: lines.append("  DECLARE %s %s;" % (my, ty))
    lines += body
    if not fn.returns: lines.append("  RETURN 0;")
    lines.append("END;;")
    return "\n".join(x for x in lines if x is not None)

FN_RE = re.compile(r"((?:--[^\n]*\n)*)CREATE OR REPLACE FUNCTION (\w+)\s*\((.*?)\)\s*(?:RETURNS\s+([\w ]+?))?\s*\n?\s*LANGUAGE\s+(\w+)([^$]*?)AS\s+\$(\w*)\$(.*?)\$\7\$;", re.S)
VIEW_RE = re.compile(r"((?:--[^\n]*\n)*)CREATE OR REPLACE VIEW (\w+) AS(.*?);\n", re.S)

def main():
    src_dir, schema, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    cols = schema_columns(schema)
    runtime_py = open(os.path.join(src_dir, "..", "..", "..", "cedarscript_runtime.py")).read()
    order = re.findall(r'^\s*"([0-9a-z_]+\.sql)",', runtime_py.split("RUNTIME_FILES = (")[1].split("\n)\n")[0], re.M)
    texts = [(f, open(os.path.join(src_dir, f)).read()) for f in order]
    fns_ret = {}
    found = []
    for f, s in texts:
        for m in FN_RE.finditer(s):
            name, params, returns = m.group(2), m.group(3), m.group(4)
            fns_ret[name.lower()] = my_type(returns) if returns else "INT"
    report = []
    out = ["-- SQLDoom's game tic (github.com/cedardb/sqldoom, sql/runtime/functions, GPL-2.0) as ASQL stored functions.",
           "-- Converted from CedarScript by a script and then edited by hand where it said to; see README.md. Run under",
           "-- sql_mode 'PIPES_AS_CONCAT,PG_DIVISION' (each function keeps it), as the SQL inside is PostgreSQL's.",
           "-- A statement ends at `;;` in this file.", ""]
    for f, s in texts:
        out.append("-- ==== %s ====" % f)
        pos = 0
        items = sorted([(m.start(), "fn", m) for m in FN_RE.finditer(s)] + [(m.start(), "view", m) for m in VIEW_RE.finditer(s)], key=lambda x: x[0])
        for start, kind, m in items:
            if kind == "view":
                out.append(m.group(1).rstrip("\n"))
                out.append("CREATE OR REPLACE VIEW %s AS%s;;" % (m.group(2), m.group(3)))
                out.append("")
                continue
            name, params, returns, lang, body = m.group(2), m.group(3), m.group(4), m.group(5), m.group(8)
            plist = []
            for prm in [x.strip() for x in params.split(",") if x.strip()]:
                pn, pt = prm.split(None, 1)
                pt = re.sub(r"\s+DEFAULT.*", "", pt, flags=re.I)
                plist.append((pn, pt))
            comments = [c for c in m.group(1).rstrip("\n").split("\n") if c]
            if lang.lower() != "cedarscript":
                report.append("%s: LANGUAGE %s, ported by hand" % (name, lang))
                out.append("-- %s: LANGUAGE %s, ported by hand below\n" % (name, lang))
                continue
            try:
                global CUR_BODY
                CUR_BODY = body
                fn = Fn(name, plist, returns, lex(body), comments)
                out.append(convert_fn(fn, fns_ret, cols, report))
            except Exception as e:
                report.append("%s: NOT CONVERTED: %s" % (name, e))
                out.append("-- %s: NOT CONVERTED (%s)\n" % (name, e))
            out.append("")
    open(out_path, "w").write("\n".join(out) + "\n")
    print("\n".join(report))
    print(len(report), "notes", file=sys.stderr)

main()
