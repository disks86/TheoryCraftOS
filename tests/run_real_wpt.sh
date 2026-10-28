#!/usr/bin/env bash
# Runs files of the real web-platform-tests (https://github.com/web-platform-tests/wpt) in PageTest and summarizes the results.
#
#   tests/run_real_wpt.sh <wpt checkout> <out dir> <dir or file in the checkout>...
#
# The checkout can be sparse: git clone --depth 1 --filter=blob:none --sparse https://github.com/web-platform-tests/wpt.git
#                             git -C wpt sparse-checkout set resources common dom url encoding html/dom
# PAGETEST is the PageTest binary (default build/PageTest; tests/run_host_tests.sh PageTest builds one). One result file per test file goes to <out dir>
# (lines "WPT PASS|FAIL|TIMEOUT|NOTRUN <name> :: <message>" and "WPTDONE <status>"); summary.txt has the counts per file. Each file runs in its own process with a
# 60 second limit. Pages (.html), .window.js and .any.js files run; manual tests, https-only tests, crash tests and idlharness files are left out.
set -e
root=$(cd "$1" && pwd); out=$2; shift 2
mkdir -p "$out"; out=$(cd "$out" && pwd)
export root out PAGETEST=${PAGETEST:-build/PageTest}
cd "$root"
for t in "$@"; do find "$t" -type f \( -name '*.html' -o -name '*.htm' -o -name '*.window.js' -o -name '*.any.js' \); done \
  | grep -v -e '/resources/' -e '-manual' -e 'support/' -e '\.https\.' -e 'crashtests' -e '\-ref\.' -e '/reference/' -e 'idlharness' | sort > "$out/pages.txt"
# a page with <meta name=variant content="?1-1000"> is run once per variant (the page address has the variant)
python3 - "$out" <<'PY'
import sys, re
out = sys.argv[1]
rows = []
for name in open(out + '/pages.txt').read().split('\n'):
    if not name: continue
    variants = re.findall(r'<meta\s+name=["\']variant["\']\s+content=["\']([^"\']*)["\']', open(name, errors='replace').read()) if name.endswith(('.html', '.htm')) else []
    kept = [v for v in variants if v and v[0] in '?#']
    rows.extend([name + v for v in kept] if kept else [name])
open(out + '/files.txt', 'w').write('\n'.join(rows) + '\n')
PY
tr '\n' '\0' < "$out/files.txt" | xargs -0 -P "${JOBS:-12}" -I{} bash -c 'f=$1; o=$out/$(echo "$f" | tr "/?&=#" "_____").txt; WPT_ROOT=$root WPT_REAL_FILE=$f timeout 60 $PAGETEST > $o 2>&1; echo "RC $?" >> $o' _ {} || true
python3 - "$out" <<'PY'
import sys, os, glob
out = sys.argv[1]
rows = []
for f in sorted(glob.glob(out + '/*.txt')):
    if os.path.basename(f) in ('files.txt', 'pages.txt', 'summary.txt'): continue
    passed = failed = 0
    lines = open(f, errors='replace').read().splitlines()
    for l in lines:
        if l.startswith('WPT PASS'): passed += 1
        elif l.startswith('WPT '): failed += 1
    done = [l for l in lines if l.startswith('WPTDONE') and not l.startswith('WPTDONE NOFINISH')]
    rows.append((os.path.basename(f)[:-4], passed, failed, done[0][8:] if done else 'DID NOT FINISH'))
open(out + '/summary.txt', 'w').write('\n'.join('%s pass=%d fail=%d %s' % r for r in rows) + '\n')
print('files %d, subtests passed %d, failed %d, files that did not finish %d' % (len(rows), sum(r[1] for r in rows), sum(r[2] for r in rows), sum(1 for r in rows if r[3] == 'DID NOT FINISH')))
PY
