---
name: git-commit
description: Write a commit message for this repo's already-staged changes. Use when the user asks to commit, write a commit message, or amend one — in this repository specifically. Does not stage anything and does not run git commit itself unless asked to.
---

# git-commit

Write a commit message for whatever is currently staged. Base the message ONLY on `git diff --cached` (and `git diff --cached --stat` for an overview). Never run `git add`, `git add -A`, `git add -u`, or any staging command — staging is the caller's decision, made before this skill runs. If nothing is staged, say so and stop instead of guessing from unstaged changes.

## Execution

Always run this skill in a sub-agent. Never write the commit message inline in the main conversation.

Pick the sub-agent's model from the size of `git diff --cached`:

- Under 50 lines: Haiku.
- 50 to 500 lines: Sonnet.
- Over 500 lines: Opus.

Reasoning effort is always `high`, regardless of model.

The only thing the sub-agent reports back is the final commit message text. It MUST NOT report its reasoning, the diff it read, or a description of what it changed — none of that is useful to the caller.

## Subject line

- Carries a Conventional Commits 1.0.0 type prefix: `fix:`, `feat:`, `test:`, `docs:`, `refactor:`, `chore:`, `build:`, etc.
- Is always imperative.
- For a bug fix, use the standard vulnerability-class term when the bug fits one (out-of-bounds read/write, use-after-free, double-free, integer overflow, type confusion, ...): `fix: close use-after-free in NO_CACHE's read buffer`.
- Fits in 50 characters, prefix included. Tighten the wording until it fits; never drop accuracy to hit the count.

## Body

Skip the body entirely if the diff is self-explanatory. Otherwise:

- The body exists only for a fact the diff itself cannot show: motivation, a bug's trigger condition, a design tradeoff.
- Stay on the code. Never mention how the bug was found (code review, fuzzing, a specific crash report, ...) or anything else about the process behind the commit.
- Describe the finality, not the diff. State what goes wrong and why it matters, or what changed in effect — not how the code does it. The reader can already see the code; the body's job is to say what the code cannot say about itself.
- Do not name functions, member variables, or classes unless understanding the commit is impossible without that name. Prefer a plain description of the role or behavior instead.
- Every paragraph is at most 3 lines.
- Wrap every line at 72 characters. A paragraph is several consecutive wrapped lines with no blank line inside it. Exactly one blank line separates the subject from the body, and separates paragraphs from each other.
- Write in plain English: the vocabulary and sentence complexity of a 16-year-old native speaker. Genuine C++/technical terms stay precise, as a 10-year C++ expert would use them — do not dumb down real technical vocabulary.
- This applies to sentence construction, not just word choice. A comment-style compressed clause ("underflows the fixup math, walking patch pointer outside its buffer") reads like code-adjacent shorthand, not prose. Write full, plain sentences instead: "A sector size of 0 or 1 triggers an integer underflow, pointing past the buffer it should stay inside and corrupting memory." Same technical terms (integer underflow, buffer, corrupting memory), ordinary sentence structure.
- Never name a private member variable (e.g. `buffer_`, `record_buffer_`); describe its role instead ("the shared read buffer"). Public API names (functions, classes, types) are fine to use, but only when naming them is unavoidable.
- Use RFC 2119 keywords (MUST, MUST NOT, SHOULD, MAY, ...) wherever the text states an obligation or invariant.

### Bug-fix commits specifically

When the commit's main purpose is a bug fix, structure the body as two parts, each following the rules above:

1. The bug: what goes wrong, and what triggers it.
2. The fix: what changed to stop it.

Skip the second paragraph if it would only restate the first one with the negative flipped to a positive ("nothing checked X" becoming "X is now checked"). Write it only when it carries a fact the bug paragraph doesn't already imply.

## Trailers

Keep a `Co-Authored-By:` trailer if the repository's existing commits already carry one for the same author.
