#!/usr/bin/env python3
"""Saves a web page as one self-contained HTML file for browser layout checks: its linked style sheets (media that does not apply to a screen
is skipped, @import is followed one level) are put into <style> elements in place of the <link> elements, and its scripts are dropped.
The result is what the layout engine is judged on without the network or a script engine in the way.

Usage: scripts/fetch_page.py <url> <output.html>
"""
import re, sys, urllib.parse, urllib.request

USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) TheoryCraftOS"


def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    return urllib.request.urlopen(request, timeout=20).read().decode("utf8", "replace")


def with_imports(css, base, depth=1):
    def follow(match):
        if depth <= 0:
            return ""
        target = match.group(1) or match.group(2)
        try:
            return with_imports(get(urllib.parse.urljoin(base, target)), urllib.parse.urljoin(base, target), depth - 1)
        except Exception:
            return ""
    return re.sub(r"@import\s+(?:url\(\s*['\"]?([^'\")]+)['\"]?\s*\)|['\"]([^'\"]+)['\"])[^;]*;", follow, css)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    url, out = sys.argv[1], sys.argv[2]
    html = get(url)

    def inline_link(match):
        tag = match.group(0)
        if "stylesheet" not in tag.lower():
            return tag
        media = re.search(r'media=["\']([^"\']*)', tag)
        if media and re.fullmatch(r"\s*print\s*", media.group(1)):
            return ""
        href = re.search(r'href=["\']([^"\']+)', tag)
        if not href:
            return tag
        target = urllib.parse.urljoin(url, href.group(1).replace("&amp;", "&"))
        try:
            return "<style>" + with_imports(get(target), target) + "</style>"
        except Exception:
            return tag

    html = re.sub(r"<link\b[^>]*>", inline_link, html)
    html = re.sub(r"<script\b.*?</script>", "", html, flags=re.S | re.I)
    with open(out, "w", encoding="utf8") as f:
        f.write(html)


if __name__ == "__main__":
    main()
