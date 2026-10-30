# Desktop evidence

Some checks need a real desktop: interactive capture, overlays, input latency,
GPU drivers, physical X11 sessions. GitHub-hosted runners cannot provide them,
and LayerC has no self-hosted runners. Instead, anyone may run a desktop
harness on their own machine and post the result to a pull request as a
structured comment. [`desktop-evidence.yml`](../.github/workflows/desktop-evidence.yml)
validates the comment as untrusted data and publishes an **advisory,
non-required** check run on the tested commit.

`ci.yml` stays the mandatory gate. A desktop-evidence check never replaces it,
and nothing requires one to merge.

## What a check does and does not prove

A desktop-evidence check proves that a GitHub account posted a well-formed
report naming a specific commit of that pull request. It does **not** prove
that the tests ran, that they ran on the stated machine, or that the reported
CI run and build identity are genuine: GitHub executed nothing. A dishonest
tester can fabricate a pass. Weigh reports by who submitted them, and
reproduce important claims on an independent machine before relying on them
for a release.

| Submitter (`author_association`) | Trust tier | Check conclusion |
| --- | --- | --- |
| `OWNER`, `MEMBER`, `COLLABORATOR` | maintainer-attested | `success` when the evaluation is `complete`, otherwise `failure` |
| anyone else | unverified | always `neutral`; the title starts with `Unverified` |

The evaluation uses the harness definition in
[`.github/desktop-evidence.json`](../.github/desktop-evidence.json) on the
default branch:

- `failed`: any result is `fail`.
- `incomplete`: not failed, but a `required` test is missing, or is `skip` or
  `unavailable` without an `allowUnavailable` entry.
- `complete`: otherwise. `optional` tests may have any status except `fail`.

Unverified checks still show the evaluation in their summary, but never
conclude `success` or `failure`.

## Running untrusted pull request code

A harness runs the pull request's binaries and scripts on your machine. Treat
code from anyone you do not trust as hostile:

- Use a dedicated, reimageable test machine for physical-hardware harnesses,
  or a disposable VM when the harness accepts `vm`.
- Keep personal files, credentials with repository write access, and sensitive
  networks off that machine. Post the comment from elsewhere if needed: the
  helpers' `-DryRun`/`--dry-run` mode prints the comment instead of posting it.
- Disposable LayerC profiles and settings keys protect application data only;
  they are not a sandbox.

## Submitting a report

The helpers resolve the commit from the pull request: the head commit of an
open or closed pull request, or the merge commit of a merged one (post-merge
evidence on `main`). They require the checkout to be that commit with no
tracked changes, build the report, validate it with
`tools/desktop_evidence.py`, and post it with `gh pr comment`.

Windows hardware benchmarks (interactive desktop, quiescent box: no `layerd` or
`layere` running) on the binaries that `ci.yml` built for that commit:

```powershell
pwsh -File tools\submit-desktop-evidence.ps1 -PullRequest 123
pwsh -File tools\submit-desktop-evidence.ps1 -PullRequest 123 -DryRun
```

