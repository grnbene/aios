#!/usr/bin/env python3
"""Reference for the MIME reader: the functions the old Python IMAP worker used to turn a message into
a mail record, kept verbatim so tools/mime-check.cpp can be held against them. Test-only; the product has
no Python in it."""
import base64, email, email.header, email.utils, json, re, sys, time

def decode_mime(val):
    if not val:
        return ''
    out = []
    for part, enc in email.header.decode_header(val):
        if isinstance(part, bytes):
            try:
                out.append(part.decode(enc or 'utf-8', errors='replace'))
            except Exception:
                out.append(part.decode('latin1', errors='replace'))
        else:
            out.append(str(part))

    return ' '.join(' '.join(out).split())


def clean_html(text):
    text = re.sub(r'<style.*?</style>', '', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<script.*?</script>', '', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'<br\s*/?>', '\n', text, flags=re.IGNORECASE)
    text = re.sub(r'</p>', '\n\n', text, flags=re.IGNORECASE)
    text = re.sub(r'<[^>]+>', '', text)
    for ent, ch in (('&nbsp;', ' '), ('&amp;', '&'), ('&lt;', '<'), ('&gt;', '>'), ('&quot;', '"')):
        text = text.replace(ent, ch)
    return text.strip()


def inline_cid_images(html, parts):
    """Replace src="cid:XXX" with the part's own bytes as a data: url.

    An inline image -- a signature logo, a pasted screenshot -- is carried IN
    the message as a MIME part and referenced by Content-ID. Nothing can fetch
    a cid: url over a network; it only means anything next to the message it
    came from. Folding the bytes in here means the stored html is complete on
    its own, the renderer needs no attachment plumbing, and displaying it costs
    no request. In this mailbox 334 messages use cid: images against 120 using
    remote ones, so this is the common case and not the exotic one.
    """
    if not html or not parts:
        return html
    for cid, (mime, payload) in parts.items():
        if not payload:
            continue
        url = 'data:%s;base64,%s' % (mime, base64.b64encode(payload).decode('ascii'))
        for ref in ('cid:' + cid, 'cid:' + cid.upper(), 'cid:' + cid.lower()):
            html = html.replace('"' + ref + '"', '"' + url + '"')
            html = html.replace("'" + ref + "'", "'" + url + "'")
    return html


def body_and_attachments(msg):
    """Plain text for the list and for search, and the ORIGINAL html untouched.


    This used to return only clean_html(...) -- the regex-stripped text -- and
    that was all anyone stored, so the real message was destroyed at sync time
    and there was nothing left to render however good a renderer you had. The
    html part now comes back beside the text, exactly as the sender wrote it.
    """
    body, html, attachments = '', '', []
    cid_parts = {}
    if msg.is_multipart():
        for part in msg.walk():
            if part.get_content_maintype() == 'multipart':
                continue
            ctype = part.get_content_type()
            fname = part.get_filename()
            cid = str(part.get('Content-ID', '')).strip('<> \t\r\n')
            if cid and ctype.startswith('image/'):
                cid_parts[cid] = (ctype, part.get_payload(decode=True) or b'')
            if fname:
                payload = part.get_payload(decode=True) or b''
                attachments.append({'filename': decode_mime(fname),
                                    'size_bytes': len(payload),
                                    'mime_type': ctype,
                                    'content_id': cid})
                continue
            if 'attachment' in str(part.get('Content-Disposition', '')):
                continue
            payload = part.get_payload(decode=True)
            if not payload:
                continue
            text = payload.decode(part.get_content_charset() or 'utf-8', errors='replace')
            # Keep the best of each: the first text/plain as the body, the first
            # text/html as html. A multipart/alternative carries both and they
            # are the same message said twice.
            if ctype == 'text/html':
                if not html:
                    html = text
                if not body:
                    body = clean_html(text)
            elif ctype == 'text/plain' and not body:
                body = text
    else:

        payload = msg.get_payload(decode=True)
        if payload:
            text = payload.decode(msg.get_content_charset() or 'utf-8', errors='replace')
            if msg.get_content_type() == 'text/html':
                html, body = text, clean_html(text)
            else:
                body = text
    return body, inline_cid_images(html, cid_parts), attachments


def envelope_from(raw, uid, flags, body_text, attachments, have_body, fallback_ts=0, html_text=''):
    msg = email.message_from_bytes(raw)
    date_hdr = msg.get('Date', '')
    try:
        parsed = email.utils.parsedate_to_datetime(date_hdr) if date_hdr else None
    except Exception:
        parsed = None
    ts = int(parsed.timestamp()) if parsed else int(fallback_ts)
    from_name, from_addr = email.utils.parseaddr(decode_mime(msg.get('From', '')))
    return {
        'uid': uid,
        'message_id': ' '.join(str(msg.get('Message-ID', '')).split()),
        'from_name': decode_mime(from_name) or from_addr,
        'from_addr': from_addr,
        'to': decode_mime(msg.get('To', '')),
        'cc': decode_mime(msg.get('Cc', '')),
        'subject': decode_mime(msg.get('Subject', '(No Subject)')),
        'date_str': (parsed.strftime('%b %d %Y') if parsed
                     else (time.strftime('%b %d %Y', time.localtime(ts)) if ts else 'Unknown')),
        'timestamp': ts,
        'snippet': ' '.join(body_text.split())[:120],
        # Only carry the body when we actually pulled the whole message; a
        # 2KB peek must never be stored as if it were the message.
        'body': body_text if have_body else '',
        'have_body': have_body,
        'has_html': bool(html_text),
        'is_unread': '\\Seen' not in flags,
        'is_flagged': '\\Flagged' in flags,
        'attachments': attachments,
    }



