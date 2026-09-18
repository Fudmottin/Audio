# Code Documentation (Literate Programming)

This project embraces **literate programming principles** in source code comments.
Comments are not an afterthought — they are the externalized memory for a project
where the developer is learning unfamiliar domains (DSP, Core Audio, file formats,
music theory). The code explains the *how*; comments explain the *why*. This is
also useful for future readers of the code, including you.

## Philosophy

- **Comments capture the "why"**: domain knowledge, tradeoffs, decisions, and
  reasoning that executable code cannot express. The code shows what it does;
  comments explain why it does it that way.
- **Comments are the externalized memory**: for a project where the user is
  learning unfamiliar domains, comments serve as a searchable reference that
  can be revisited months later.
- **AI-assisted synchronization**: since AI tools update comments alongside
  code changes, the synchronization problem that traditionally makes comments
  stale is largely solved. Comments stay current because they are updated
  together with the code.
- **Literate Programming**: Donald Knuth's vision — documentation and code
  are intertwined, with narrative explaining the structure and reasoning.
  This project applies that philosophy through structured comments.

## JavaDoc-Style Markup

Comments use structured markup inspired by JavaDoc and Doxygen. This allows
comments to be extracted into documentation by tools like Doxygen, even if
we don't use Doxygen today. The markup is also highly readable by AI tools.

**Header files** — use `/** ... */` block comments on public interfaces:

```cpp
/**
 * @file module.h
 * @brief Brief description of the module's purpose.
 *
 * Longer narrative explaining the module's role, design rationale,
 * and key decisions. This is the "why" that code alone cannot express.
 *
 * @section section-name Section Title
 *
 * A detailed explanation of a specific aspect:
 * - Why a particular approach was chosen
 * - What tradeoffs were considered
 * - What common pitfalls to avoid
 *
 * @param paramName Description of the parameter.
 * @return Description of the return value.
 * @brief One-line summary (optional).
 * @note Important caveats or gotchas.
 */
```

**Source files** — use `/** ... */` block comments at the top of files and
sections, with `// Core Guidelines:` annotations for specific rules:

```cpp
/**
 * @file module.cpp
 * @brief Implementation of the module described in module.h.
 *
 * Domain context explaining implementation decisions:
 * - Why this pattern was chosen over alternatives
 * - What bugs were fixed and why
 * - What debugging information is available
 *
 */
```

## Comment Structure Checklist

When writing or reviewing comments, ensure each file has:

1. **File-level narrative** (`@file`, `@brief`): What this file does and why.
2. **Section blocks** (`@section`): Domain-specific explanations for key
   decisions (e.g., why a particular API pattern, why a conversion formula).
3. **Public interface docs** (`@param`, `@return`, `@brief`): On every
   public method in header files.
4. **Domain context notes** (`@note`): Gotchas, pitfalls, common mistakes,
   and the reasoning behind non-obvious code.

## What to Document

Priority order for adding domain context comments:

1. **API boundary decisions** — Why a particular function signature or
   class design was chosen.
2. **Format/protocol decisions** — Why a specific encoding, byte order,
   or format was selected (e.g., 32-bit integer vs 80-bit extended float).
3. **Bug fixes** — What the bug was, why it happened, and why the fix
   works (so it doesn't reappear).
4. **Debugging hooks** — What diagnostic values are available and how
   to interpret them (e.g., what ioCallbackCount == 0 means).
5. **Algorithmic choices** — Why one algorithm was chosen over alternatives
   (e.g., YINfft over YIN for piano transcription).

## What NOT to Document

- Trivial code that explains itself (e.g., `i++` — increment i).
- Code that is about to be deleted.

## Example: Float-to-Int16 Conversion

```cpp
// Domain context: The clamping and scaling process:
// 1. std::max(-1.0f, ...) clamps negative values (handles no overflow).
// 2. std::min(1.0f, ...) clamps positive values (prevents overflow).
// 3. Multiply by 32767.0f (not 32768.0f) — this is critical.
// 4. Cast to int16_t — this truncates the float to integer.
//
// Why 32767? int16_t ranges from -32768 to +32767 (asymmetric
// in two's complement). If we scaled to 32768, a float of 1.0
// would produce 32768, which overflows to -32768 (a loud,
// distorted sample). Using 32767 avoids this well-known gotcha.
```

This comment explains the *why* (asymmetric two's complement range),
the *what* (clamping and scaling steps), and the *gotcha* (32767 vs
32768) that a beginner would need to know.

## Verification Checklist

When reviewing new code for literate programming compliance, check:

- [ ] File has a `@file` / `@brief` narrative explaining what it does and why
- [ ] Header files have `@section` blocks for domain-specific decisions
- [ ] Every public method in header files has `@param` and `@return`
- [ ] Critical decisions have `@note` explaining gotchas and pitfalls
- [ ] No trivial self-explanatory code is commented (e.g., `i++`)

