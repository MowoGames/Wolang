#!/usr/bin/env python3
"""Turns Wolang's documentation into the bundle mowoengine.com renders.

The guides in `docs/` and the API reference `wolang doc` writes become MDX
pages under `Wolang/Site/` in a checkout of the engine-docs repository,
beside the engine's own. The site copies them into its `/docs/` tree:
`/docs/wolang/...` for the guides, `/docs/wolang-api/...` for the
reference (D210).

    python tools/package/make_site_docs.py <engine-docs checkout> [--wolang <driver>]

With `--wolang`, the API reference is generated fresh rather than read from
`docs/api/`.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = "https://github.com/MowoGames/Wolang"
BRANCH = "alpha"

# The guides, in the order the sidebar shows them: source, page, sidebar title.
GUIDES = [
    ("docs/install.md", "install", "Install"),
    ("docs/tour.md", "tour", "A tour"),
    ("docs/cookbook.md", "cookbook", "Cookbook"),
    ("docs/stdlib.md", "stdlib", "Standard library"),
    ("docs/spec.md", "spec", "Language reference"),
    ("docs/packages.md", "packages", "Packages"),
    ("docs/embedding.md", "embedding", "Embedding"),
    ("docs/debugging.md", "debugging", "Debugging"),
    ("editors/README.md", "editors", "Editors"),
    ("docs/diagnostics.md", "diagnostics", "Diagnostics"),
    ("docs/building.md", "building", "Building from source"),
]
PACKAGES = ["std", "core", "game", "http"]
FENCES = {"sh": "bash", "bat": "powershell", "cmd": "powershell"}


def git(*arguments):
    result = subprocess.run(["git", "-C", str(ROOT), *arguments], capture_output=True, text=True)
    return result.stdout.strip()


def page_link(target, here):
    """Where a Markdown link in |here| points on the site."""
    if re.match(r"^[a-z]+:", target) or target.startswith("#") or target.startswith("/"):
        return target
    path, _, anchor = target.partition("#")
    resolved = (ROOT / here).parent.joinpath(path).resolve()
    try:
        relative = resolved.relative_to(ROOT).as_posix()
    except ValueError:
        return target
    suffix = "#" + anchor if anchor else ""
    for source, slug, _ in GUIDES:
        if relative == source:
            return "/docs/wolang/{}/{}".format(slug, suffix)
    if relative == "docs/overview.md":
        return "/docs/wolang/" + suffix
    for package in PACKAGES:
        if relative == "docs/api/{}.md".format(package):
            return "/docs/wolang-api/{}/{}".format(package, suffix)
    kind = "tree" if resolved.is_dir() else "blob"
    return "{}/{}/{}/{}{}".format(REPOSITORY, kind, BRANCH, relative, suffix)


def escape_text(segment):
    """What MDX would read as JSX or an expression, made literal."""
    segment = re.sub(r"<(https?://[^>]+)>", r"[\1](\1)", segment)
    return segment.replace("\\{", "{").replace("{", "\\{").replace("}", "\\}").replace("<", "&lt;")


def convert_line(line, here):
    parts = re.split(r"(`+[^`]*`+)", line)
    out = []
    for part in parts:
        if part.startswith("`"):
            out.append(part)
            continue
        part = re.sub(r"\]\(([^)\s]+)\)", lambda m: "](" + page_link(m.group(1), here) + ")", part)
        out.append(escape_text(part))
    return "".join(out)


def to_mdx(text, here):
    """The page's title, its first paragraph, and its body as MDX."""
    lines = text.replace("\r\n", "\n").split("\n")
    title = None
    body = []
    fenced = False
    for line in lines:
        if line.startswith("```"):
            fenced = not fenced
            if fenced:
                language = line[3:].strip()
                line = "```" + FENCES.get(language, language)
            body.append(line)
            continue
        if fenced:
            body.append(line)
            continue
        if title is None and line.startswith("# "):
            title = line[2:].strip()
            continue
        body.append(convert_line(line, here))

    paragraph = []
    fenced = False
    for line in lines:
        if line.startswith("```"):
            fenced = not fenced
            continue
        if fenced or line.startswith("#") or line.startswith("<") or line.startswith("|") or line.startswith(">"):
            if paragraph:
                break
            continue
        if not line.strip():
            if paragraph:
                break
            continue
        paragraph.append(line.strip())
    return title, describe(" ".join(paragraph)), "\n".join(body).strip() + "\n"


def describe(paragraph):
    plain = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", paragraph)
    plain = plain.replace("`", "").replace("**", "").replace("*", "")
    plain = re.sub(r"\s*\(D\d+\)", "", plain)
    if len(plain) <= 158:
        return plain
    cut = plain[:158]
    sentence = cut.rfind(". ")
    if sentence > 60:
        return cut[:sentence + 1]
    return cut[:cut.rfind(" ")].rstrip(",;:") + "…"