Physical Linux X11 checklist (wraps `tools/run-linux-v26-evidence.sh`; see
[`Linux.md`](Linux.md#v262-calibration-and-evidence)):

```sh
bash tools/submit-desktop-evidence.sh 123 gnome native 0x4600007
bash tools/submit-desktop-evidence.sh 123 kde flatpak 0x4600007 --dry-run
```

## Comment format

GitHub limits a comment to 65536 characters. The body, after converting CRLF
and CR to LF, must:

1. start with the exact line `<!-- desktop-evidence/1 -->`;
2. contain exactly one line that is exactly `` ```json ``, followed later by a
   line that is exactly `` ``` ``. The lines between them are the report, at
   most 16384 UTF-8 bytes.

Everything else is free text for human readers and is ignored. Only newly
created comments are evaluated; editing a comment changes nothing.

## Report schema `desktop-evidence/1`

A strict JSON object. Duplicate keys, `NaN`/`Infinity`, unknown keys at any
level, and wrong JSON types (for example `1` for a boolean) are rejected.

| Key | Type and limits |
| --- | --- |
| `schema` | exactly `"desktop-evidence/1"` |
| `project` | `owner/repo`; must equal the receiving repository (ASCII case-insensitive) |
| `commit` | 40 lowercase hex digits; must be a commit of the pull request, or its merge commit once merged |
| `recordedAt` | UTC `YYYY-MM-DDTHH:MM:SSZ` |
| `harness` | `{ "id": <id>, "version": <version> }`; `id` must be defined in the harness configuration |
| `build` | see below; `source` must be listed in the harness's `builds` |
| `environment` | `{ "os": <text>, "session": <id>, "machine": "physical" \| "vm", "gpu": <text> (optional) }`; `session` and `machine` must be listed in the harness's `sessions` and `machines` |
| `results` | 1 to 64 objects `{ "id": <id>, "status": "pass" \| "fail" \| "skip" \| "unavailable", "detail": <text> (optional, up to 200) }`; ids unique and defined by the harness as `required` or `optional` |

`build` is one of:

- `{ "source": "ci-artifact", "ciRunId": <integer 1..2^53-1>, "builtCommit": <40 hex>, "identitySha256": <64 lowercase hex> }`.
  The CI run and the SHA-256 of its build-identity file, as reported by the
  tester. A pull request CI run builds the test merge commit, so
  `builtCommit` may differ from `commit`.
- `{ "source": "local-build", "builtCommit": <40 hex>, "dirty": <boolean> }`.

Patterns:

- `<id>`: `^[a-z0-9][a-z0-9._-]{0,63}$`.
- `<version>`: `^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$`.
- `<text>`: 1 to 80 code points unless stated otherwise, drawn from Unicode
  letters, marks, numbers, punctuation, symbols, and the ASCII space, excluding
  `` ` ``, `|`, `<`, and `>`. Control, format (including bidirectional
  overrides), line-separator, and surrogate code points are rejected. No
  leading or trailing space.

Reports must not contain host names, user names, or file paths; the helpers
never copy them.

## Harness configuration

[`.github/desktop-evidence.json`](../.github/desktop-evidence.json) is the
project-specific part. The default branch's copy decides which harnesses,
sessions, machines, build sources, and test ids a report may use:

```json
{
  "schema": "desktop-evidence-config/1",
  "harnesses": {
    "<id>": {
      "description": "<shown in the check summary>",
      "sessions": ["<id>"],
      "machines": ["physical"],
      "builds": ["ci-artifact"],
      "required": ["<id>"],
      "optional": ["<id>"],
      "allowUnavailable": { "<required id>": "<reason shown in the summary>" }
    }
  }
}
```

## Publication

The workflow runs on `issue_comment` `created` for pull request comments from
non-bot accounts that start with the marker. It runs the default branch's copy
of itself, checks out only `tools/desktop_evidence.py` and the configuration,
never builds or runs pull request code, and receives the comment through the
environment. Its token has `checks: write`, `contents: read`, and
`pull-requests: read`.

An accepted report becomes one completed check run on `commit`:

- name: `Desktop evidence: <harness> (<session>) by @<login>`. A newer report
  from the same account for the same harness and session supersedes the older
  one on that commit;
- `external_id`: `comment-<comment id>`; `details_url`: the comment;
- title: tier, evaluation, and status counts; summary: the trust statement,
  build, environment, the SHA-256 of the canonical report (keys sorted, no
  insignificant whitespace), and the results table. Report text appears only
  inside code spans.

Because the check is bound to `commit`, pushing a new commit leaves it on the
old one; the new head has no desktop evidence until someone reports again.

A rejected report creates no check run. The workflow run fails and its job
summary lists the reasons. `tools/desktop_evidence.py publish` exits 2 for a
rejected report and 1 for usage or internal errors.

Limits: GitHub lists at most 250 commits of a pull request, so reports on
older commits of very large pull requests are rejected. In a private
repository only collaborators can comment, so only maintainer-attested reports
are possible there.

## Validating locally

```sh
python tools/desktop_evidence.py validate --config .github/desktop-evidence.json --comment-file comment.md --repository EnderKeera/LayerC
python -m unittest discover -s tools/tests
```
