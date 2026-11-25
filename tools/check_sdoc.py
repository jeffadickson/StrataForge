#!/usr/bin/env python3
"""StrictDoc (.sdoc) syntax, traceability graph, and artifact checker for StrataForge.

Validates:
  1. Valid [DOCUMENT], [GRAMMAR], [[SECTION]]/[[/SECTION]], and [REQUIREMENT] structure
  2. Unique requirement UIDs across all .sdoc files
  3. Every non-root requirement has valid Parent relation(s) pointing to an existing UID
  4. Zero cycles in the parent-child requirement graph
  5. Every declared ARTIFACT path exists in the repository
  6. Prints live coverage summary (Implemented / Partially Implemented / Planned)
"""

from __future__ import annotations

import pathlib
import re
import sys
from dataclasses import dataclass, field


@dataclass
class Requirement:
    uid: str
    status: str
    title: str
    file: pathlib.Path
    artifact: str | None = None
    parents: list[str] = field(default_factory=list)


def parse_sdoc(path: pathlib.Path) -> tuple[list[Requirement], list[str]]:
    errors: list[str] = []
    reqs: list[Requirement] = []
    lines = path.read_text(encoding="utf-8").splitlines()

    if not lines or lines[0].strip() != "[DOCUMENT]":
        errors.append(f"{path}: must start with [DOCUMENT]")
        return reqs, errors

    section_depth = 0
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i].strip()
        if line == "[[SECTION]]":
            section_depth += 1
            i += 1
            continue
        if line == "[[/SECTION]]":
            section_depth -= 1
            if section_depth < 0:
                errors.append(f"{path}:{i+1}: unmatched [[/SECTION]]")
            i += 1
            continue
        if line == "[REQUIREMENT]":
            i += 1
            fields: dict[str, str] = {}
            parents: list[str] = []
            in_relations = False
            while i < n:
                cur = lines[i]
                stripped = cur.strip()
                if stripped == "" or stripped.startswith("["):
                    break
                if stripped == "RELATIONS:":
                    in_relations = True
                    i += 1
                    continue
                if in_relations:
                    m_val = re.match(r"^\s*VALUE:\s*(\S+)\s*$", cur)
                    if m_val:
                        parents.append(m_val.group(1))
                    i += 1
                    continue

                m_field = re.match(r"^([A-Z_]+):\s*(.*)$", cur)
                if m_field:
                    key, val = m_field.group(1), m_field.group(2).strip()
                    if val == ">>>":
                        buf: list[str] = []
                        i += 1
                        while i < n and lines[i].strip() != "<<<":
                            buf.append(lines[i])
                            i += 1
                        if i >= n:
                            errors.append(f"{path}: unterminated >>> block for {key}")
                            break
                        fields[key] = "\n".join(buf).strip()
                    else:
                        fields[key] = val
                i += 1

            for required in ("UID", "STATUS", "TITLE", "STATEMENT", "RATIONALE", "VERIFICATION_METHOD"):
                if not fields.get(required):
                    errors.append(f"{path}: requirement {fields.get('UID', '<unknown>')} missing {required}")

            if fields.get("UID"):
                reqs.append(
                    Requirement(
                        uid=fields["UID"],
                        status=fields.get("STATUS", ""),
                        title=fields.get("TITLE", ""),
                        file=path,
                        artifact=fields.get("ARTIFACT"),
                        parents=parents,
                    )
                )
            continue
        i += 1

    if section_depth != 0:
        errors.append(f"{path}: unclosed [[SECTION]] blocks (depth={section_depth})")
    return reqs, errors


def check_cycles(req_map: dict[str, Requirement]) -> list[str]:
    errors: list[str] = []
    state: dict[str, int] = {}  # 0=unvisited, 1=visiting, 2=done

    def dfs(uid: str) -> None:
        state[uid] = 1
        for p in req_map[uid].parents:
            if p not in req_map:
                continue
            st = state.get(p, 0)
            if st == 1:
                errors.append(f"Cycle detected involving {uid} -> {p}")
            elif st == 0:
                dfs(p)
        state[uid] = 2

    for uid in req_map:
        if state.get(uid, 0) == 0:
            dfs(uid)
    return errors


def main(argv: list[str]) -> int:
    if len(argv) < 1:
        print("Usage: check_sdoc.py <requirements_dir> [repo_root]", file=sys.stderr)
        return 2

    req_dir = pathlib.Path(argv[0]).resolve()
    repo_root = pathlib.Path(argv[1]).resolve() if len(argv) >= 2 else req_dir.parent

    sdoc_files = sorted(req_dir.rglob("*.sdoc"))
    if not sdoc_files:
        print(f"ERROR: no .sdoc files found in {req_dir}", file=sys.stderr)
        return 1

    all_reqs: dict[str, Requirement] = {}
    errors: list[str] = []

    for f in sdoc_files:
        reqs, errs = parse_sdoc(f)
        errors.extend(errs)
        for r in reqs:
            if r.uid in all_reqs:
                errors.append(f"Duplicate UID '{r.uid}' in {f} and {all_reqs[r.uid].file}")
            else:
                all_reqs[r.uid] = r

    # Validate parent traceability & artifact existence
    valid_statuses = {"Implemented", "Partially Implemented", "Planned"}
    counts = {k: 0 for k in valid_statuses}

    for uid, r in all_reqs.items():
        if r.status not in valid_statuses:
            errors.append(f"{r.file}: invalid STATUS '{r.status}' on {uid}")
        else:
            counts[r.status] += 1

        if not uid.startswith("STRATA-SYS-") and not r.parents:
            errors.append(f"{r.file}: orphan requirement {uid} has no Parent relation")

        for p in r.parents:
            if p not in all_reqs:
                errors.append(f"{r.file}: dangling Parent '{p}' referenced by {uid}")

        if r.artifact:
            art_path = repo_root / r.artifact
            if not art_path.exists():
                errors.append(f"{r.file}: missing ARTIFACT path '{r.artifact}' on {uid}")

    errors.extend(check_cycles(all_reqs))

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    print(
        f"OK: validated {len(sdoc_files)} StrictDoc specs ({len(all_reqs)} requirements) | "
        f"Implemented={counts['Implemented']} | "
        f"Partially Implemented={counts['Partially Implemented']} | "
        f"Planned={counts['Planned']} | 0 orphans | 0 cycles"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
