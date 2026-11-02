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
    ("docs/whats-new.md", "whats-new", "What is new"),
    ("docs/tour.md", "tour", "A tour"),
    ("docs/cookbook.md", "cookbook", "Cookbook"),
    ("docs/stdlib.md", "stdlib", "Standard library"),
    ("docs/spec.md", "spec", "Language reference"),
    ("docs/packages.md", "packages", "Packages"),
    ("docs/embedding.md", "embedding", "Embedding"),
    ("docs/debugging.md", "debugging", "Debugging"),
    ("editors/README.md", "editors", "Editors"),
    ("docs/diagnostics.md", "diagnostics", "Diagnostics"),
    ("docs/style.md", "style", "Style"),
    ("docs/architecture.md", "architecture", "How the compiler works"),
    ("docs/building.md", "building", "Building from source"),
    ("docs/releasing.md", "releasing", "Releasing"),
    ("docs/roadmap.md", "roadmap", "Roadmap"),
    ("docs/decisions.md", "decisions", "Design decisions"),
    ("CHANGELOG.md", "changelog", "Changelog"),
]
PACKAGES = ["std", "core", "game", "http", "db", "gfx", "image", "tls", "math"]
FENCES = {"sh": "bash", "bat": "powershell", "cmd": "powershell"}
CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)", re.DOTALL)


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


def convert_text(chunk, here):
    """A run of prose lines: code spans stay as written, even across a line break."""
    out = []
    last = 0
    for span in CODE_SPAN.finditer(chunk):
        out.append(convert_prose(chunk[last:span.start()], here))
        out.append(span.group(0))
        last = span.end()
    out.append(convert_prose(chunk[last:], here))
    return "".join(out)


def convert_prose(part, here):
    part = re.sub(r"\]\(([^)\s]+)\)", lambda m: "](" + page_link(m.group(1), here) + ")", part)
    return escape_text(part)


def to_mdx(text, here):
    """The page's title, its first paragraph, and its body as MDX."""
    lines = text.replace("\r\n", "\n").split("\n")
    title = None
    body = []
    pending = []
    fenced = False

    def flush():
        if pending:
            body.append(convert_text("\n".join(pending), here))
            pending.clear()

    for line in lines:
        if line.startswith("```"):
            flush()
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
        if not line.strip() or line.startswith("#"):
            flush()
            body.append(convert_text(line, here) if line.strip() else line)
            continue
        pending.append(line)
    flush()

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


def slug_of(heading):
    plain = re.sub(r"[`*_]", "", heading).strip().lower()
    plain = re.sub(r"[^\w\- ]", "", plain)
    return plain.replace(" ", "-")


def headings_of(body):
    seen = {}
    found = set()
    fenced = False
    for line in body.split("\n"):
        if line.startswith("```"):
            fenced = not fenced
            continue
        match = None if fenced else re.match(r"^#{1,6}\s+(.*?)\s*#*$", line)
        if match:
            slug = slug_of(match.group(1))
            count = seen.get(slug, 0)
            seen[slug] = count + 1
            found.add(slug if count == 0 else "{}-{}".format(slug, count))
    return found


def target_page(target):
    """The generated file a site link points at, or None for an outside link."""
    path = target.partition("#")[0].rstrip("/")
    if path == "/docs/wolang":
        return "wolang/index.mdx"
    if path == "/docs/wolang-api":
        return "wolang-api/index.mdx"
    if path.startswith("/docs/wolang-api/"):
        return "wolang-api/{}.mdx".format(path[len("/docs/wolang-api/"):])
    if path.startswith("/docs/wolang/"):
        return "wolang/{}.mdx".format(path[len("/docs/wolang/"):])
    return None


