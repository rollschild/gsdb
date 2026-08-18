#!/usr/bin/env bash
# Scan explanation docs for references into the source tree and report, for each
# one, what the referenced location currently contains.
#
# Emits a human/model-readable report. It never modifies anything.
#
# Usage:
#   scan-refs.sh [doc ...]      # default scope: .claude/*.md .codex/*.md ./*.md
#
# Report legend (per reference):
#   OK        file exists, line in range          -> compare the shown text yourself
#   NOFILE    no such file in the repo            -> probably an illustrative example
#   AMBIG     bare filename matches >1 repo file  -> resolve by hand
#   RANGE     line number past end of file        -> definitely stale
#   MOVED     file changed since the doc's last commit -> re-verify every ref into it

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 1

EXT='cpp|hpp|h|inc|s|txt|cmake|nix|json'
REF_RE="([A-Za-z0-9_./+-]*/)?[A-Za-z0-9_+-]+\.($EXT):[0-9]+(-[0-9]+)?"
PROSE_RE='[Ll]ines?[[:space:]]+[0-9]+([[:space:]]*[-–—][[:space:]]*[0-9]+)?'

if [ "$#" -gt 0 ]; then
    docs=("$@")
else
    mapfile -t docs < <(git ls-files '*.md' ':!:build/**' | grep -vE '^(CLAUDE)\.md$')
fi

# ---- resolve a referenced path to a repo file -------------------------------
resolve() {
    local ref="$1"
    # exact repo-root-relative hit wins
    [ -f "$ref" ] && { printf '%s' "$ref"; return 0; }
    # otherwise suffix-match against tracked files: handles both a bare
    # "target.cpp" and a partial path like "detail/dwarf.h"
    local -a hits
    mapfile -t hits < <(git ls-files | grep -E "(^|/)$(printf '%s' "$ref" | sed 's/[].[*^$+?(){}|\\]/\\&/g')$")
    case "${#hits[@]}" in
        0) return 1 ;;
        1) printf '%s' "${hits[0]}"; return 0 ;;
        *) printf '%s' "AMBIG:${hits[*]}"; return 2 ;;
    esac
}

show_line() {  # file, lineno -> "  <n>| <text>"
    local f="$1" n="$2"
    local txt
    txt="$(awk -v n="$n" 'NR==n{print; exit}' "$f")"
    printf '      %6s| %s\n' "$n" "$txt"
}

total=0; stale=0
declare -A CHANGED_CACHE=()

for doc in "${docs[@]}"; do
    [ -f "$doc" ] || continue
    refs="$(grep -onE "$REF_RE" "$doc" 2>/dev/null | sort -u -t: -k1,1n)"
    prose="$(grep -onE "$PROSE_RE" "$doc" 2>/dev/null)"
    [ -z "$refs" ] && [ -z "$prose" ] && continue

    doc_commit="$(git log -1 --format=%H -- "$doc" 2>/dev/null)"
    doc_date="$(git log -1 --format=%ad --date=short -- "$doc" 2>/dev/null)"
    printf '\n══════════════════════════════════════════════════════════════════\n'
    printf 'DOC  %s\n' "$doc"
    printf '     last committed: %s %s   -> diff source since then with:\n' \
        "${doc_date:-uncommitted}" "$(git log -1 --format=%h -- "$doc" 2>/dev/null)"
    printf '     git diff %s HEAD -- <source-file>\n' \
        "${doc_commit:-HEAD}"
    printf '══════════════════════════════════════════════════════════════════\n'

    while IFS= read -r hit; do
        [ -z "$hit" ] && continue
        docline="${hit%%:*}"
        ref="${hit#*:}"
        # split "path:N" / "path:N-M"
        loc="${ref##*:}"
        path="${ref%:*}"
        start="${loc%%-*}"
        end="${loc##*-}"
        total=$((total + 1))

        resolved="$(resolve "$path")"
        rc=$?
        if [ "$rc" -eq 1 ]; then
            printf '  [NOFILE]  %s:%s  ->  %s   (illustrative example? verify)\n' \
                "$doc" "$docline" "$ref"
            stale=$((stale + 1))
            continue
        fi
        if [ "$rc" -eq 2 ]; then
            printf '  [AMBIG ]  %s:%s  ->  %s   candidates: %s\n' \
                "$doc" "$docline" "$ref" "${resolved#AMBIG:}"
            stale=$((stale + 1))
            continue
        fi

        nlines="$(wc -l <"$resolved")"
        flag='OK    '
        if [ "$start" -gt "$nlines" ] || [ "$end" -gt "$nlines" ]; then
            flag='RANGE '
            stale=$((stale + 1))
        fi

        # did this source file change since the doc was last committed?
        moved=''
        if [ -n "$doc_commit" ]; then
            key="$doc_commit|$resolved"
            if [ -z "${CHANGED_CACHE[$key]+x}" ]; then
                if [ -n "$(git diff --name-only "$doc_commit" HEAD -- "$resolved" 2>/dev/null)" ] \
                   || ! git diff --quiet HEAD -- "$resolved" 2>/dev/null; then
                    CHANGED_CACHE[$key]='MOVED'
                else
                    CHANGED_CACHE[$key]=''
                fi
            fi
            moved="${CHANGED_CACHE[$key]}"
        fi
        [ -n "$moved" ] && moved="  <MOVED: file edited since doc>"

        printf '  [%s]  %s:%s  ->  %s  (%s has %s lines)%s\n' \
            "$flag" "$doc" "$docline" "$ref" "$resolved" "$nlines" "$moved"
        if [ "$flag" = 'OK    ' ]; then
            show_line "$resolved" "$start"
            [ "$end" != "$start" ] && show_line "$resolved" "$end"
        fi
    done <<<"$refs"

    if [ -n "$prose" ]; then
        printf '  ---- bare prose refs (need the doc'\''s surrounding file context) ----\n'
        while IFS= read -r hit; do
            [ -z "$hit" ] && continue
            printf '  [PROSE ]  %s:%s  "%s"\n' "$doc" "${hit%%:*}" "${hit#*:}"
        done <<<"$prose"
    fi

    nfence="$(grep -c '^```' "$doc" 2>/dev/null || echo 0)"
    printf '  ---- %s fenced code block(s); check any verbatim source excerpts ----\n' \
        "$((nfence / 2))"
done

printf '\n──────────────────────────────────────────────────────────────────\n'
printf 'scanned %s path:line reference(s); %s could not be resolved mechanically.\n' \
    "$total" "$stale"
printf 'The rest are only PROVEN TO EXIST, not proven correct -- read the source\n'
printf 'text printed under each one and check it against what the doc claims.\n'
printf '──────────────────────────────────────────────────────────────────\n'
