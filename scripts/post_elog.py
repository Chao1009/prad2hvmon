#!/usr/bin/env python3
"""
post_elog.py — manually post a saved local auto-report to the JLab logbook.

Mirrors the prad2hvd logic, but date-bucketed instead of run-bucketed
(prad2hvd is a continuous HV monitor; there are no runs).

  * Dedup check: GET <elog_url>/api/elog/entries?book=<book>&title=...&
      field=lognumber&field=title&limit=20      (server: handleElogCheck)
    Substring server-side, EXACT title match client-side.

  * Post:        PUT <elog_url>/incoming/prad2hv_<UTCstamp>.xml with the
                 XML body, cert + key auth.

Stdlib only — no `requests`, no `pip install`.

Usage:
  ./post_elog.py 2026-05-09                      # latest XML in 2026-05-09/
  ./post_elog.py path/to/report.xml              # explicit path
  ./post_elog.py 2026-05-09 --check-only         # dedup-check only, no post
  ./post_elog.py path/to/report.xml --force      # skip dedup check
"""

import argparse
import datetime as _dt
import json
import re
import ssl
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

# Defaults match the daemon's gui_config.json defaults.  Override via
# flags or by pointing --reports-dir at a different archive.
DEFAULT_REPORTS_DIR = "/home/clasrun/prad2hv_daq/monitor/reports"
DEFAULT_CERT        = "/home/clasrun/prad2_daq/monitor/keys/elog-cert.pem"
DEFAULT_KEY         = "/home/clasrun/prad2_daq/monitor/keys/elog-key.pem"
DEFAULT_URL         = "https://logbooks.jlab.org"
DEFAULT_BOOK        = "PRADLOG"
TIMEOUT_S           = 30

# Tag substitutions applied to every upload so old saved XMLs whose
# <Tags> block predates the elog enum tightening still post cleanly.
# Empty NEW values drop the entire <tag>…</tag> block — used to strip
# stale tags that aren't in PRADLOG's enum anymore.
DEFAULT_TAG_REWRITES = {
    "AutoReport":      "Autolog",
    "PRad2":           "DAQ",
    # Reasons-as-tags from older daemons:
    "scheduled":       "",
    "manual":          "",
    "fault":           "",
}


def find_xml_in_bucket(reports_dir: str, bucket: str):
    """Latest report_*.xml in <reports_dir>/<bucket>/, or None."""
    bucket_dir = Path(reports_dir) / bucket
    if not bucket_dir.is_dir():
        return None
    xmls = sorted(bucket_dir.glob("report_*.xml"),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    return xmls[0] if xmls else None


def is_iso_date(s: str) -> bool:
    """True if s looks like YYYY-MM-DD (the bucket key on disk)."""
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", s):
        return False
    try:
        _dt.date.fromisoformat(s)
        return True
    except ValueError:
        return False


def title_from_xml(xml_path: Path):
    """Extract the <title>…</title> from a saved elog XML so we can dedup
    against the live logbook by exact match.  Returns the title string,
    or None if the XML is malformed / has no title."""
    try:
        text = Path(xml_path).read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    m = re.search(r"<title>([^<]*)</title>", text)
    return m.group(1) if m else None


def make_ssl_ctx(cert: str, key: str) -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    return ctx


def check_elog(url: str, book: str, cert: str, key: str, title: str):
    """Mirror handleElogCheck — exact-title match against the live logbook."""
    qs = urllib.parse.urlencode([
        ("book",  book),
        ("title", title),
        ("field", "lognumber"),
        ("field", "title"),
        ("limit", "20"),
    ])
    full = f"{url}/api/elog/entries?{qs}"
    ctx = make_ssl_ctx(cert, key)
    req = urllib.request.Request(full, method="GET")
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=TIMEOUT_S) as r:
            body = r.read().decode("utf-8", errors="replace")
            code = r.status
    except urllib.error.HTTPError as e:
        return {"checked": False, "detail": f"HTTP {e.code}"}
    except Exception as e:
        return {"checked": False, "detail": f"network: {e}"}
    if code != 200:
        return {"checked": False, "detail": f"HTTP {code}"}
    try:
        j = json.loads(body)
    except json.JSONDecodeError as e:
        return {"checked": False, "detail": f"json: {e}"}
    if j.get("stat") != "ok":
        return {"checked": False, "detail": j.get("message", "non-ok stat")}
    entries = j.get("data", {}).get("entries", [])
    for e in entries:
        if e.get("title") == title:
            return {"checked": True, "exists": True,
                    "lognumber": e.get("lognumber"),
                    "matched_count": len(entries)}
    return {"checked": True, "exists": False,
            "matched_count": len(entries)}


def rewrite_tags(xml_text: str, mapping: dict) -> str:
    """Apply OLD=NEW rewrites inside <tag>…</tag>.  Empty NEW drops the
    block entirely.  Match is exact-text inside <tag> so body content
    can't accidentally trip the substitution."""
    if not mapping:
        return xml_text

    def _sub(m):
        old = m.group(1)
        if old not in mapping:
            return m.group(0)
        new = mapping[old]
        return "" if new == "" else f"<tag>{new}</tag>"

    out = re.sub(r"<tag>([^<]*)</tag>", _sub, xml_text)
    out = re.sub(r"<Tags>\s*</Tags>\s*", "", out)
    out = re.sub(r"(<Tags>)(\s*\n)+", r"\1\n", out)
    return out


