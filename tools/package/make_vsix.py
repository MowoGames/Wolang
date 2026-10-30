#!/usr/bin/env python3
"""Builds the Visual Studio Code extension into a `.vsix`.

A `.vsix` is a zip with two manifests in it, so this is a zip with two
manifests in it. The usual way is `npx vsce package`, which wants npm, a
network and a login; this wants Python, which the repository already
needs for its other scripts (D175).

    python3 tools/package/make_vsix.py            # build/wolang-1.1.0.vsix
    python3 tools/package/make_vsix.py --out .    # somewhere else

The extension is plain JavaScript with no dependencies, so there is
nothing to compile and nothing to bundle: what goes in the zip is what
is in the directory.
"""

import argparse
import json
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
EXTENSION = ROOT / "editors" / "vscode"

# What goes in, beyond the compiled output: everything the extension
# reads at run time, and the three files a marketplace shows.
INCLUDED = [
    "package.json",
    "README.md",
    "CHANGELOG.md",
    "LICENSE",
    "icon.png",
    "extension.js",
    "language-configuration.json",
    "syntaxes",
    "templates",
]

CONTENT_TYPES = """<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json"/>
  <Default Extension="js" ContentType="application/javascript"/>
  <Default Extension="map" ContentType="application/json"/>
  <Default Extension="md" ContentType="text/markdown"/>
  <Default Extension="png" ContentType="image/png"/>
  <Default Extension="svg" ContentType="image/svg+xml"/>
  <Default Extension="txt" ContentType="text/plain"/>
  <Default Extension="vsixmanifest" ContentType="text/xml"/>
  <Default Extension="" ContentType="text/plain"/>
</Types>
"""


def manifest(package):
    """The `.vsixmanifest` the marketplace and the editor both read."""
    name = package["name"]
    publisher = package["publisher"]
    version = package["version"]
    display = package.get("displayName", name)
    description = package.get("description", "")
    categories = ",".join(package.get("categories", []))
    keywords = package.get("keywords", [])
    tags = ",".join(keywords)
    engine = package.get("engines", {}).get("vscode", "^1.75.0")
    repository = package.get("repository", {})
    if isinstance(repository, dict):
        repository = repository.get("url", "")
    banner = package.get("galleryBanner", {})

    properties = [
        ('Microsoft.VisualStudio.Code.Engine', engine),
        ('Microsoft.VisualStudio.Services.Links.Source', repository),
        ('Microsoft.VisualStudio.Services.Links.Getstarted', repository),
        ('Microsoft.VisualStudio.Services.Links.GitHub', repository),
        ('Microsoft.VisualStudio.Services.Branding.Color',
         banner.get("color", "#0E1222")),
        ('Microsoft.VisualStudio.Services.Branding.Theme',
         banner.get("theme", "dark")),
        ('Microsoft.VisualStudio.Services.GitHubFlavoredMarkdown',
         'true'),
    ]
    written = "\n".join(
        '      <Property Id="{}" Value="{}"/>'.format(key, escape(value))
        for key, value in properties if value)

    assets = [
        ('Microsoft.VisualStudio.Code.Manifest', 'extension/package.json'),
        ('Microsoft.VisualStudio.Services.Content.Details',
         'extension/README.md'),
        ('Microsoft.VisualStudio.Services.Content.Changelog',
         'extension/CHANGELOG.md'),
        ('Microsoft.VisualStudio.Services.Content.License',
         'extension/LICENSE'),
        ('Microsoft.VisualStudio.Services.Icons.Default',
         'extension/icon.png'),
    ]
    asset_lines = "\n".join(
        '    <Asset Type="{}" Path="{}" Addressable="true"/>'.format(
            kind, path)
        for kind, path in assets
        if (EXTENSION / path.split("/", 1)[1]).exists())

    return """<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="{name}" Version="{version}" Publisher="{publisher}"/>
    <DisplayName>{display}</DisplayName>
    <Description xml:space="preserve">{description}</Description>
    <Tags>{tags}</Tags>
    <Categories>{categories}</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
{properties}
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
{assets}
  </Assets>
</PackageManifest>
""".format(name=escape(name), version=escape(version),
           publisher=escape(publisher), display=escape(display),
           description=escape(description), tags=escape(tags),
           categories=escape(categories), properties=written,
           assets=asset_lines)


def build(out_dir, quiet=False):
    package = json.loads((EXTENSION / "package.json").read_text("utf-8"))
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / "{}-{}.vsix".format(package["name"],
                                           package["version"])
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as vsix:
        vsix.writestr("extension.vsixmanifest", manifest(package))
        vsix.writestr("[Content_Types].xml", CONTENT_TYPES)
        for name in INCLUDED:
            path = EXTENSION / name
            if not path.exists():
                continue
            if path.is_dir():
                for file in sorted(path.rglob("*")):
                    if file.is_file():
                        vsix.write(file,
                                   "extension/" +
                                   str(file.relative_to(EXTENSION)))
            else:
                vsix.write(path, "extension/" + name)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(ROOT / "build"),
                        help="where the .vsix goes")
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args()
    target = build(Path(arguments.out), arguments.quiet)
    print(target)


if __name__ == "__main__":
    main()
