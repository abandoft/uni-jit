# Release policy

UniJIT versions are decimal counters written as `MAJOR.MINOR.PATCH`. Minor and
patch are single decimal digits. Every release increments exactly once:

- `1.1.8` -> `1.1.9`
- `1.1.9` -> `1.2.0`
- `1.9.9` -> `2.0.0`

Run `python3 tool/release.py next` to calculate the only valid next version.

For a new release, update the CMake project version and prepend matching
`## MAJOR.MINOR.PATCH` sections to both changelog files. Changelog sections are
strictly newest-first and consecutive; new notes never go below an older
version.

Before tagging, run:

```sh
python3 tool/release.py check
python3 tool/release.py check --tag v0.1.1
```

Push the matching `vMAJOR.MINOR.PATCH` tag only after the branch CI is green.
The release workflow reruns the full hosted CI matrix, extracts the selected
section with `tool/release.py notes`, and uses that exact `CHANGELOG.md` body
as the GitHub Release description. Re-running the workflow updates the same
release instead of creating a duplicate.