def validate(site):
    """What would break the site's build or its links, one message each."""
    problems = []
    pages = {}
    for page in sorted(site.rglob("*.mdx")):
        pages[page.relative_to(site).as_posix()] = page.read_text(encoding="utf-8")

    orders = {}
    anchors = {}
    for name, text in pages.items():
        if not text.startswith("---\n") or "\n---\n" not in text:
            problems.append("{}: no front matter".format(name))
            continue
        head, body = text[4:].split("\n---\n", 1)
        fields = {}
        for line in head.split("\n"):
            key, _, value = line.partition(": ")
            try:
                fields[key] = json.loads(value)
            except ValueError:
                problems.append("{}: front matter '{}' is not JSON".format(name, key))
        for required in ("title", "description", "order", "generated"):
            if not fields.get(required) and fields.get(required) != 0:
                problems.append("{}: front matter lacks '{}'".format(name, required))
        if name.startswith("wolang-api/") and name != "wolang-api/index.mdx":
            package = name[len("wolang-api/"):-len(".mdx")]
            for module in re.findall(r"^## (\S+)", body, flags=re.MULTILINE):
                if module != package and not module.startswith(package + "."):
                    problems.append("{}: documents {}, which is not in the package".format(name, module))
        if not name.endswith("/index.mdx"):
            folder = name.rpartition("/")[0]
            order = fields.get("order")
            if order in orders.setdefault(folder, {}):
                problems.append("{}: order {} is also {}".format(name, order, orders[folder][order]))
            orders[folder][order] = name
        anchors[name] = headings_of(body)

    for name, text in pages.items():
        if "\n---\n" not in text:
            continue
        body = text.split("\n---\n", 1)[1]
        if sum(1 for line in body.split("\n") if line.startswith("```")) % 2:
            problems.append("{}: a code fence is never closed".format(name))
            continue
        prose = re.sub(r"^```.*?^```[^\n]*$", "", body, flags=re.MULTILINE | re.DOTALL)
        prose = CODE_SPAN.sub("", prose)
        for number, line in enumerate(prose.split("\n"), start=1):
            bare = line.replace("\\{", "").replace("\\}", "")
            if "{" in bare or "}" in bare:
                problems.append("{}: an unescaped brace in prose: {}".format(name, line.strip()[:70]))
            if "<" in line:
                problems.append("{}: an unescaped '<' in prose: {}".format(name, line.strip()[:70]))

        for target in re.findall(r"\]\(([^)\s]+)\)", prose):
            if target.startswith("#"):
                if target[1:] and target[1:] not in anchors.get(name, ()):
                    problems.append("{}: no heading for {}".format(name, target))
                continue
            page = target_page(target)
            if page is not None:
                if page not in pages:
                    problems.append("{}: {} points at no page".format(name, target))
                    continue
                anchor = target.partition("#")[2]
                if anchor and anchor not in anchors.get(page, ()):
                    problems.append("{}: {} has no heading '{}'".format(name, page, anchor))
                continue
            if target.startswith(REPOSITORY + "/"):
                relative = target[len(REPOSITORY) + 1:].partition("#")[0].split("/", 2)
                if len(relative) == 3 and not (ROOT / relative[2]).exists():
                    problems.append("{}: {} names a file that is not in the repository".format(name, relative[2]))
                continue
            if not re.match(r"^[a-z]+:", target) and not target.startswith("/"):
                problems.append("{}: a relative link that was not converted: {}".format(name, target))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", nargs="?", help="a checkout of the engine-docs repository")
    parser.add_argument("--wolang", help="the wolang driver, to generate the API reference fresh")
    parser.add_argument("--check", action="store_true", help="build into a scratch directory, validate the pages, write nothing else")
    arguments = parser.parse_args()
    if arguments.bundle is None and not arguments.check:
        parser.error("a bundle directory is required unless --check is given")

    scratch = Path(tempfile.mkdtemp(prefix="wolang-site-")) if arguments.bundle is None else None
    bundle = Path(arguments.bundle).resolve() if arguments.bundle else scratch
    try:
        status = build(bundle, arguments)
        problems = validate(bundle / "Wolang" / "Site")
    finally:
        if scratch is not None:
            shutil.rmtree(scratch, ignore_errors=True)
    for problem in problems:
        print("make_site_docs: " + problem, file=sys.stderr)
    if problems:
        print("make_site_docs: {} problems".format(len(problems)), file=sys.stderr)
        return 1
    return status


def build(bundle, arguments):
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
