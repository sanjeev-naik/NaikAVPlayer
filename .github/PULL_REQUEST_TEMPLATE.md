<!--
Thank you for contributing to NaikAVPlayer! 

Please fill out the template below to help our maintainers review and merge your changes efficiently.
Ensure that you have read our CONTRIBUTING.md before submitting this pull request.
-->

## Summary

*Briefly describe the key additions or modifications introduced by this Pull Request.*

---

## Motivation

*Why is this change necessary? What problem or issue does this solve? If it fixes an issue, describe the bug and how your patch corrects it.*

---

## Changes

*List the key changes in a bulleted format. Point out any modified files, newly added files, or refactored components.*

- **Component/File Modified**: Describe the change.
- **Component/File Modified**: Describe the change.

---

## Testing

*Testing is crucial for keeping our engine robust. Please describe how you tested your changes.*

### 1. Verification Matrix
*Please check the platforms on which you have compiled and ran this branch:*
- [ ] **Windows (MSVC)**
- [ ] **Windows (MinGW/Clang)**
- [ ] **Linux (GCC/Clang)**
- [ ] **macOS (Apple Clang)**
- [ ] **Raspberry Pi (Linux arm64)**

### 2. Manual Verification
*Outline the steps you took to manually test the playback, UI, or sync behavior (e.g., test files loaded, seek actions, loop toggle):*
1. ...
2. ...

### 3. Automated Test Suite
- [ ] Checked that `NaikAVPlayer_tests` compile and pass locally.
- [ ] Confirmed code coverage has not dropped on core files (aiming for 100% line coverage).
- [ ] Ran checks with address/undefined sanitizers active and resolved any issues.

---

## Screenshots or Videos (Optional)

*If your changes modify the user interface (PlayerUI, HUD widgets, vector icons, glassmorphism layout), please attach screenshots or screen recordings showing before/after results.*

| Before | After |
| :---: | :---: |
| *Insert Image* | *Insert Image* |

---

## Related Issues

*Link any open issues that this pull request fixes or addresses (e.g., `Fixes #123`, `Closes #456`).*

- Resolves #

---

## Checklist

*Before requesting a review, please ensure you have checked all of the following:*

- [ ] My code follows the project's **C++17 coding standards** and naming conventions.
- [ ] The build finishes successfully without compiler warnings (`TREAT_WARNINGS_AS_ERRORS=ON`).
- [ ] I have formatted my code and cleaned up unnecessary comments.
- [ ] All new public methods/classes are documented in their header files.
- [ ] I have written unit/integration tests covering new branch paths.
- [ ] AddressSanitizer and UndefinedBehaviorSanitizer run clean with no leaks or runtime errors.
- [ ] My git commits conform to the **Conventional Commits** specification.
