#!/usr/bin/env python3
"""Holds tools/mime-check.cpp (the in-tree MIME reader) against Python's `email` package, using the
functions the old IMAP worker used (scripts/mime-reference.py), over a corpus of messages written to cover
what real mail does: folded and RFC 2047 headers, quoted-printable and base64, charsets, nested multipart,
attachments with RFC 2231 names, cid images, odd dates, LF-only line endings, missing boundaries.

    python3 scripts/mime-diff.py build/aios-mime-check

Exits non-zero with the field-by-field differences when the two disagree. Test-only; no Python in the product."""
import base64, email, email.message, email.policy, json, os, subprocess, sys, tempfile, importlib.util, textwrap

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ref", os.path.join(here, "mime-reference.py"))
ref = importlib.util.module_from_spec(spec); spec.loader.exec_module(ref)

def crlf(s): return s.replace("\r\n", "\n").replace("\n", "\r\n").encode("latin1")
def b64(b): return base64.encodebytes(b).decode()

PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==")
cases = {}

cases["plain"] = crlf("From: Alice Example <alice@example.com>\nTo: bob@example.com\nSubject: Hello\nDate: Tue, 1 Jul 2003 10:52:37 +0200\nMessage-ID: <1@x>\n\nHello Bob\n")
cases["nosubject"] = crlf("From: a@b.c\nDate: Wed, 02 Oct 2002 08:00:00 EST\n\nbody\n")
cases["folded"] = crlf("From: =?UTF-8?B?w4lsw6g=?= <e@x.org>\nTo: a@x.org,\n   b@x.org,\n \"Last, First\" <c@x.org>\nSubject: a very\n long subject that\n\tis folded\nDate: Thu, 3 Oct 2002 08:00:00 GMT\n\nx\n")
cases["rfc2047"] = crlf("From: \"=?iso-8859-1?Q?Andr=E9_Dupont?=\" <a@x.fr>\nSubject: =?utf-8?Q?Caf=C3=A9_=E2=82=AC?= =?utf-8?B?IGFuZCBtb3Jl?=\nDate: Fri, 4 Oct 2002 08:00:00 +0000\n\nok\n")
cases["rfc2047-adjacent"] = crlf("From: a@x\nSubject: =?UTF-8?B?SGVsbG8g?= =?UTF-8?B?V29ybGQ=?=\nDate: Fri, 4 Oct 2002 08:00:00 +0000\n\nok\n")
cases["qp-body"] = crlf("From: a@x\nSubject: qp\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/plain; charset=utf-8\nContent-Transfer-Encoding: quoted-printable\n\nCaf=C3=A9 costs =E2=82=AC5 and this line is long enough to need a soft=\n break here\n")
cases["latin1-body"] = crlf("From: a@x\nSubject: l1\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/plain; charset=iso-8859-1\nContent-Transfer-Encoding: 8bit\n\n").replace(b"8bit\r\n\r\n", b"8bit\r\n\r\n") + "Voilà l'été\r\n".encode("latin1")
cases["cp1252-body"] = crlf("From: a@x\nSubject: w\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/plain; charset=windows-1252\nContent-Transfer-Encoding: 8bit\n\n") + b"smart \x93quotes\x94 \x80 and \x96 dash\r\n"
cases["koi8"] = crlf("From: a@x\nSubject: k\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/plain; charset=koi8-r\nContent-Transfer-Encoding: 8bit\n\n") + "Привет, мир\r\n".encode("koi8-r")
cases["cp1251"] = crlf("From: a@x\nSubject: c\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/plain; charset=windows-1251\nContent-Transfer-Encoding: base64\n\n") + b64("Привет, мир".encode("cp1251")).encode()
cases["latin2"] = crlf("From: a@x\nSubject: c2\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/plain; charset=iso-8859-2\nContent-Transfer-Encoding: 8bit\n\n") + "Zażółć gęślą jaźń\r\n".encode("iso8859_2")
cases["html-only"] = crlf("From: a@x\nSubject: h\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: text/html; charset=utf-8\n\n<html><head><style>p{color:red}</style></head><body><p>Hello&nbsp;<b>there</b> &amp; welcome</p><script>alert(1)</script>line<br>two<br/>three</body></html>\n")
cases["alternative"] = crlf("""From: a@x
Subject: alt
Date: Fri, 4 Oct 2002 08:00:00 +0000
MIME-Version: 1.0
Content-Type: multipart/alternative; boundary="BB"

--BB
Content-Type: text/plain; charset=utf-8

plain version
--BB
Content-Type: text/html; charset=utf-8

<p>html <i>version</i></p>
--BB--
""")
cases["alternative-html-first"] = crlf("""From: a@x
Subject: alt2
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/alternative; boundary=BB

--BB
Content-Type: text/html

<p>html first</p>
--BB
Content-Type: text/plain

plain second
--BB--
""")
cases["mixed-attach"] = crlf("""From: a@x
Subject: att
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/mixed; boundary="MM"

--MM
Content-Type: multipart/alternative; boundary="AA"

--AA
Content-Type: text/plain; charset=utf-8

see attached
--AA
Content-Type: text/html; charset=utf-8

<p>see attached</p>
--AA--
--MM
Content-Type: application/pdf; name="report.pdf"
Content-Disposition: attachment; filename="report.pdf"
Content-Transfer-Encoding: base64

""" + b64(b"%PDF-1.4 fake pdf bytes " * 20) + """
--MM
Content-Type: text/plain; name="notes.txt"
Content-Disposition: attachment; filename*=utf-8''caf%C3%A9%20notes.txt
Content-Transfer-Encoding: 7bit

attachment text
--MM--
""")
cases["rfc2231-continued"] = crlf("""From: a@x
Subject: long names
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/mixed; boundary="MM"

--MM
Content-Type: text/plain

body
--MM
Content-Type: application/octet-stream
Content-Disposition: attachment;
 filename*0*=utf-8''a%20very%20long;
 filename*1*=%20name%20%E2%82%AC.bin
Content-Transfer-Encoding: base64

AAECAwQ=
--MM--
""")
cases["encoded-filename"] = crlf("""From: a@x
Subject: enc
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/mixed; boundary="MM"

--MM
Content-Type: text/plain

body
--MM
Content-Type: image/png; name="=?utf-8?B?5pel5pys6KqeLnBuZw==?="
Content-Disposition: attachment; filename="=?utf-8?B?5pel5pys6KqeLnBuZw==?="
Content-Transfer-Encoding: base64

""" + b64(PNG) + """
--MM--
""")
cases["cid-image"] = crlf("""From: a@x
Subject: cid
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/related; boundary="RR"

--RR
Content-Type: text/html; charset=utf-8

<html><body><img src="cid:logo123"> and <img src='cid:LOGO123'></body></html>
--RR
Content-Type: image/png
Content-ID: <logo123>
Content-Transfer-Encoding: base64

""" + b64(PNG) + """
--RR--
""")
cases["lf-only"] = b"From: a@x\nSubject: lf\nDate: Fri, 4 Oct 2002 08:00:00 +0000\nContent-Type: multipart/alternative; boundary=zz\n\n--zz\nContent-Type: text/plain\n\nline1\nline2\n--zz--\n"
cases["no-closing-boundary"] = crlf("""From: a@x
Subject: trunc
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/alternative; boundary="zz"

--zz
Content-Type: text/plain

cut off
""")
cases["date-comment"] = crlf("From: a@x\nSubject: d\nDate: Mon, 7 Oct 2002 13:22:01 -0700 (PDT)\n\nx\n")
cases["date-2digit"] = crlf("From: a@x\nSubject: d\nDate: 7 Oct 02 13:22 GMT\n\nx\n")
cases["date-bad"] = crlf("From: a@x\nSubject: d\nDate: sometime last week\n\nx\n")
cases["date-nodate"] = crlf("From: a@x\nSubject: d\n\nx\n")
cases["date-zone-name"] = crlf("From: a@x\nSubject: d\nDate: Tue, 1 Jul 2003 10:52:37 EDT\n\nx\n")
cases["from-comment"] = crlf("From: bob@example.com (Bob Builder)\nSubject: d\nDate: Tue, 1 Jul 2003 10:52:37 +0000\n\nx\n")
cases["from-quoted"] = crlf("From: \"Smith, John\" <j@x.com>\nTo: \"A\" <a@x>, B <b@x>\nCc: c@x\nSubject: d\nDate: Tue, 1 Jul 2003 10:52:37 +0000\n\nx\n")
cases["empty-body"] = crlf("From: a@x\nSubject: empty\nDate: Tue, 1 Jul 2003 10:52:37 +0000\n\n")
cases["base64-body"] = crlf("From: a@x\nSubject: b64\nDate: Tue, 1 Jul 2003 10:52:37 +0000\nContent-Type: text/plain; charset=utf-8\nContent-Transfer-Encoding: base64\n\n") + b64("héllo wörld ✓".encode()).encode()
cases["inline-no-name"] = crlf("""From: a@x
Subject: inl
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/mixed; boundary="M"

--M
Content-Type: text/plain

main
--M
Content-Type: text/plain
Content-Disposition: attachment

attached text without a filename
--M--
""")
cases["forwarded"] = crlf("""From: a@x
Subject: fwd
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/mixed; boundary="M"

--M
Content-Type: message/rfc822

From: inner@x
Subject: inner
Content-Type: text/plain

the inner body
--M--
""")
cases["nested-deep"] = crlf("""From: a@x
Subject: deep
Date: Fri, 4 Oct 2002 08:00:00 +0000
Content-Type: multipart/mixed; boundary="O"

--O
Content-Type: multipart/related; boundary="I"

--I
Content-Type: multipart/alternative; boundary="A"

--A
Content-Type: text/plain

deep plain
--A
Content-Type: text/html

<b>deep html</b>
--A--
--I--
--O--
""")
cases["utf8-8bit"] = crlf("From: a@x\nSubject: u\nDate: Tue, 1 Jul 2003 10:52:37 +0000\nContent-Type: text/plain; charset=utf-8\nContent-Transfer-Encoding: 8bit\n\n") + "日本語のテキスト ✓ é\r\n".encode()
cases["bad-utf8-label"] = crlf("From: a@x\nSubject: u\nDate: Tue, 1 Jul 2003 10:52:37 +0000\nContent-Type: text/plain; charset=utf-8\n\n") + b"ok \xe9 bad byte\r\n"

