# Releasing Wolang

A release is one pushed tag. The tag starts `.github/workflows/release.yml`,
which builds every archive, publishes them on GitHub with a `SHA256SUMS`,
and then installs the result the way a user would (D248). The version is
written once, in `CMakeLists.txt`; everything else follows it.

The examples below release 1.1.0. Run them from a clean checkout of
`alpha`.

## Before you start

Settings the repository needs, once:

- **Actions may write to contents.** Settings, Actions, General, Workflow
  permissions: "Read and write permissions" (or leave the default and rely
  on the `publish` job's own `permissions: contents: write`, which is how
  the workflow asks for it; an organisation policy that forces read-only
  would block it).
- **Tags are not protected against you.** If tag protection rules exist,
  the person pushing `v*` must be allowed to create them.
- **Secrets are optional.** `DISCORD_WEBHOOK_URL`, `TELEGRAM_BOT_TOKEN`
  and `CHANGELOG_WEBHOOK_URL` only drive the chat announcements; without
  them nothing is sent and the release is unaffected. Nothing else is
  needed: the workflow publishes with the run's own `GITHUB_TOKEN`.

## Cutting a release

1. Branch and bump the version. This edits `CMakeLists.txt` and the two
   editor manifests, which cannot read it from CMake:

   ```sh
   git switch alpha && git pull
   git switch -c release/1.1.0
   python3 tools/package/bump_version.py 1.1.0
   ```

2. Write the `## 1.1.0` section at the top of `CHANGELOG.md`, above the
   previous one: grouped, plain, and about what a user will notice. This
   text becomes the release notes. Then check that everything agrees:

   ```sh
   python3 tools/package/check_version.py
   python3 tools/package/release_notes.py 1.1.0
   ```

   The second command prints what the release page will say.

3. Run the checks CI runs, then open a pull request and merge it into
   `alpha`:

   ```sh
   .claude/scripts/wl ci
   git commit -am "chore: release 1.1.0"
   git push -u origin release/1.1.0
   gh pr create --base alpha --title "chore: release 1.1.0" --body "Version 1.1.0 and its changelog."
   ```

4. Tag the merge commit on `alpha` and push the tag. An annotated tag, so
   the release has a date and an author:

   ```sh
   git switch alpha && git pull
   git tag -a v1.1.0 -m "Wolang 1.1.0"
   git push origin v1.1.0
   ```

5. Watch it:

   ```sh
   gh run watch "$(gh run list --workflow release --limit 1 --json databaseId --jq '.[0].databaseId')"
   ```

   Or open the Actions tab. About 20 to 40 minutes, most of it LLVM
   being installed and the three builds.

## What the workflow does

| Job | What |
|---|---|
| `check` | Fails in seconds if the tag is not `v` plus the version in `CMakeLists.txt`, if the editor manifests disagree, or if `CHANGELOG.md` has no section for it. Everything else waits for it. |
| `artifacts` (3) | Builds Linux x86-64 and arm64 and Windows x86-64 (macOS is out until the account has macOS runners; the rows to restore are in the workflow), each against the pinned LLVM 22 so that `wolang build` works. Runs `wolang --version`, the std tests, and `wolang build examples/hello.wo` and runs the result. Then `cmake --install`, an archive, its checksum, and the embedding-only package. |
| `msi` | The Windows installer, with the PATH entry and the Add/Remove Programs record. |
| `extension` | The VS Code extension as a `.vsix`. |
| `publish` | Collects the checksums into one `SHA256SUMS` and creates the GitHub release with every file. The notes are the `CHANGELOG.md` section for the version, then the pull requests GitHub generates. This is the only job with write access. |
| `smoke` (2) | On Ubuntu and Windows: runs `install.sh` or `install.ps1` from the tagged checkout with `--version 1.1.0`, checks `wolang --version` prints `wolang 1.1.0`, and runs `examples/hello.wo`. It tests the published files, the checksums and the scripts together. |
| `notify` | Tells the chat channels that the release did or did not happen. |
| `changelog` | Posts the commits since the previous tag to the changelog channels. Never fails the release. |

The workflow can also be started by hand (Actions, release, Run workflow,
with the version) to try the builds. That run checks the version and
builds the artifacts but publishes nothing, since only a tag publishes.

## Verifying a release

Download an archive and its checksums and check them, exactly as the
installer does:

```sh
mkdir verify && cd verify
base=https://github.com/MowoGames/Wolang/releases/download/v1.1.0
curl -fLO "$base/wolang-linux-x86_64.tar.gz"
curl -fLO "$base/SHA256SUMS"
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf wolang-linux-x86_64.tar.gz
wolang-linux-x86_64/bin/wolang --version
```

On macOS use `shasum -a 256 -c SHA256SUMS --ignore-missing`. The release
page should list, for 1.1.0: three `wolang-<system>-<arch>` archives (a
`.zip` on Windows), three `wolang-embed-...` archives, the `.msi`, the
`.vsix` and `SHA256SUMS`. Then install it the user's way
([install.md](install.md)):

```sh
curl -fsSL https://raw.githubusercontent.com/MowoGames/Wolang/alpha/install.sh | sh
```

## When it goes wrong

**The `check` job failed.** The tag does not match the source. Nothing was
built or published. Delete the tag, fix the version (or the tag), and tag
again:

```sh
git push origin --delete v1.1.0
git tag -d v1.1.0
```

**A build job failed, nothing was published.** If it was a flake, re-run
the failed jobs from the Actions page (or `gh run rerun <id> --failed`); the
tag stays. If the fix is a code change, merge it to `alpha`, then move the
tag: delete it as above and tag the new commit.

**It was published and is broken** (the `smoke` job failed, or a user
reports it). Delete the release and the tag, fix, and release again under
the same version if nobody has used it yet, or under a new patch version
if they have. A tag people have already fetched should not move.

```sh
gh release delete v1.1.0 --yes --cleanup-tag
git tag -d v1.1.0
```

**Only `smoke` failed.** The files are published. Read its log. It runs
the scripts from the tagged checkout, so if a script in the tag is wrong,
fix it on `alpha` and release a patch version; re-running the job would
test the same old script.

**Chat notifications did not arrive.** Check the secrets above. They never
affect the release.

## The first release

There is no previous tag, so:

- `changelog` runs `git describe` for the previous tag, finds none, and
  posts a card that says "First release" instead of a list of commits. That
  is expected, not a failure.
- GitHub's generated notes cover every pull request ever merged when there
  is no previous release, so they are long. The `CHANGELOG.md` section
  comes first and is the part to read; edit the release page by hand after
  publishing if the tail is unwanted.
- Until the first release is published, `releases/latest` does not exist, so
  `install.sh` with no `--version` fails with "could not find a release".
  Once it is out, `latest` is resolved through the API, and through the
  `/releases/latest` redirect when the API is rate limited. Pinning works
  from the first minute: `--version 1.1.0`.
- A release created as a draft or a pre-release is not `latest`. The
  workflow publishes a normal release.
- The install one-liners fetch the script from the `alpha` branch, so the
  scripts must be merged there before anyone is told to run them.

## Testing the installer without a release

`tools/package/test_install.sh` builds an archive laid out like the
release's, serves it from localhost and runs `install.sh` against it,
including the refusals (a wrong or missing checksum, a bad version). CI
runs it on Linux; run it yourself with a build in `build/debug`:

```sh
sh tools/package/test_install.sh build/debug
```