def front(fields):
    out = ["---"]
    for key, value in fields.items():
        if value is None:
            continue
        out.append("{}: {}".format(key, json.dumps(value, ensure_ascii=False)))
    out.append("---")
    return "\n".join(out) + "\n\n"


def last_changed(source):
    return git("log", "-1", "--format=%cI", "--", source) or None


def write_page(path, fields, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(front(fields) + body, encoding="utf-8", newline="\n")


def api_sources(driver):
    """Where the API reference is read from: fresh when there is a driver."""
    if driver is None:
        return ROOT / "docs" / "api", None
    held = Path(tempfile.mkdtemp(prefix="wolang-api-"))
    for package in PACKAGES:
        subprocess.run([driver, "doc", str(ROOT / "lib" / package), "-o", str(held / (package + ".md"))], check=True, capture_output=True)
    return held, held


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", help="a checkout of the engine-docs repository")
    parser.add_argument("--wolang", help="the wolang driver, to generate the API reference fresh")
    arguments = parser.parse_args()

    bundle = Path(arguments.bundle).resolve()
    out = bundle / "Wolang"
    if out.exists():
        shutil.rmtree(out)
    site = out / "Site"
    commit = git("rev-parse", "HEAD")
    short = commit[:9]
    dates = {}

    title, description, body = to_mdx((ROOT / "docs" / "overview.md").read_text(encoding="utf-8"), "docs/overview.md")
    write_page(site / "wolang" / "index.mdx", {"title": title, "description": description, "order": 14, "icon": "Braces", "generated": "wolang", "verifiedAt": short}, body)
    dates["wolang/index.mdx"] = last_changed("docs/overview.md")

    for order, (source, slug, sidebar) in enumerate(GUIDES, start=1):
        title, description, body = to_mdx((ROOT / source).read_text(encoding="utf-8"), source)
        fields = {"title": title or sidebar, "sidebarTitle": sidebar if sidebar != title else None, "description": description, "order": order, "generated": "wolang", "verifiedAt": short}
        write_page(site / "wolang" / (slug + ".mdx"), fields, body)
        dates["wolang/{}.mdx".format(slug)] = last_changed(source)

    api, temporary = api_sources(str(Path(arguments.wolang).resolve()) if arguments.wolang else None)
    listing = ["Every declaration the packages offer, generated from the source by `wolang doc` on each commit.", ""]
    for order, package in enumerate(PACKAGES, start=1):
        source = api / (package + ".md")
        if not source.exists():
            continue
        _, _, body = to_mdx(source.read_text(encoding="utf-8"), "docs/api/{}.md".format(package))
        modules = re.findall(r"^## (\S+)", body, flags=re.MULTILINE)
        description = "The `{}` package: {}.".format(package, ", ".join(modules[:6]) + (" and more" if len(modules) > 6 else "")) if modules else "The `{}` package.".format(package)
        write_page(site / "wolang-api" / (package + ".mdx"), {"title": "{} reference".format(package), "sidebarTitle": package, "description": description.replace("`", ""), "order": order, "generated": "wolang", "verifiedAt": short}, body)
        dates["wolang-api/{}.mdx".format(package)] = last_changed("lib/{}".format(package))
        listing.append("- [{}](/docs/wolang-api/{}/) — {}".format(package, package, ", ".join("`{}`".format(m) for m in modules) or "the package"))
    write_page(site / "wolang-api" / "index.mdx", {"title": "Wolang API reference", "sidebarTitle": "Wolang API", "description": "Every module the Wolang packages offer, generated from the source on each commit.", "order": 15, "icon": "FileText", "generated": "wolang", "verifiedAt": short}, "\n".join(listing) + "\n")
    if temporary is not None:
        shutil.rmtree(temporary, ignore_errors=True)

    grammar = ROOT / "editors" / "vscode" / "syntaxes" / "wolang.tmLanguage.json"
    shutil.copyfile(grammar, out / "wolang.tmLanguage.json")

    version = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8").split("project(", 1)[1])
    manifest = {
        "wolang": {
            "commit": short,
            "commitFull": commit,
            "commitDate": git("log", "-1", "--format=%cI") or None,
            "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
            "version": version.group(1) if version else "unknown",
        },
        "dates": dict(sorted((k, v) for k, v in dates.items() if v)),
    }
    (out / "bundle.json").write_text(json.dumps(manifest, indent="\t") + "\n", encoding="utf-8", newline="\n")
    count = sum(1 for _ in site.rglob("*.mdx"))
    print("{} pages for Wolang {} at {} in {}".format(count, manifest["wolang"]["version"], short, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