def reference(raw):
    msg = email.message_from_bytes(raw)
    body, html, atts = ref.body_and_attachments(msg)
    r = ref.envelope_from(raw, 1, [], body, atts, True, 0, html)
    r["html"] = html
    return r

FIELDS = ["message_id", "from_name", "from_addr", "to", "cc", "subject", "timestamp", "date_str", "snippet", "body", "html"]

def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "build/aios-mime-check"
    with tempfile.TemporaryDirectory() as d:
        paths = []
        for name, raw in cases.items():
            p = os.path.join(d, name + ".eml")
            open(p, "wb").write(raw); paths.append(p)
        out = subprocess.run([exe] + paths, capture_output=True, check=True).stdout.decode("utf-8", "replace")
    mine = {json.loads(l)["file"].rsplit("/", 1)[1][:-4]: json.loads(l) for l in out.splitlines()}
    bad = 0
    for name, raw in cases.items():
        want, got = reference(raw), mine[name]
        diffs = []
        for f in FIELDS:
            if want[f] != got[f]: diffs.append((f, want[f], got[f]))
        wa = [(a["filename"], a["size_bytes"], a["mime_type"], a["content_id"]) for a in want["attachments"]]
        ga = [(a["filename"], a["size_bytes"], a["mime_type"], a["content_id"]) for a in got["attachments"]]
        if wa != ga: diffs.append(("attachments", wa, ga))
        if diffs:
            bad += 1
            print("DIFF", name)
            for f, w, g in diffs: print("   %-12s python=%r\n   %-12s   aios=%r" % (f, w, "", g))
    # the Date: header writer (no strftime on bare metal): the same text email.utils makes, for ordinary and awkward epochs
    import email.utils
    epochs = [0, 1, 86399, 86400, 951782400, 951868800, 1026899065, 1760633449, 1893456000, 4102444799, -1, -86400, -2208988800, 1709164800]
    got = subprocess.run([exe, "--date"] + [str(e) for e in epochs], capture_output=True, check=True).stdout.decode().splitlines()
    for e, g in zip(epochs, got):
        want = email.utils.formatdate(e, usegmt=False).replace("-0000", "+0000")
        if g != want:
            bad += 1; print("DATE differs for %d: python=%r aios=%r" % (e, want, g))
    print("%d cases + %d dates, %d differ" % (len(cases), len(epochs), bad))
    return 1 if bad else 0

sys.exit(main())