def post_elog(url: str, cert: str, key: str, xml_path: Path,
              rewrite_map: dict = None):
    """Mirror handleElogPost — shells out to curl --upload-file (the
    same command prad2hvd runs).  /incoming/<name> is one-shot on the
    elog side, so the timestamped basename doubles as a per-attempt
    uniqueness guard.  Apache on logbooks.jlab.org is picky about the
    request shape (Expect: 100-continue, …); using curl directly avoids
    tripping urllib's PUT differences."""
    upload_name = "prad2hv_" + Path(xml_path).name.replace("report_", "")
    full = f"{url}/incoming/{upload_name}"
    marker = "___HTTP_CODE___"

    if rewrite_map:
        import tempfile
        text = Path(xml_path).read_text(encoding="utf-8")
        text = rewrite_tags(text, rewrite_map)
        tf = tempfile.NamedTemporaryFile(mode="w", suffix=".xml",
                                         delete=False, encoding="utf-8")
        tf.write(text); tf.close()
        upload_src = tf.name
    else:
        upload_src = str(xml_path)

    cmd = [
        "curl", "-sS",
        "--cert", cert, "--key", key,
        "--upload-file", upload_src,
        full,
        "-w", f"\n{marker}%{{http_code}}",
        "-m", str(TIMEOUT_S),
    ]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=TIMEOUT_S + 5)
    finally:
        if rewrite_map:
            try: Path(upload_src).unlink()
            except Exception: pass
    if isinstance(p, Exception):
        return 0, str(p)
    out = (p.stdout or "") + (p.stderr or "")
    m = re.search(re.escape(marker) + r"(\d+)\s*$", out)
    if not m:
        return 0, out.strip() or "no HTTP code from curl"
    code = int(m.group(1))
    body = out[:m.start()].rstrip()
    return code, body


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target",
        help="UTC date bucket (YYYY-MM-DD), or a path to a saved report.xml")
    ap.add_argument("--reports-dir", default=DEFAULT_REPORTS_DIR)
    ap.add_argument("--cert", default=DEFAULT_CERT)
    ap.add_argument("--key",  default=DEFAULT_KEY)
    ap.add_argument("--url",  default=DEFAULT_URL)
    ap.add_argument("--book", default=DEFAULT_BOOK)
    ap.add_argument("--force", action="store_true",
        help="skip the dedup check (post even if already in elog)")
    ap.add_argument("--check-only", action="store_true",
        help="print dedup result and exit; do not post")
    ap.add_argument("--rewrite-tag", action="append", default=[],
        metavar="OLD=NEW",
        help="extra tag rewrite to apply before upload (repeatable). "
             f"defaults already cover {DEFAULT_TAG_REWRITES}")
    ap.add_argument("--no-rewrite-tags", action="store_true",
        help="disable the default tag rewrites (post the XML verbatim)")
    args = ap.parse_args(argv)

    # Resolve XML path: explicit file → use it; bucket date → pick the
    # latest XML in <reports_dir>/<bucket>/.
    if Path(args.target).is_file():
        xml_path = Path(args.target)
    elif is_iso_date(args.target):
        xml_path = find_xml_in_bucket(args.reports_dir, args.target)
        if xml_path is None:
            sys.exit(f"no XML found in {args.reports_dir}/{args.target}/")
    else:
        sys.exit(f"target '{args.target}' is neither YYYY-MM-DD nor a file path")

    title = title_from_xml(xml_path)
    if not title:
        sys.exit(f"could not read <title> from {xml_path}")

    print(f"title : {title}")
    print(f"xml   : {xml_path}")

    if not args.force:
        print("checking elog...")
        c = check_elog(args.url, args.book, args.cert, args.key, title)
        if not c.get("checked"):
            print(f"  dedup unavailable: {c.get('detail')}")
            if not args.check_only:
                # Match the daemon's fail-closed policy on uncertain dedup.
                sys.exit("dedup couldn't run — refusing to post "
                         "(re-run with --force to override)")
        elif c.get("exists"):
            print(f"  ALREADY IN ELOG: lognumber={c.get('lognumber')}  (skipping)")
            sys.exit(0)
        else:
            print(f"  not in elog ({c.get('matched_count')} fuzzy hits)")
        if args.check_only:
            return

    print("posting...")
    rewrite_map = {} if args.no_rewrite_tags else dict(DEFAULT_TAG_REWRITES)
    for spec in args.rewrite_tag:
        if "=" not in spec:
            sys.exit(f"--rewrite-tag expects OLD=NEW, got '{spec}'")
        old, new = spec.split("=", 1)
        rewrite_map[old.strip()] = new.strip()
    if rewrite_map:
        print(f"tag rewrites: {rewrite_map}")
    code, body = post_elog(args.url, args.cert, args.key, xml_path,
                           rewrite_map=rewrite_map or None)
    print(f"HTTP {code}")
    if body:
        print(body[:1000])
    sys.exit(0 if code in (200, 201) else 1)


if __name__ == "__main__":
    main()
