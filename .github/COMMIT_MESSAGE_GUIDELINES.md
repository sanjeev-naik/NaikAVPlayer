# Git Commit Message Guidelines

We enforce the **Conventional Commits** specification for all commits in the **NaikAVPlayer** repository. This enables automated changelog generation, clean git histories, and simplifies the release process.

---

## Format

Every commit message must follow this structured pattern:

```text
<type>(<scope>): <short summary>

[optional body description explaining "why", not "what"]

[optional footer(s) referencing issues, e.g., Fixes #123]
```

*Note: The type and scope must always be lowercase.*

---

## Supported Commit Types

Choose the type that matches the primary intent of your change:

| Type | Purpose | Example |
| :--- | :--- | :--- |
| `feat` | A new feature or user-facing capability | `feat(player): add frame stepping support` |
| `fix` | A bug fix (restores expected behavior) | `fix(decoder): resolve hw decoder seek freeze` |
| `docs` | Documentation only changes | `docs(readme): update build instructions` |
| `refactor` | Code change that neither fixes a bug nor adds a feature | `refactor(audio): simplify volume bypass checks` |
| `perf` | A code change that improves execution performance | `perf(renderer): reduce GPU texture update frequency` |
| `test` | Adding missing tests or correcting existing tests | `test(decoder): add fallback unit test paths` |
| `build` | Changes that affect the build system or external deps | `build(cmake): link platform pthread on Linux` |
| `ci` | Changes to CI configuration files and scripts | `ci(github): add windows runner build check` |
| `chore` | Other changes that do not modify src or test files | `chore(git): update gitignore templates` |
| `revert` | Reverts a previous commit | `revert: feat(player): add frame stepping support` |

---

## Common Scopes

The scope specifies the codebase module affected by your commit. The following list of scopes is recommended:

- **Player Core**: `player` (General playback state machine in `PlayerController`), `decoder` (Generic decoding loop).
- **Subsystems**: `audio` (`AudioDecoder`), `video` (`VideoDecoder`), `subtitle` (Subtitles / text overlays), `renderer` (Texture mapping, SDL YUV).
- **Libraries/UI**: `ffmpeg` (AVCodec, AVFormat, SWScale integration), `imgui` (Dear ImGui settings/wrappers), `ui` (`PlayerUI` overlays, HUD), `network` (Network streaming features).
- **Build/Meta**: `build` (Build targets), `cmake` (`CMakeLists.txt`), `docs` (Manuals/Guides), `tests` (`tests.cpp`).
- **Platforms**: `linux`, `windows`, `macos`, `android`.

*If a commit spans multiple scopes or doesn't fit any particular scope, you may omit the parentheses and scope entirely (e.g. `feat: improve cross-platform key binds`).*

---

## Commit Guidelines & Best Practices

1. **Use the Imperative Mood**: Formulate the short summary as if you are commanding the codebase to do something.
   - **Yes**: `fix(audio): correct volume attenuation bypass`
   - **No**: `fixed volume attenuation` or `correcting volume attenuation`
2. **Character Limits**: Keep the summary line under **72 characters** (strict limit is 50-72 characters). This prevents line wrapping in GitHub's commits UI.
3. **No Period**: Do not end the summary line with a period.
4. **Use the Body for "Why"**: The summary describes *what* changed. If the change is complex, add a blank line after the summary and write a paragraph explaining *why* the change was made, constraints encountered, or architectural trade-offs.
5. **Reference Issues in the Footer**: If the commit resolves an issue, add a blank line at the end and write `Fixes #<issue-number>` or `Closes #<issue-number>`.

---

## Good vs. Bad Examples

### Good Examples

```text
feat(player): add loop playback mode

Introduce toggleable continuous replay via a 'Loop' HUD button or by pressing
the 'L' key. This automatically seeks back to the start when the video ends.

Fixes #45
```

```text
fix(renderer): prevent memory leak during texture resizing

Ensure SDL_DestroyTexture is called on the old texture before allocating
a new streaming texture during window resize events.
```

```text
perf(audio): bypass attenuation loop when muted or full volume

Directly use std::memset / std::memcpy when volume is <= 0.01f or >= 0.99f
to eliminate byte scaling arithmetic overhead.
```

---

### Bad Examples

```text
Fixed the decoder freezing issue on windows.
```
*Why it's bad: Missing conventional commit prefix (type/scope), uses past tense ("Fixed"), starts with a capital letter, ends with a period, and summary is too verbose.*

```text
feat: Added a button
```
*Why it's bad: Missing scope (optional but recommended for clarity), uses past tense ("Added"), does not explain what the button does.*

```text
refactor(PlayerController.cpp): fix bug in seek CatchupMode landing
```
*Why it's bad: Mixing types (uses type `refactor` to `fix` a bug). Code changes that fix bugs must use the `fix` type. The scope `PlayerController.cpp` is a file name; use the general scope `player` instead.*
